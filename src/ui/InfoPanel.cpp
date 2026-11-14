#include "InfoPanel.h"
#include "core/Config.h"
#include "core/Strings.h"
#include "core/ImageLoader.h"
#include "core/ThumbnailCache.h"
#include "core/ExifReader.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QAction>
#include <QPainter>
#include <QPainterPath>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

namespace {

// 允许出现在 [Info] 字段列表里的字段名(写错直接报错,不悄悄忽略)
const char* const kBasicFields[] = { "Name", "Dimensions", "Format", "Size", "Modified", "Path" };
const char* const kExifFields[] = { "Camera", "Lens", "Exposure", "Aperture", "ISO", "FocalLength",
                                    "ExposureBias", "TakenDate", "Software", "Orientation" };

// 读字段列表并检查每一项都合法
QStringList fieldList(const QString& key, const char* const* allowed, int allowedCount)
{
    const QStringList list = Config::instance().getList(key);
    for (int i = 0; i < list.size(); ++i) {
        bool ok = false;
        for (int k = 0; k < allowedCount; ++k) {
            if (list.at(i) == QLatin1String(allowed[k])) ok = true;
        }
        if (!ok) Config::fatal(S("err.infoField").arg(key).arg(list.at(i)));
    }
    return list;
}

QString bytesText(qint64 bytes)
{
    if (bytes < 1024) return S("status.sizeB").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024.0) return S("status.sizeKB").arg(QString::number(kb, 'f', 1));
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return S("status.sizeMB").arg(QString::number(mb, 'f', 1));
    return S("status.sizeGB").arg(QString::number(mb / 1024.0, 'f', 1));
}

// 一行 "名称 值"(值已转义);名称颜色取调色板的次要文字色
QString row(const QWidget* w, const QString& label, const QString& value)
{
    const QString c = w->palette().color(QPalette::PlaceholderText).name();
    return QStringLiteral("<tr><td style='color:%1; padding-right:8px;' valign='top'>%2</td><td>%3</td></tr>")
        .arg(c, label.toHtmlEscaped(), value.isEmpty() ? S("info.empty").toHtmlEscaped() : value.toHtmlEscaped());
}

QString section(const QString& title)
{
    return QStringLiteral("<p style='margin:0 0 2px 0;'><b>%1</b></p>").arg(title.toHtmlEscaped());
}

} // namespace

// ---------------------------------------------------------------- 直方图

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent), peak_(0), has_(false)
{
    for (int c = 0; c < 4; ++c) for (int i = 0; i < 256; ++i) bins_[c][i] = 0;
}

void HistogramWidget::setImage(const QImage& image)
{
    for (int c = 0; c < 4; ++c) for (int i = 0; i < 256; ++i) bins_[c][i] = 0;
    peak_ = 0;
    has_ = !image.isNull();
    if (has_) {
        const QImage img = image.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
                ++bins_[0][r];
                ++bins_[1][g];
                ++bins_[2][b];
                ++bins_[3][(r * 299 + g * 587 + b * 114) / 1000];
            }
        }
        for (int c = 0; c < 4; ++c) for (int i = 0; i < 256; ++i) peak_ = qMax(peak_, bins_[c][i]);
    }
    update();
}

void HistogramWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().base());
    p.setPen(palette().mid().color());
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    if (!has_ || peak_ <= 0) return;
    const QRectF area = QRectF(rect()).adjusted(2, 2, -2, -2);
    const QColor colors[4] = { QColor(230, 70, 70, 120), QColor(70, 200, 90, 120), QColor(80, 130, 240, 120),
                               palette().text().color() };
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int c = 0; c < 4; ++c) {
        QPainterPath path;
        path.moveTo(area.left(), area.bottom());
        for (int i = 0; i < 256; ++i) {
            const qreal x = area.left() + area.width() * i / 255.0;
            const qreal y = area.bottom() - area.height() * bins_[c][i] / double(peak_);
            path.lineTo(x, y);
        }
        path.lineTo(area.right(), area.bottom());
        path.closeSubpath();
        if (c < 3) {
            p.fillPath(path, colors[c]);
        } else {
            p.setPen(colors[c]);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }
}

// ---------------------------------------------------------------- 信息面板

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent)
{
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    preview_ = new PreviewPane(this);
    basic_ = new QLabel(this);
    exif_ = new QLabel(this);
    histTitle_ = new QLabel(this);
    hist_ = new HistogramWidget(this);
    for (int i = 0; i < 2; ++i) {
        QLabel* l = (i == 0) ? basic_ : exif_;
        l->setTextFormat(Qt::RichText);
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    histTitle_->setTextFormat(Qt::RichText);
    lay->addWidget(preview_);
    lay->addWidget(basic_);
    lay->addWidget(exif_);
    lay->addWidget(histTitle_);
    lay->addWidget(hist_);
    lay->addStretch(1);
    connect(preview_, &PreviewPane::imageChanged, hist_, &HistogramWidget::setImage);
    applyOptions();
}

// 面板顶部的图标开关条
void InfoPanel::setToggleActions(const QList<QAction*>& actions)
{
    QWidget* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("infoToggles"));
    QHBoxLayout* row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    for (int i = 0; i < actions.size(); ++i) {
        if (actions.at(i) == nullptr) continue;
        QToolButton* b = new QToolButton(strip);
        b->setDefaultAction(actions.at(i));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        b->setFocusPolicy(Qt::NoFocus);
        row->addWidget(b);
    }
    row->addStretch(1);
    static_cast<QVBoxLayout*>(layout())->insertWidget(0, strip);
}

