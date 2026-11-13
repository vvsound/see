#include "Strings.h"
#include "Config.h"

#include <QCoreApplication>
#include <QFile>

Strings::Strings() {}

// 全局唯一实例
Strings& Strings::instance()
{
    static Strings inst;
    return inst;
}

// 解析 键名=文字 的文本;\n 换行、\\ 反斜杠
bool Strings::parse(const QString& text, const QString& sourceName, QString* error)
{
    QString t = text;
    if (t.startsWith(QChar(0xFEFF))) t.remove(0, 1);
    const QStringList lines = t.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        const QString tr = line.trimmed();
        if (tr.isEmpty() || tr.startsWith(QLatin1Char(';')) || tr.startsWith(QLatin1Char('#'))) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            // 引导阶段界面文字还没载入,这里只能直接写文字
            *error = QStringLiteral("界面文字文件 %1 第 %2 行不是 键名=文字 的格式").arg(sourceName).arg(i + 1);
            return false;
        }
        const QString key = line.left(eq).trimmed();
        const QString raw = line.mid(eq + 1);
        QString value;
        for (int k = 0; k < raw.size(); ++k) {
            const QChar c = raw.at(k);
            if (c == QLatin1Char('\\') && k + 1 < raw.size()) {
                const QChar n = raw.at(k + 1);
                if (n == QLatin1Char('n')) { value.append(QLatin1Char('\n')); ++k; continue; }
                if (n == QLatin1Char('\\')) { value.append(QLatin1Char('\\')); ++k; continue; }
            }
            value.append(c);
        }
        if (map_.contains(key)) {
            *error = QStringLiteral("界面文字文件 %1 第 %2 行:键名 %3 重复").arg(sourceName).arg(i + 1).arg(key);
            return false;
        }
        map_.insert(key, value);
    }
    return true;
}

// 载入文字表:先找 exe 旁 lang/zh-CN.ini,没有则用内置
bool Strings::load(QString* error)
{
    map_.clear();
    const QString external = Config::appDir() + QStringLiteral("/lang/zh-CN.ini");
    const QString path = QFile::exists(external) ? external : QStringLiteral(":/lang/zh-CN.ini");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取界面文字文件 %1:%2").arg(path).arg(f.errorString());
        return false;
    }
    return parse(QString::fromUtf8(f.readAll()), path, error);
}

// 取文字;找不到键名直接报错退出
QString Strings::get(const QString& key) const
{
    QMap<QString, QString>::const_iterator it = map_.constFind(key);
    if (it == map_.constEnd()) {
        Config::fatal(QStringLiteral("界面文字表缺少键名 %1").arg(key));
    }
    return it.value();
}

// 键名是否存在
bool Strings::has(const QString& key) const
{
    return map_.contains(key);
}
