#include "Commands.h"
#include "Config.h"
#include "Strings.h"

#include <QFile>
#include <QKeySequence>
#include <QKeyCombination>
#include <QMap>

Commands::Commands() : QObject(nullptr) {}

// 全局唯一实例
Commands& Commands::instance()
{
    static Commands inst;
    return inst;
}

// 注册一个命令
void Commands::registerCommand(const CommandDef& def)
{
    if (defIndex_.contains(def.id)) {
        Config::fatal(QStringLiteral("命令 ID 重复注册:%1").arg(def.id));
    }
    defIndex_.insert(def.id, defs_.size());
    defs_.append(def);
}

// 注册全部内置命令(以后加新命令就在这里加一行,并在 ini 模板 [Keys] 和 lang 里加对应项)
void Commands::registerDefaults()
{
    if (!defs_.isEmpty()) return;
    struct Row { const char* id; const char* keys; const char* icon; bool checkable; CmdScope scope; };
    const CmdScope B = CmdScope::Browser;
    const CmdScope V = CmdScope::Viewer;
    const CmdScope A = CmdScope::Both;
    const Row rows[] = {
        {"OpenFile", "Ctrl+O", "open.svg", false, A},
        {"OpenSelected", "Return,Enter", "open.svg", false, B},
        {"Exit", "Alt+F4", "exit.svg", false, A},
        {"GoUp", "Backspace", "up.svg", false, B},
        {"Refresh", "F5", "refresh.svg", false, B},
        {"NextImage", "Space,PageDown,Right,Down", "next.svg", false, V},
        {"PrevImage", "Backspace,PageUp,Left,Up", "prev.svg", false, V},
        {"FirstImage", "Home", "first.svg", false, V},
        {"LastImage", "End", "last.svg", false, V},
        {"BackToBrowser", "Esc,Return,Enter", "back.svg", false, V},
        {"ZoomIn", "+", "zoomin.svg", false, V},
        {"ZoomOut", "-", "zoomout.svg", false, V},
        {"FitWindow", "*", "fit.svg", false, V},
        {"ActualSize", "/", "actual.svg", false, V},
        {"RotateLeft", "L", "rotl.svg", false, V},
        {"RotateRight", "R", "rotr.svg", false, V},
        {"Fullscreen", "F,F11", "fullscreen.svg", true, V},
        {"Delete", "Delete", "delete.svg", false, A},
        {"Rename", "F2", "rename.svg", false, A},
        {"CopyTo", "F7", "copyto.svg", false, A},
        {"MoveTo", "F6", "moveto.svg", false, A},
        {"CopyFile", "Ctrl+C", "copy.svg", false, B},
        {"CopyImage", "Ctrl+C", "copy.svg", false, V},
        {"SelectAll", "Ctrl+A", "selectall.svg", false, B},
        {"Properties", "Alt+Return", "properties.svg", false, A},
        {"Print", "Ctrl+P", "print.svg", false, A},
        {"ViewThumbInfo", "", "view_thumbinfo.svg", true, B},
        {"ViewThumbs", "", "view_thumbs.svg", true, B},
        {"ViewIcons", "", "view_icons.svg", true, B},
        {"ViewList", "", "view_list.svg", true, B},
        {"ViewDetails", "", "view_details.svg", true, B},
        {"ToggleToolbar", "", "toggle_toolbar.svg", true, A},
        {"ToggleStatusBar", "", "toggle_statusbar.svg", true, A},
        {"ToggleMenu", "", "toggle_menu.svg", true, A},
        {"TogglePathBar", "", "toggle_pathbar.svg", true, B},
        {"ToggleTree", "", "toggle_tree.svg", true, B},
        {"TogglePreview", "", "toggle_preview.svg", true, B},
        {"AlwaysOnTop", "", "ontop.svg", true, V},
        {"Options", "", "options.svg", false, A},
        {"ManageExtensions", "", "extensions.svg", false, A},
        {"FileAssoc", "", "assoc.svg", false, A},
        {"About", "", "about.svg", false, A},
        {"CutFile", "Ctrl+X", "moveto.svg", false, B},
        {"CopyImageSel", "Ctrl+Shift+C", "copy.svg", false, B},
        {"EditDescription", "Ctrl+D", "properties.svg", false, B},
        {"ReloadImage", "Ctrl+R", "refresh.svg", false, V},
        {"SaveAs", "Ctrl+S", "copyto.svg", false, V},
        {"CloseImage", "Ctrl+Q", "back.svg", false, V},
        {"ZoomLock", "Ctrl+L", "fit.svg", true, V},
        {"RotateBy", "", "rotr.svg", false, V}
    };
    const int n = int(sizeof(rows) / sizeof(rows[0]));
    for (int i = 0; i < n; ++i) {
        CommandDef d;
        d.id = QString::fromLatin1(rows[i].id);
        d.nameKey = QStringLiteral("cmd.") + d.id;
        d.defaultKeys = QString::fromLatin1(rows[i].keys);
        d.iconFile = QString::fromLatin1(rows[i].icon);
        d.checkable = rows[i].checkable;
        d.scope = rows[i].scope;
        registerCommand(d);
    }
}

