#include "ExifReader.h"
#include "Strings.h"

#include <QFile>
#include <QByteArray>
#include <QtGlobal>
#include <cmath>

namespace {

// 带边界检查的字节读取(整个 EXIF 数据在内存里,越界直接返回 false)
struct Buf {
    const uchar* p;
    qint64 n;
    bool big;  // 大端(MM)还是小端(II)
    bool u16(qint64 off, quint32* v) const
    {
        if (off < 0 || off + 2 > n) return false;
        *v = big ? ((p[off] << 8) | p[off + 1]) : ((p[off + 1] << 8) | p[off]);
        return true;
    }
    bool u32(qint64 off, quint32* v) const
    {
        if (off < 0 || off + 4 > n) return false;
        *v = big ? ((quint32(p[off]) << 24) | (quint32(p[off + 1]) << 16) | (quint32(p[off + 2]) << 8) | p[off + 3])
                 : ((quint32(p[off + 3]) << 24) | (quint32(p[off + 2]) << 16) | (quint32(p[off + 1]) << 8) | p[off]);
        return true;
    }
};

// 一个 IFD 条目
struct Entry {
    quint32 tag, type, count;
    qint64 dataOff;  // 数据位置(<=4 字节时指向条目内的值域)
};

int typeSize(quint32 t)
{
    switch (t) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    }
    return 0;
}

// 读 IFD 的全部条目;结构不对返回 false
bool readIfd(const Buf& b, qint64 ifdOff, QList<Entry>* out, QString* why)
{
    quint32 cnt = 0;
    if (!b.u16(ifdOff, &cnt)) { *why = S("err.exifTruncated"); return false; }
    for (quint32 i = 0; i < cnt; ++i) {
        const qint64 e = ifdOff + 2 + qint64(i) * 12;
        Entry en;
        if (!b.u16(e, &en.tag) || !b.u16(e + 2, &en.type) || !b.u32(e + 4, &en.count)) {
            *why = S("err.exifTruncated");
            return false;
        }
        const int ts = typeSize(en.type);
        const qint64 total = qint64(ts) * en.count;
        if (ts == 0) continue;  // 不认识的类型,跳过
        if (total <= 4) {
            en.dataOff = e + 8;
        } else {
            quint32 off = 0;
            if (!b.u32(e + 8, &off) || qint64(off) + total > b.n) { *why = S("err.exifTruncated"); return false; }
            en.dataOff = off;
        }
        out->append(en);
    }
    return true;
}

QString asciiOf(const Buf& b, const Entry& e)
{
    QByteArray raw(reinterpret_cast<const char*>(b.p + e.dataOff), int(e.count));
    const int z = raw.indexOf('\0');
    if (z >= 0) raw.truncate(z);
    return QString::fromLatin1(raw).trimmed();
}

bool rationalOf(const Buf& b, const Entry& e, bool isSigned, double* v)
{
    quint32 num = 0, den = 0;
    if (e.count < 1 || !b.u32(e.dataOff, &num) || !b.u32(e.dataOff + 4, &den) || den == 0) return false;
    *v = (isSigned ? double(qint32(num)) : double(num)) / double(den);
    return true;
}

QString trimNum(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    }
    return s;
}

