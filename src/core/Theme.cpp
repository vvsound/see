#include "Theme.h"
#include "Config.h"

#include <QApplication>
#include <QFile>

// theme\current.qss 的完整路径
QString Theme::path()
{
    return Config::appDir() + QStringLiteral("/theme/current.qss");
}

// 读取样式表文本
bool Theme::read(QString* qss, QString* error)
{
    QFile f(path());
    if (!f.exists()) {
        *error = QStringLiteral("找不到主题文件 %1。请用 MoonManager 的“主题”页应用一个主题,再“部署”。").arg(path());
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取主题文件 %1:%2").arg(path()).arg(f.errorString());
        return false;
    }
    *qss = QString::fromUtf8(f.readAll());
    return true;
}

// 读取并应用
bool Theme::apply(QString* error)
{
    QString qss;
    if (!read(&qss, error)) return false;
    qApp->setStyleSheet(qss);
    return true;
}