// 把按键名转成 Qt 键码;不认识返回 0
static int keyFromName(const QString& name)
{
    static QMap<QString, int> table;
    if (table.isEmpty()) {
        table.insert(QStringLiteral("space"), Qt::Key_Space);
        table.insert(QStringLiteral("return"), Qt::Key_Return);
        table.insert(QStringLiteral("enter"), Qt::Key_Enter);
        table.insert(QStringLiteral("esc"), Qt::Key_Escape);
        table.insert(QStringLiteral("escape"), Qt::Key_Escape);
        table.insert(QStringLiteral("backspace"), Qt::Key_Backspace);
        table.insert(QStringLiteral("delete"), Qt::Key_Delete);
        table.insert(QStringLiteral("del"), Qt::Key_Delete);
        table.insert(QStringLiteral("insert"), Qt::Key_Insert);
        table.insert(QStringLiteral("ins"), Qt::Key_Insert);
        table.insert(QStringLiteral("home"), Qt::Key_Home);
        table.insert(QStringLiteral("end"), Qt::Key_End);
        table.insert(QStringLiteral("pageup"), Qt::Key_PageUp);
        table.insert(QStringLiteral("pgup"), Qt::Key_PageUp);
        table.insert(QStringLiteral("pagedown"), Qt::Key_PageDown);
        table.insert(QStringLiteral("pgdown"), Qt::Key_PageDown);
        table.insert(QStringLiteral("left"), Qt::Key_Left);
        table.insert(QStringLiteral("right"), Qt::Key_Right);
        table.insert(QStringLiteral("up"), Qt::Key_Up);
        table.insert(QStringLiteral("down"), Qt::Key_Down);
        table.insert(QStringLiteral("tab"), Qt::Key_Tab);
        for (int i = 1; i <= 24; ++i) table.insert(QStringLiteral("f%1").arg(i), Qt::Key_F1 + i - 1);
    }
    const QString low = name.toLower();
    if (table.contains(low)) return table.value(low);
    if (name.size() == 1) {
        const ushort u = name.at(0).toUpper().unicode();
        if (u > 32 && u < 127) return int(u); // ASCII 字符的 Qt 键码与 ASCII 相同
    }
    return 0;
}

// 把 "Ctrl+O,F2" 解析成快捷键列表
QList<QKeySequence> Commands::parseKeys(const QString& id, const QString& text) const
{
    QList<QKeySequence> out;
    const QStringList parts = text.split(QLatin1Char(','));
    for (int i = 0; i < parts.size(); ++i) {
        const QString s = parts.at(i).trimmed();
        if (s.isEmpty()) continue;
        QString mods;
        QString keyName;
        if (s == QLatin1String("+")) {
            keyName = s;
        } else if (s.endsWith(QLatin1String("++"))) {
            keyName = QStringLiteral("+");
            mods = s.left(s.size() - 2);
        } else {
            const int p = s.lastIndexOf(QLatin1Char('+'));
            if (p < 0) {
                keyName = s;
            } else {
                mods = s.left(p);
                keyName = s.mid(p + 1);
            }
        }
        Qt::KeyboardModifiers m;
        const QStringList ml = mods.split(QLatin1Char('+'), Qt::SkipEmptyParts);
        bool bad = false;
        for (int k = 0; k < ml.size(); ++k) {
            const QString mm = ml.at(k).trimmed().toLower();
            if (mm == QLatin1String("ctrl")) m |= Qt::ControlModifier;
            else if (mm == QLatin1String("alt")) m |= Qt::AltModifier;
            else if (mm == QLatin1String("shift")) m |= Qt::ShiftModifier;
            else if (mm == QLatin1String("meta")) m |= Qt::MetaModifier;
            else bad = true;
        }
        const int key = keyFromName(keyName);
        if (bad || key == 0) {
            Config::fatal(S("err.keyInvalid").arg(id).arg(s));
        }
        out.append(QKeySequence(QKeyCombination(m, static_cast<Qt::Key>(key))));
    }
    return out;
}

