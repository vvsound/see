#include "Config.h"
#include "Strings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QMessageBox>
#include <QTextStream>
#include <cstdlib>

Config::Config() : loaded_(false) {}

// 全局唯一实例
Config& Config::instance()
{
    static Config inst;
    return inst;
}

// exe 所在文件夹
QString Config::appDir()
{
    return QCoreApplication::applicationDirPath();
}

// QuickView.ini 的完整路径
QString Config::iniPath()
{
    return appDir() + QStringLiteral("/QuickView.ini");
}

// 显示错误对话框并立刻退出程序
void Config::fatal(const QString& message)
{
    if (qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr) {
        QMessageBox::critical(nullptr, QStringLiteral("QuickView"), message);
    } else {
        qCritical("%s", qPrintable(message));
    }
    std::exit(1);
}

// 解析文本为行列表
bool Config::parseText(const QString& text, const QString& sourceName, QList<Line>* out, QString* error) const
{
    QString t = text;
    if (t.startsWith(QChar(0xFEFF))) t.remove(0, 1);
    const QStringList raws = t.split(QLatin1Char('\n'));
    QString section;
    for (int i = 0; i < raws.size(); ++i) {
        QString raw = raws.at(i);
        if (raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
        if (i == raws.size() - 1 && raw.isEmpty()) break; // 末尾换行产生的空串
        Line l;
        l.raw = raw;
        l.section = section;
        l.isEntry = false;
        const QString tr = raw.trimmed();
        if (tr.isEmpty() || tr.startsWith(QLatin1Char(';')) || tr.startsWith(QLatin1Char('#'))) {
            // 空行/注释
        } else if (tr.startsWith(QLatin1Char('[')) && tr.endsWith(QLatin1Char(']'))) {
            section = tr.mid(1, tr.size() - 2).trimmed();
            l.section = section;
        } else {
            const int eq = raw.indexOf(QLatin1Char('='));
            if (eq <= 0 || section.isEmpty()) {
                *error = S("err.configLine").arg(sourceName).arg(i + 1).arg(S("err.badLine"));
                return false;
            }
            l.key = raw.left(eq).trimmed();
            l.value = raw.mid(eq + 1).trimmed();
            l.isEntry = true;
        }
        out->append(l);
    }
    return true;
}

// 解析内置模板
bool Config::loadTemplate(QString* error)
{
    if (!metas_.isEmpty()) return true;
    QFile f(QStringLiteral(":/QuickView.default.ini"));
    if (!f.open(QIODevice::ReadOnly)) {
        *error = S("err.templateBroken").arg(f.errorString());
        return false;
    }
    templateText_ = QString::fromUtf8(f.readAll());
    QList<Line> lines;
    if (!parseText(templateText_, QStringLiteral("QuickView.default.ini"), &lines, error)) return false;

    QString pendingComment;
    QString pendingType;
    QString pendingExtra;
    for (int i = 0; i < lines.size(); ++i) {
        const Line& l = lines.at(i);
        const QString tr = l.raw.trimmed();
        if (tr.startsWith(QLatin1String(";@"))) {
            const QString body = tr.mid(2).trimmed();
            const int sp = body.indexOf(QLatin1Char(' '));
            pendingType = (sp < 0) ? body : body.left(sp);
            pendingExtra = (sp < 0) ? QString() : body.mid(sp + 1).trimmed();
        } else if (tr.startsWith(QLatin1Char(';'))) {
            pendingComment = tr.mid(1).trimmed();
        } else if (l.isEntry) {
            if (pendingType.isEmpty()) {
                *error = S("err.templateBroken").arg(l.section + QLatin1Char('/') + l.key);
                return false;
            }
            ConfigKeyMeta m;
            m.key = l.section + QLatin1Char('/') + l.key;
            m.type = pendingType;
            m.defaultValue = l.value;
            m.comment = pendingComment;
            m.minValue = 0;
            m.maxValue = 0;
            m.hasRange = false;
            if (pendingType == QLatin1String("enum")) {
                m.enumValues = pendingExtra.split(QLatin1Char('|'));
            } else if ((pendingType == QLatin1String("int") || pendingType == QLatin1String("double")) && !pendingExtra.isEmpty()) {
                const QStringList r = pendingExtra.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (r.size() == 2) {
                    m.minValue = r.at(0).toDouble();
                    m.maxValue = r.at(1).toDouble();
                    m.hasRange = true;
                }
            }
            metaIndex_.insert(m.key, metas_.size());
            metas_.append(m);
            pendingComment.clear();
            pendingType.clear();
            pendingExtra.clear();
        }
    }
    return true;
}

// 首次运行:ini 不存在则生成
bool Config::ensureFileExists(QString* error)
{
    if (!loadTemplate(error)) return false;
    if (QFile::exists(iniPath())) return true;
    QFile f(iniPath());
    if (!f.open(QIODevice::WriteOnly)) {
        *error = S("err.createIni").arg(iniPath()).arg(f.errorString());
        return false;
    }
    f.write(templateText_.toUtf8());
    return true;
}

// 校验单个值
bool Config::validateValue(const ConfigKeyMeta& m, const QString& value, QString* why) const
{
    if (m.type == QLatin1String("bool")) {
        if (value != QLatin1String("0") && value != QLatin1String("1")) { *why = S("err.valueBool"); return false; }
    } else if (m.type == QLatin1String("int")) {
        bool ok = false;
        const int v = value.toInt(&ok);
        if (!ok) { *why = S("err.valueInt"); return false; }
        if (m.hasRange && (v < m.minValue || v > m.maxValue)) {
            *why = S("err.valueRange").arg(m.minValue).arg(m.maxValue);
            return false;
        }
    } else if (m.type == QLatin1String("double")) {
        bool ok = false;
        const double v = value.toDouble(&ok);
        if (!ok) { *why = S("err.valueDouble"); return false; }
        if (m.hasRange && (v < m.minValue || v > m.maxValue)) {
            *why = S("err.valueRange").arg(m.minValue).arg(m.maxValue);
            return false;
        }
    } else if (m.type == QLatin1String("enum")) {
        if (!m.enumValues.contains(value)) {
            *why = S("err.valueEnum").arg(m.enumValues.join(QLatin1Char('/')));
            return false;
        }
    } else if (m.type == QLatin1String("color")) {
        if (!value.startsWith(QLatin1Char('#')) || !QColor(value).isValid()) { *why = S("err.valueColor"); return false; }
    }
    return true;
}

// 读取 ini 并对照模板逐项检查
bool Config::load(QString* error)
{
    if (!loadTemplate(error)) return false;
    QFile f(iniPath());
    if (!f.open(QIODevice::ReadOnly)) {
        *error = S("err.readFile").arg(iniPath()).arg(f.errorString());
        return false;
    }
    lines_.clear();
    index_.clear();
    if (!parseText(QString::fromUtf8(f.readAll()), iniPath(), &lines_, error)) return false;
    for (int i = 0; i < lines_.size(); ++i) {
        const Line& l = lines_.at(i);
        if (!l.isEntry) continue;
        const QString k = l.section + QLatin1Char('/') + l.key;
        if (index_.contains(k)) {
            *error = S("err.configLine").arg(iniPath()).arg(i + 1).arg(S("err.dupKey").arg(k));
            return false;
        }
        const ConfigKeyMeta* m = meta(k);
        if (m == nullptr) {
            *error = S("err.configLine").arg(iniPath()).arg(i + 1).arg(S("err.unknownKey").arg(k));
            return false;
        }
        QString why;
        if (!validateValue(*m, l.value, &why)) {
            *error = S("err.configLine").arg(iniPath()).arg(i + 1).arg(k + QStringLiteral(": ") + why);
            return false;
        }
        index_.insert(k, i);
    }
    for (int i = 0; i < metas_.size(); ++i) {
        if (!index_.contains(metas_.at(i).key)) {
            *error = S("err.configMissing").arg(iniPath()).arg(metas_.at(i).key);
            return false;
        }
    }
    loaded_ = true;
    return true;
}

// 取原始行(必须存在)
const Config::Line& Config::lineFor(const QString& key) const
{
    QMap<QString, int>::const_iterator it = index_.constFind(key);
    if (it == index_.constEnd()) {
        fatal(S("err.configMissing").arg(iniPath()).arg(key));
    }
    return lines_.at(it.value());
}

// 读取字符串
QString Config::getString(const QString& key) const
{
    return lineFor(key).value;
}

// 读取布尔
bool Config::getBool(const QString& key) const
{
    return lineFor(key).value == QLatin1String("1");
}

// 读取整数
int Config::getInt(const QString& key) const
{
    return lineFor(key).value.toInt();
}

// 读取小数
double Config::getDouble(const QString& key) const
{
    return lineFor(key).value.toDouble();
}

// 读取颜色
QColor Config::getColor(const QString& key) const
{
    return QColor(lineFor(key).value);
}

// 读取列表
QStringList Config::getList(const QString& key) const
{
    QStringList out;
    const QStringList parts = lineFor(key).value.split(QLatin1Char(','));
    for (int i = 0; i < parts.size(); ++i) {
        const QString p = parts.at(i).trimmed();
        if (!p.isEmpty()) out.append(p);
    }
    return out;
}

// 读取枚举
QString Config::getEnum(const QString& key) const
{
    return lineFor(key).value;
}

// 修改字符串并写回
void Config::setString(const QString& key, const QString& value)
{
    const Line& old = lineFor(key);
    (void)old;
    const ConfigKeyMeta* m = meta(key);
    if (m != nullptr) {
        QString why;
        if (!validateValue(*m, value, &why)) fatal(S("err.configType").arg(key).arg(value).arg(why));
    }
    const int i = index_.value(key);
    lines_[i].value = value;
    lines_[i].raw = lines_.at(i).key + QLatin1Char('=') + value;
    writeFile();
}

// 修改布尔
void Config::setBool(const QString& key, bool value)
{
    setString(key, value ? QStringLiteral("1") : QStringLiteral("0"));
}

// 修改整数
void Config::setInt(const QString& key, int value)
{
    setString(key, QString::number(value));
}

// 修改小数(保留两位)
void Config::setDouble(const QString& key, double value)
{
    setString(key, QString::number(value, 'f', 2));
}

// 修改列表
void Config::setList(const QString& key, const QStringList& value)
{
    setString(key, value.join(QLatin1Char(',')));
}

// 写回文件
void Config::writeFile()
{
    QStringList out;
    for (int i = 0; i < lines_.size(); ++i) out.append(lines_.at(i).raw);
    QFile f(iniPath());
    if (!f.open(QIODevice::WriteOnly)) {
        fatal(S("err.writeFile").arg(iniPath()).arg(f.errorString()));
    }
    f.write((out.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
}

// 某键的类型说明
const ConfigKeyMeta* Config::meta(const QString& key) const
{
    QMap<QString, int>::const_iterator it = metaIndex_.constFind(key);
    if (it == metaIndex_.constEnd()) return nullptr;
    return &metas_.at(it.value());
}

// 模板里所有键
QStringList Config::allKeys() const
{
    QStringList out;
    for (int i = 0; i < metas_.size(); ++i) out.append(metas_.at(i).key);
    return out;
}

// 某段里的键名
QStringList Config::keysInSection(const QString& section) const
{
    QStringList out;
    const QString prefix = section + QLatin1Char('/');
    for (int i = 0; i < metas_.size(); ++i) {
        if (metas_.at(i).key.startsWith(prefix)) out.append(metas_.at(i).key.mid(prefix.size()));
    }
    return out;
}