// 重新读取 [Info] 设置并刷新当前内容
void InfoPanel::applyOptions()
{
    Config& cfg = Config::instance();
    preview_->applyOptions();
    preview_->setVisible(cfg.getBool(QStringLiteral("Info/ShowPreview")));
    preview_->setMinimumHeight(cfg.getInt(QStringLiteral("Info/PreviewHeight")));
    preview_->setMaximumHeight(cfg.getInt(QStringLiteral("Info/PreviewHeight")));
    const bool hist = cfg.getBool(QStringLiteral("Info/ShowHistogram"));
    histTitle_->setText(section(S("info.section.histogram")));
    histTitle_->setVisible(hist);
    hist_->setVisible(hist);
    hist_->setFixedHeight(cfg.getInt(QStringLiteral("Info/HistogramHeight")));
    fieldList(QStringLiteral("Info/BasicFields"), kBasicFields, int(sizeof(kBasicFields) / sizeof(kBasicFields[0])));
    fieldList(QStringLiteral("Info/ExifFields"), kExifFields, int(sizeof(kExifFields) / sizeof(kExifFields[0])));
    const QString p = path_;
    path_.clear();
    showFile(p);
}

void InfoPanel::clear()
{
    path_.clear();
    preview_->clear();
    basic_->clear();
    exif_->clear();
}

void InfoPanel::showFile(const QString& path)
{
    if (path.isEmpty()) {
        clear();
        return;
    }
    if (path == path_) return;
    path_ = path;
    Config& cfg = Config::instance();
    if (cfg.getBool(QStringLiteral("Info/ShowPreview")) || cfg.getBool(QStringLiteral("Info/ShowHistogram"))) preview_->showFile(path);
    if (cfg.getBool(QStringLiteral("Info/ShowBasic"))) basic_->setText(basicHtml(path));
    else basic_->clear();
    if (cfg.getBool(QStringLiteral("Info/ShowExif"))) exif_->setText(exifHtml(path));
    else exif_->clear();
}

QString InfoPanel::basicHtml(const QString& path) const
{
    const QStringList fields = fieldList(QStringLiteral("Info/BasicFields"), kBasicFields, int(sizeof(kBasicFields) / sizeof(kBasicFields[0])));
    const QFileInfo fi(path);
    ImageInfo info = ThumbnailCache::instance().cachedInfo(path);
    QString infoErr;
    if (!info.valid && !ImageLoader::instance().readInfo(path, &info, &infoErr)) info.valid = false;
    QString html = section(S("info.section.basic")) + QStringLiteral("<table cellspacing='0' cellpadding='1'>");
    for (int i = 0; i < fields.size(); ++i) {
        const QString& f = fields.at(i);
        QString v;
        if (f == QLatin1String("Name")) v = fi.fileName();
        else if (f == QLatin1String("Path")) v = QDir::toNativeSeparators(fi.absolutePath());
        else if (f == QLatin1String("Size")) v = bytesText(fi.size());
        else if (f == QLatin1String("Modified")) v = fi.lastModified().toString(QStringLiteral("yyyy/M/d H:mm:ss"));
        else if (f == QLatin1String("Dimensions")) {
            if (info.valid) v = S("info.dimensions").arg(info.size.width()).arg(info.size.height()).arg(info.bits);
            else v = S("info.readFailed").arg(infoErr);  // 读不到就明说原因,不编造
        } else if (f == QLatin1String("Format")) {
            v = info.valid ? info.format.toUpper() : S("info.readFailed").arg(infoErr);
        }
        html += row(this, S(QStringLiteral("info.field.") + f), v);
    }
    return html + QStringLiteral("</table>");
}

QString InfoPanel::exifHtml(const QString& path) const
{
    const QStringList fields = fieldList(QStringLiteral("Info/ExifFields"), kExifFields, int(sizeof(kExifFields) / sizeof(kExifFields[0])));
    QString html = section(S("info.section.exif"));
    const ExifResult r = readExif(path);
    const QString dim = palette().color(QPalette::PlaceholderText).name();
    switch (r.status) {
    case ExifStatus::NoExif:
        return html + QStringLiteral("<span style='color:%1'>%2</span>").arg(dim, S("info.exif.none").toHtmlEscaped());
    case ExifStatus::Unsupported:
        return html + QStringLiteral("<span style='color:%1'>%2</span>").arg(dim, S("info.exif.unsupported").toHtmlEscaped());
    case ExifStatus::Error:
        return html + QStringLiteral("<span style='color:#d04040'>%1</span>").arg(S("info.exif.error").arg(r.message).toHtmlEscaped());
    case ExifStatus::Ok:
        break;
    }
    const QMap<QString, QString>& v = r.values;
    html += QStringLiteral("<table cellspacing='0' cellpadding='1'>");
    for (int i = 0; i < fields.size(); ++i) {
        const QString& f = fields.at(i);
        QString val;
        if (f == QLatin1String("Camera")) {
            QString make = v.value(QStringLiteral("Make")), model = v.value(QStringLiteral("Model"));
            val = model.startsWith(make, Qt::CaseInsensitive) ? model : (make + QLatin1Char(' ') + model).trimmed();
        } else if (f == QLatin1String("Lens")) val = v.value(QStringLiteral("LensModel"));
        else if (f == QLatin1String("Exposure")) val = v.value(QStringLiteral("ExposureTime"));
        else if (f == QLatin1String("Aperture")) val = v.value(QStringLiteral("FNumber"));
        else if (f == QLatin1String("TakenDate")) val = v.value(QStringLiteral("DateTimeOriginal"));
        else val = v.value(f);  // ISO FocalLength ExposureBias Software Orientation
        html += row(this, S(QStringLiteral("info.field.") + f), val);
    }
    return html + QStringLiteral("</table>");
}
