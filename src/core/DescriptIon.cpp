#include "DescriptIon.h"
#include "Config.h"
#include "Strings.h"

#include <QDir>
#include <QFile>
#include <QStringConverter>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

DescriptIon::DescriptIon(const QString& folder) : folder_(folder), dirty_(false)
{
}

// descript.ion 的完整路径
QString DescriptIon::filePath() const
{
    return QDir(folder_).absoluteFilePath(QStringLiteral("descript.ion"));
}

// 读取:文件不存在不算错;编码不对直接报错(请检查"OEM 转换"选项)
bool DescriptIon::load(QString* error)
{
    map_.clear();
    nameCase_.clear();
    dirty_ = false;
    QFile f(filePath());
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = S("err.readFile").arg(filePath()).arg(f.errorString());
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();
    const bool oem = Config::instance().getBool(QStringLiteral("Archive/DescriptOem"));
    QStringDecoder dec(oem ? QStringConverter::System : QStringConverter::Utf8);
    const QString text = dec.decode(raw);
    if (dec.hasError()) {
        if (error != nullptr) *error = S("err.readFile").arg(filePath()).arg(S("err.descriptEncoding"));
        return false;
    }
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.trimmed().isEmpty()) continue;
        QString name;
        QString rest;
        if (line.startsWith(QLatin1Char('"'))) {
            const int close = line.indexOf(QLatin1Char('"'), 1);
            if (close < 0) {
                if (error != nullptr) *error = S("err.configLine").arg(filePath()).arg(i + 1).arg(line);
                return false;
            }
            name = line.mid(1, close - 1);
            rest = line.mid(close + 1);
        } else {
            int sp = 0;
            while (sp < line.size() && !line.at(sp).isSpace()) ++sp;
            name = line.left(sp);
            rest = line.mid(sp);
        }
        const QString desc = rest.trimmed();
        if (name.isEmpty() || desc.isEmpty()) continue;
        map_.insert(name.toLower(), desc);
        nameCase_.insert(name.toLower(), name);
    }
    return true;
}

// 取某文件的描述
QString DescriptIon::get(const QString& fileName) const
{
    return map_.value(fileName.toLower());
}

// 设置描述(空表示删除)
void DescriptIon::set(const QString& fileName, const QString& text)
{
    const QString key = fileName.toLower();
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (map_.remove(key) > 0) {
            nameCase_.remove(key);
            dirty_ = true;
        }
        return;
    }
    if (map_.value(key) != t) {
        map_.insert(key, t);
        nameCase_.insert(key, fileName);
        dirty_ = true;
    }
}

// 写回文件:全部描述为空时删除文件;按"OEM 转换""隐藏属性"选项处理
bool DescriptIon::save(QString* error)
{
    if (!dirty_) return true;
    const QString path = filePath();
#ifdef Q_OS_WIN
    // 已有隐藏属性的文件不能直接覆盖写,先清掉属性
    if (QFile::exists(path)) {
        SetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_ATTRIBUTE_NORMAL);
    }
#endif
    if (map_.isEmpty()) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            if (error != nullptr) *error = S("err.writeFile").arg(path).arg(QFile(path).errorString());
            return false;
        }
        dirty_ = false;
        return true;
    }
    QString text;
    QMap<QString, QString>::const_iterator it = map_.constBegin();
    for (; it != map_.constEnd(); ++it) {
        QString name = nameCase_.value(it.key());
        if (name.contains(QLatin1Char(' '))) name = QLatin1Char('"') + name + QLatin1Char('"');
        text += name + QLatin1Char(' ') + it.value() + QStringLiteral("\r\n");
    }
    const bool oem = Config::instance().getBool(QStringLiteral("Archive/DescriptOem"));
    QStringEncoder enc(oem ? QStringConverter::System : QStringConverter::Utf8);
    const QByteArray bytes = enc.encode(text);
    if (enc.hasError()) {
        if (error != nullptr) *error = S("err.writeFile").arg(path).arg(S("err.descriptEncoding"));
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = S("err.writeFile").arg(path).arg(f.errorString());
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        if (error != nullptr) *error = S("err.writeFile").arg(path).arg(f.errorString());
        return false;
    }
    f.close();
#ifdef Q_OS_WIN
    if (Config::instance().getBool(QStringLiteral("Archive/DescriptHidden"))) {
        SetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_ATTRIBUTE_HIDDEN);
    }
#endif
    dirty_ = false;
    return true;
}

// 是否被修改过
bool DescriptIon::isDirty() const
{
    return dirty_;
}
