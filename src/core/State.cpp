#include "State.h"
#include "Config.h"
#include "Strings.h"

#include <QFile>
#include <QStringList>

State::State() {}

// 全局唯一实例
State& State::instance()
{
    static State inst;
    return inst;
}

// QuickView.state 的完整路径
QString State::statePath()
{
    return Config::appDir() + QStringLiteral("/QuickView.state");
}

// 读取状态文件(不存在不算错;内容有错直接报错)
void State::load()
{
    values_.clear();
    QFile f(statePath());
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly)) {
        Config::fatal(S("err.readFile").arg(statePath()).arg(f.errorString()));
    }
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.trimmed().isEmpty()) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            Config::fatal(S("err.stateFile").arg(statePath()).arg(S("err.badLine")));
        }
        values_.insert(line.left(eq), line.mid(eq + 1));
    }
}

// 保存到磁盘
void State::save()
{
    QFile f(statePath());
    if (!f.open(QIODevice::WriteOnly)) {
        Config::fatal(S("err.writeFile").arg(statePath()).arg(f.errorString()));
    }
    QMap<QString, QString>::const_iterator it;
    for (it = values_.constBegin(); it != values_.constEnd(); ++it) {
        f.write((it.key() + QLatin1Char('=') + it.value() + QLatin1Char('\n')).toUtf8());
    }
}

// 读字符串
QString State::getString(const QString& key, const QString& def) const
{
    QMap<QString, QString>::const_iterator it = values_.constFind(key);
    return it == values_.constEnd() ? def : it.value();
}

// 读整数
int State::getInt(const QString& key, int def) const
{
    bool ok = false;
    const int v = getString(key, QString()).toInt(&ok);
    return ok ? v : def;
}

// 读布尔
bool State::getBool(const QString& key, bool def) const
{
    const QString v = getString(key, QString());
    if (v.isEmpty()) return def;
    return v == QLatin1String("1");
}

// 读列表(用 | 分隔,因为路径里不会出现)
QStringList State::getList(const QString& key) const
{
    const QString v = getString(key, QString());
    if (v.isEmpty()) return QStringList();
    return v.split(QLatin1Char('|'));
}

// 写字符串并保存
void State::setString(const QString& key, const QString& value)
{
    values_.insert(key, value);
    save();
}

// 写整数
void State::setInt(const QString& key, int value)
{
    setString(key, QString::number(value));
}

// 写布尔
void State::setBool(const QString& key, bool value)
{
    setString(key, value ? QStringLiteral("1") : QStringLiteral("0"));
}

// 写列表
void State::setList(const QString& key, const QStringList& value)
{
    setString(key, value.join(QLatin1Char('|')));
}