// 按 [Keys] 配置创建所有 QAction
void Commands::createActions(QObject* parent)
{
    for (int i = 0; i < defs_.size(); ++i) {
        const CommandDef& d = defs_.at(i);
        if (actions_.contains(d.id)) continue;
        QAction* a = new QAction(parent);
        a->setText(S(d.nameKey));
        a->setData(d.id);
        a->setCheckable(d.checkable);
        a->setIcon(iconFor(d.id));
        a->setShortcuts(parseKeys(d.id, Config::instance().getString(QStringLiteral("Keys/") + d.id)));
        a->setShortcutContext(Qt::WindowShortcut);
        a->setIconVisibleInMenu(false);
        connect(a, SIGNAL(triggered()), this, SLOT(onActionTriggered()));
        actions_.insert(d.id, a);
    }
}

// 取 QAction
QAction* Commands::action(const QString& id) const
{
    return actions_.value(id, nullptr);
}

// 命令是否已注册
bool Commands::has(const QString& id) const
{
    return defIndex_.contains(id);
}

// 取命令定义
CommandDef Commands::def(const QString& id) const
{
    if (!defIndex_.contains(id)) {
        Config::fatal(S("err.unknownCommand").arg(id).arg(QStringLiteral("Commands::def")));
    }
    return defs_.at(defIndex_.value(id));
}

// 所有命令 ID
QStringList Commands::ids() const
{
    QStringList out;
    for (int i = 0; i < defs_.size(); ++i) out.append(defs_.at(i).id);
    return out;
}

// 设置处理函数
void Commands::setHandler(const QString& id, std::function<void()> fn)
{
    if (!defIndex_.contains(id)) {
        Config::fatal(S("err.unknownCommand").arg(id).arg(QStringLiteral("Commands::setHandler")));
    }
    handlers_.insert(id, fn);
}

// 执行命令
void Commands::trigger(const QString& id)
{
    QAction* a = action(id);
    if (a == nullptr) {
        Config::fatal(S("err.unknownCommand").arg(id).arg(QStringLiteral("Commands::trigger")));
    }
    if (!a->isEnabled()) return;
    a->trigger();
}

// 所有 QAction 触发后统一走这里
void Commands::onActionTriggered()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (a == nullptr) return;
    const QString id = a->data().toString();
    QHash<QString, std::function<void()> >::const_iterator it = handlers_.constFind(id);
    if (it == handlers_.constEnd()) {
        Config::fatal(S("err.noHandler").arg(id));
    }
    emit commandTriggered(id);
    it.value()();
}

// 按窗口模式启用/禁用命令
void Commands::setMode(WindowMode mode)
{
    for (int i = 0; i < defs_.size(); ++i) {
        const CommandDef& d = defs_.at(i);
        QAction* a = actions_.value(d.id, nullptr);
        if (a == nullptr) continue;
        bool on = (d.scope == CmdScope::Both)
                  || (d.scope == CmdScope::Browser && mode == WindowMode::Browser)
                  || (d.scope == CmdScope::Viewer && mode == WindowMode::Viewer);
        a->setEnabled(on);
    }
}

// 命令图标:先找 exe 旁 icons 文件夹,再找内置
QIcon Commands::iconFor(const QString& id) const
{
    const CommandDef d = def(id);
    if (d.iconFile.isEmpty()) return QIcon();
    const QString external = Config::appDir() + QStringLiteral("/icons/") + d.iconFile;
    if (QFile::exists(external)) return QIcon(external);
    return QIcon(QStringLiteral(":/icons/") + d.iconFile);
}

// 命令显示名
QString Commands::nameOf(const QString& id) const
{
    return S(def(id).nameKey);
}
