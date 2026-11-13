#include "CustomCommands.h"
#include "Config.h"
#include "Commands.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

CustomCommands& CustomCommands::instance()
{
    static CustomCommands inst;
    return inst;
}

QString CustomCommands::path()
{
    return Config::appDir() + QStringLiteral("/custom_commands.ini");
}

bool CustomCommands::has(const QString& id) const
{
    for (int i = 0; i < items_.size(); ++i) if (items_.at(i).id == id) return true;
    return false;
}

CustomCommands::Item CustomCommands::item(const QString& id) const
{
    for (int i = 0; i < items_.size(); ++i) if (items_.at(i).id == id) return items_.at(i);
    Config::fatal(QStringLiteral("内部错误:自定义命令 %1 不存在").arg(id));
    return Item();
}

// 读取并注册
bool CustomCommands::load(QString* error)
{
    items_.clear();
    QFile f(path());
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取 %1:%2").arg(path()).arg(f.errorString());
        return false;
    }
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    const QStringList lines = text.split(QLatin1Char('\n'));

    const QStringList fields = QStringList() << QStringLiteral("Name") << QStringLiteral("Program")
        << QStringLiteral("Args") << QStringLiteral("Scope") << QStringLiteral("Icon") << QStringLiteral("Keys");
    struct Sec { QString id; int line; QMap<QString, QString> vals; };
    QList<Sec> secs;
    const QString where = path();

    for (int i = 0; i < lines.size(); ++i) {
        QString raw = lines.at(i);
        if (raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
        const QString tr = raw.trimmed();
        if (tr.isEmpty() || tr.startsWith(QLatin1Char(';')) || tr.startsWith(QLatin1Char('#'))) continue;
        if (tr.startsWith(QLatin1Char('[')) && tr.endsWith(QLatin1Char(']'))) {
            const QString name = tr.mid(1, tr.size() - 2).trimmed();
            if (!name.startsWith(QLatin1String("Custom.")) || name.size() <= 7) {
                *error = QStringLiteral("%1 第%2行:段名必须写成 [Custom.命令ID]").arg(where).arg(i + 1);
                return false;
            }
            Sec s;
            s.id = name.mid(7);
            s.line = i + 1;
            for (int k = 0; k < secs.size(); ++k) {
                if (secs.at(k).id == s.id) {
                    *error = QStringLiteral("%1 第%2行:命令 %3 重复定义(第%4行已定义)").arg(where).arg(i + 1).arg(s.id).arg(secs.at(k).line);
                    return false;
                }
            }
            secs.append(s);
            continue;
        }
        const int eq = raw.indexOf(QLatin1Char('='));
        if (eq <= 0 || secs.isEmpty()) {
            *error = QStringLiteral("%1 第%2行:这一行不是“项目=值”,也不是 [段名]").arg(where).arg(i + 1);
            return false;
        }
        const QString key = raw.left(eq).trimmed();
        if (!fields.contains(key)) {
            *error = QStringLiteral("%1 第%2行:不认识的项目 %3(可用:%4)").arg(where).arg(i + 1).arg(key).arg(fields.join(QLatin1Char(' ')));
            return false;
        }
        if (secs.last().vals.contains(key)) {
            *error = QStringLiteral("%1 第%2行:命令 %3 的项目 %4 写了两次").arg(where).arg(i + 1).arg(secs.last().id).arg(key);
            return false;
        }
        secs.last().vals.insert(key, raw.mid(eq + 1).trimmed());
    }

    const QRegularExpression idRe(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    const QRegularExpression phRe(QStringLiteral("\\{[^{}]*\\}"));
    Commands& cmds = Commands::instance();
    for (int s = 0; s < secs.size(); ++s) {
        const Sec& sec = secs.at(s);
        const QString at = QStringLiteral("%1 第%2行 [Custom.%3]").arg(where).arg(sec.line).arg(sec.id);
        if (!idRe.match(sec.id).hasMatch()) {
            *error = at + QStringLiteral(":命令 ID 只能用字母、数字、下划线,并且以字母开头");
            return false;
        }
        if (cmds.has(sec.id)) {
            *error = at + QStringLiteral(":命令 ID 和已有命令重名");
            return false;
        }
        for (int k = 0; k < fields.size(); ++k) {
            if (!sec.vals.contains(fields.at(k))) {
                *error = at + QStringLiteral(":缺少项目 %1(6 项都要写,Icon 和 Keys 可以留空)").arg(fields.at(k));
                return false;
            }
        }
        Item it;
        it.id = sec.id;
        it.name = sec.vals.value(QStringLiteral("Name"));
        it.program = sec.vals.value(QStringLiteral("Program"));
        it.args = sec.vals.value(QStringLiteral("Args"));
        it.scope = sec.vals.value(QStringLiteral("Scope"));
        it.icon = sec.vals.value(QStringLiteral("Icon"));
        it.keys = sec.vals.value(QStringLiteral("Keys"));
        if (it.name.isEmpty()) { *error = at + QStringLiteral(":Name 不能为空"); return false; }
        if (it.program.isEmpty()) { *error = at + QStringLiteral(":Program 不能为空"); return false; }
        if (it.scope != QLatin1String("Browser") && it.scope != QLatin1String("Viewer") && it.scope != QLatin1String("Both")) {
            *error = at + QStringLiteral(":Scope 必须是 Browser、Viewer、Both 之一,现在是“%1”").arg(it.scope);
            return false;
        }
        QRegularExpressionMatchIterator mi = phRe.globalMatch(it.args);
        while (mi.hasNext()) {
            const QString tok = mi.next().captured(0);
            if (tok != QLatin1String("{file}") && tok != QLatin1String("{folder}") && tok != QLatin1String("{selected}")) {
                *error = at + QStringLiteral(":Args 里的 %1 不是可用占位符(可用 {file} {folder} {selected})").arg(tok);
                return false;
            }
        }
        if (!it.icon.isEmpty()) {
            if (!it.icon.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
                *error = at + QStringLiteral(":Icon 要写成 xxx.svg 这样的文件名,或者留空");
                return false;
            }
            const bool ext = QFile::exists(Config::appDir() + QStringLiteral("/icons/") + it.icon);
            const bool inner = QFile::exists(QStringLiteral(":/icons/") + it.icon);
            if (!ext && !inner) {
                *error = at + QStringLiteral(":找不到图标 %1(放进 exe 旁边的 icons 文件夹,或用内置图标名)").arg(it.icon);
                return false;
            }
        }
        // {selected} 要单独作为一个参数(会展开成多个文件)
        const QStringList toks = QProcess::splitCommand(it.args);
        for (int k = 0; k < toks.size(); ++k) {
            if (toks.at(k).contains(QLatin1String("{selected}")) && toks.at(k) != QLatin1String("{selected}")) {
                *error = at + QStringLiteral(":{selected} 必须单独作为一个参数,不能和别的文字写在同一个参数里");
                return false;
            }
        }
        items_.append(it);

        CommandDef d;
        d.id = it.id;
        d.nameKey = QString();
        d.defaultKeys = it.keys;
        d.iconFile = it.icon;
        d.checkable = false;
        d.scope = (it.scope == QLatin1String("Browser")) ? CmdScope::Browser
                  : (it.scope == QLatin1String("Viewer")) ? CmdScope::Viewer : CmdScope::Both;
        d.custom = true;
        d.displayName = it.name;
        cmds.registerCommand(d);
    }
    return true;
}

// 展开成 程序 + 参数
bool CustomCommands::build(const QString& id, const QString& file, const QString& folder,
                           const QStringList& selected, QString* program, QStringList* args, QString* error) const
{
    const Item it = item(id);
    *program = it.program;
    args->clear();
    const QStringList toks = QProcess::splitCommand(it.args);
    for (int i = 0; i < toks.size(); ++i) {
        QString t = toks.at(i);
        if (t == QLatin1String("{selected}")) {
            if (selected.isEmpty()) {
                *error = QStringLiteral("命令“%1”的参数用了 {selected},但现在没有选中任何文件").arg(it.name);
                return false;
            }
            for (int k = 0; k < selected.size(); ++k) args->append(QDir::toNativeSeparators(selected.at(k)));
            continue;
        }
        if (t.contains(QLatin1String("{file}"))) {
            if (file.isEmpty()) {
                *error = QStringLiteral("命令“%1”的参数用了 {file},但现在没有当前图片/文件").arg(it.name);
                return false;
            }
            t.replace(QLatin1String("{file}"), QDir::toNativeSeparators(file));
        }
        if (t.contains(QLatin1String("{folder}"))) {
            if (folder.isEmpty()) {
                *error = QStringLiteral("命令“%1”的参数用了 {folder},但现在没有当前文件夹").arg(it.name);
                return false;
            }
            t.replace(QLatin1String("{folder}"), QDir::toNativeSeparators(folder));
        }
        args->append(t);
    }
    return true;
}