// 把一个 IFD 里关心的标签放进 values
void collect(const Buf& b, const QList<Entry>& es, QMap<QString, QString>* values)
{
    for (int i = 0; i < es.size(); ++i) {
        const Entry& e = es.at(i);
        double d = 0;
        quint32 u = 0;
        switch (e.tag) {
        case 0x010F: values->insert(QStringLiteral("Make"), asciiOf(b, e)); break;
        case 0x0110: values->insert(QStringLiteral("Model"), asciiOf(b, e)); break;
        case 0x0131: values->insert(QStringLiteral("Software"), asciiOf(b, e)); break;
        case 0xA434: values->insert(QStringLiteral("LensModel"), asciiOf(b, e)); break;
        case 0x9003: values->insert(QStringLiteral("DateTimeOriginal"), asciiOf(b, e)); break;
        case 0x0112:
            if (e.type == 3 && b.u16(e.dataOff, &u)) values->insert(QStringLiteral("Orientation"), QString::number(u));
            break;
        case 0x8827:
            if (e.type == 3 && b.u16(e.dataOff, &u)) values->insert(QStringLiteral("ISO"), QString::number(u));
            break;
        case 0x829A:
            if (e.type == 5 && rationalOf(b, e, false, &d) && d > 0) {
                values->insert(QStringLiteral("ExposureTime"),
                               d < 1.0 ? QStringLiteral("1/%1 s").arg(qRound(1.0 / d)) : QStringLiteral("%1 s").arg(trimNum(d, 1)));
            }
            break;
        case 0x829D:
            if (e.type == 5 && rationalOf(b, e, false, &d)) values->insert(QStringLiteral("FNumber"), QStringLiteral("f/%1").arg(trimNum(d, 1)));
            break;
        case 0x920A:
            if (e.type == 5 && rationalOf(b, e, false, &d)) values->insert(QStringLiteral("FocalLength"), QStringLiteral("%1 mm").arg(trimNum(d, 1)));
            break;
        case 0x9204:
            if (e.type == 10 && rationalOf(b, e, true, &d)) {
                values->insert(QStringLiteral("ExposureBias"), QStringLiteral("%1%2 EV").arg(d > 0 ? QStringLiteral("+") : QString()).arg(trimNum(d, 2)));
            }
            break;
        }
    }
}

} // namespace

ExifResult readExif(const QString& path)
{
    ExifResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.message = S("err.readFile").arg(path).arg(f.errorString());
        return r;
    }
    const QByteArray head = f.read(2);
    if (head.size() < 2 || uchar(head[0]) != 0xFF || uchar(head[1]) != 0xD8) {
        r.status = ExifStatus::Unsupported;
        return r;
    }
    // 逐段找 APP1(Exif)
    for (;;) {
        QByteArray m = f.read(1);
        if (m.isEmpty()) { r.status = ExifStatus::NoExif; return r; }
        if (uchar(m[0]) != 0xFF) {
            r.message = S("err.exifBadMarker");
            return r;
        }
        int marker = 0xFF;
        while (marker == 0xFF) {
            const QByteArray c = f.read(1);
            if (c.isEmpty()) { r.status = ExifStatus::NoExif; return r; }
            marker = uchar(c[0]);
        }
        if (marker == 0xD9 || marker == 0xDA) { r.status = ExifStatus::NoExif; return r; }  // 图像数据开始,不再有头部段
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;                // 无长度的标记
        const QByteArray lenb = f.read(2);
        if (lenb.size() < 2) { r.message = S("err.exifTruncated"); return r; }
        const int len = (uchar(lenb[0]) << 8) | uchar(lenb[1]);
        if (len < 2) { r.message = S("err.exifBadMarker"); return r; }
        if (marker != 0xE1) {
            if (!f.seek(f.pos() + (len - 2))) { r.message = S("err.exifTruncated"); return r; }
            continue;
        }
        const QByteArray seg = f.read(len - 2);
        if (seg.size() < len - 2) { r.message = S("err.exifTruncated"); return r; }
        if (seg.size() < 6 || seg.left(6) != QByteArray("Exif\0\0", 6)) continue;  // 别的 APP1(如 XMP)
        const QByteArray tiff = seg.mid(6);
        Buf b;
        b.p = reinterpret_cast<const uchar*>(tiff.constData());
        b.n = tiff.size();
        if (b.n < 8) { r.message = S("err.exifTruncated"); return r; }
        if (tiff.left(2) == "II") b.big = false;
        else if (tiff.left(2) == "MM") b.big = true;
        else { r.message = S("err.exifBadHeader"); return r; }
        quint32 magic = 0, ifd0 = 0;
        if (!b.u16(2, &magic) || magic != 42 || !b.u32(4, &ifd0)) { r.message = S("err.exifBadHeader"); return r; }
        QList<Entry> es;
        QString why;
        if (!readIfd(b, ifd0, &es, &why)) { r.message = why; return r; }
        collect(b, es, &r.values);
        for (int i = 0; i < es.size(); ++i) {
            if (es.at(i).tag != 0x8769) continue;  // 指向 Exif 子 IFD
            quint32 off = 0;
            if (!b.u32(es.at(i).dataOff, &off)) { r.message = S("err.exifTruncated"); return r; }
            QList<Entry> sub;
            if (!readIfd(b, off, &sub, &why)) { r.message = why; return r; }
            collect(b, sub, &r.values);
        }
        r.status = ExifStatus::Ok;
        return r;
    }
}
