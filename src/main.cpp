#include <QApplication>
#include <QStringList>
#include <QString>

#include "MainWindow.h"
#include "core/Config.h"
#include "core/Strings.h"
#include "core/State.h"
#include "core/Commands.h"
#include "core/ImageLoader.h"
#include "core/SingleInstance.h"

// 程序入口:启动顺序见 PROGRESS.md 第 9 条
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("QuickView"));

    QString error;
    if (!Strings::instance().load(&error)) {
        Config::fatal(error);
    }
    if (!Config::instance().ensureFileExists(&error)) {
        Config::fatal(error);
    }
    if (!Config::instance().load(&error)) {
        Config::fatal(error);
    }
    State::instance().load();

    // 命令行路径;--new-window 表示本进程是\"新窗口\"打开方式启动的,不转交给已有实例
    QStringList paths = app.arguments();
    paths.removeFirst();
    bool newWindow = false;
    if (!paths.isEmpty() && paths.first() == QLatin1String("--new-window")) {
        newWindow = true;
        paths.removeFirst();
    }

    SingleInstance single(&app);
    if (!newWindow) {
        if (single.sendToRunning(paths)) {
            return 0;
        }
        if (!single.listen(&error)) {
            Config::fatal(S("err.startup").arg(error));
        }
    }

    Commands::instance().registerDefaults();
    Commands::instance().createActions(&app);
    ImageLoader::instance().registerDefaults();

    MainWindow window(nullptr);
    QObject::connect(&single, &SingleInstance::pathsReceived, &window,
                     [&window](const QStringList& p) { window.openExternalPaths(p, false); });
    window.startWith(paths);
    return app.exec();
}
