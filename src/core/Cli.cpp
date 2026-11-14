#include "Cli.h"
#include "Config.h"
#include "Strings.h"
#include "Commands.h"
#include "CustomCommands.h"
#include "Theme.h"

#include <QByteArray>
#include <QFile>
#include <QImageReader>
#include <QList>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cstdio>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Cli {

// 往一个标准句柄写文字。控制台用宽字符接口,管道/文件用 UTF-8 字节
static void writeTo(bool toStdErr, const QString& text)
{
#ifdef Q_OS_WIN
    HANDLE h = GetStdHandle(toStdErr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        // 没有被重定向:尝试接到启动它的控制台
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
    }
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        const std::wstring w = text.toStdWString();
        DWORD done = 0;
        WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &done, nullptr);
    } else {
        const QByteArray u = text.toUtf8();
        DWORD done = 0;
        WriteFile(h, u.constData(), static_cast<DWORD>(u.size()), &done, nullptr);
    }
#else
    const QByteArray u = text.toUtf8();
    std::fwrite(u.constData(), 1, size_t(u.size()), toStdErr ? stderr : stdout);
    std::fflush(toStdErr ? stderr : stdout);
#endif
}

void write(const QString& text, bool toStdErr)
{
    writeTo(toStdErr, text + QLatin1Char('\n'));
}

bool isToolOption(const QString& arg)
{
    return arg == QLatin1String("--list-formats")
        || arg == QLatin1String("--check-config")
        || arg == QLatin1String("--list-commands");
}

// --list-formats:每行一个小写格式名(Qt 插件支持的 + 内置 SVG)
static int listFormats()
{
    QSet<QString> set;
    const QList<QByteArray> fmts = QImageReader::supportedImageFormats();
    for (int i = 0; i < fmts.size(); ++i) set.insert(QString::fromLatin1(fmts.at(i)).toLower());
    set.insert(QStringLiteral("svg"));
    set.insert(QStringLiteral("svgz"));
    QStringList out = set.values();
    std::sort(out.begin(), out.end());
    for (int i = 0; i < out.size(); ++i) write(out.at(i), false);
    return 0;
}

// --list-commands:每行 命令ID<TAB>适用窗口<TAB>默认键<TAB>builtin 或 custom
static int listCommands()
{
    Commands& c = Commands::instance();
    c.registerDefaults();
    QString error;
    if (!CustomCommands::instance().load(&error)) {
        write(error, true);
        return 1;
    }
    const QStringList ids = c.ids();
    for (int i = 0; i < ids.size(); ++i) {
        const CommandDef d = c.def(ids.at(i));
        write(d.id + QLatin1Char('\t') + Commands::scopeName(d.scope) + QLatin1Char('\t') + d.defaultKeys
              + QLatin1Char('\t') + (d.custom ? QStringLiteral("custom") : QStringLiteral("builtin")), false);
    }
    return 0;
}

// 检查某个配置键引用的命令是否存在;有问题往 errors 里加一条(含行号)
static void checkRef(const QString& key, const QString& id, QStringList* errors, bool allowSpecial)
{
    if (id == QLatin1String("|")) return;
    if (key.startsWith(QLatin1String("Toolbar/")) && (id == QLatin1String("@PathBar") || id == QLatin1String("@Stretch"))) return;
    if (allowSpecial && (id == QLatin1String("None") || id == QLatin1String("Pan") || id == QLatin1String("ContextMenu"))) return;
    if (Commands::instance().has(id)) return;
    errors->append(QStringLiteral("%1 第%2行 [%3]:引用了不存在的命令 %4")
                       .arg(Config::iniPath()).arg(Config::instance().lineNumber(key))
                       .arg(key).arg(id));
}

// --check-config:没问题打印 OK;有问题打印“哪个文件哪一行什么问题”
static int checkConfig()
{
    QString error;
    if (!Strings::instance().load(&error)) {
        write(QStringLiteral("界面文字文件有问题:") + error, true);
        return 1;
    }
    if (!QFile::exists(Config::iniPath())) {
        write(QStringLiteral("配置文件 %1 不存在(程序第一次正常启动时会生成;或用 MoonManager 部署)").arg(Config::iniPath()), true);
        return 1;
    }
    Config& cfg = Config::instance();
    if (!cfg.load(&error)) {
        write(error, true);
        return 1;
    }
    QStringList errors;
    QString qss;
    if (!Theme::read(&qss, &error)) errors.append(error);
    else if (!Theme::checkVars(qss, &error)) errors.append(error);

    Commands& cmds = Commands::instance();
    cmds.registerDefaults();
    if (!CustomCommands::instance().load(&error)) {
        write(error, true);
        return 1;
    }
    const QStringList ids = cmds.ids();
    for (int i = 0; i < ids.size(); ++i) {
        const CommandDef d = cmds.def(ids.at(i));
        if (d.custom) {
            cmds.parseKeys(d.id, d.defaultKeys);  // 写错会直接报错退出
            continue;
        }
        const QString lang = Strings::instance().has(d.nameKey) ? QString() : d.nameKey;
        const QString tip = QStringLiteral("tip.") + d.id;
        if (!lang.isEmpty()) errors.append(QStringLiteral("界面文字表缺少键 %1").arg(lang));
        if (!Strings::instance().has(tip)) errors.append(QStringLiteral("界面文字表缺少键 %1").arg(tip));
        cmds.parseKeys(d.id, cfg.getString(QStringLiteral("Keys/") + d.id));
    }
    const char* menus[] = {"MenuBrowser", "MenuViewer"};
    for (int m = 0; m < 2; ++m) {
        const QString sec = QString::fromLatin1(menus[m]);
        const QStringList order = cfg.getList(sec + QStringLiteral("/Order"));
        for (int i = 0; i < order.size(); ++i) {
            if (!Strings::instance().has(QStringLiteral("menu.") + order.at(i))) {
                errors.append(QStringLiteral("界面文字表缺少键 menu.%1(菜单 %2/Order 用到了它)").arg(order.at(i)).arg(sec));
            }
            const QString key = sec + QLatin1Char('/') + order.at(i);
            const QStringList items = cfg.getList(key);
            for (int k = 0; k < items.size(); ++k) checkRef(key, items.at(k), &errors, false);
        }
    }
    const char* lists[] = {"Toolbar/Browser", "Toolbar/Viewer", "ContextMenu/Browser", "ContextMenu/Viewer"};
    for (int l = 0; l < 4; ++l) {
        const QString key = QString::fromLatin1(lists[l]);
        const QStringList items = cfg.getList(key);
        for (int k = 0; k < items.size(); ++k) checkRef(key, items.at(k), &errors, false);
    }
    const QStringList mouse = cfg.keysInSection(QStringLiteral("Mouse"));
    for (int i = 0; i < mouse.size(); ++i) {
        const QString key = QStringLiteral("Mouse/") + mouse.at(i);
        checkRef(key, cfg.getString(key), &errors, true);
    }
    if (!errors.isEmpty()) {
        write(errors.join(QLatin1Char('\n')), true);
        return 1;
    }
    write(QStringLiteral("OK"), false);
    return 0;
}

int runTool(const QString& option)
{
    Config::setCliMode(true);
    if (option == QLatin1String("--list-formats")) return listFormats();
    if (option == QLatin1String("--list-commands")) return listCommands();
    if (option == QLatin1String("--check-config")) return checkConfig();
    write(QStringLiteral("未知的命令行选项:") + option, true);
    return 2;
}

} // namespace Cli
