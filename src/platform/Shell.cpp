#include "Shell.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif

// 在资源管理器里显示并选中
bool Shell::showInExplorer(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return false;
#ifdef Q_OS_WIN
    QStringList args;
    if (!fi.isDir()) args << QStringLiteral("/select,");   // 和 Qt Creator 的做法一样:"/select," 与路径分成两个参数
    args << QDir::toNativeSeparators(fi.absoluteFilePath());
    return QProcess::startDetached(QStringLiteral("explorer.exe"), args);
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
#endif
}

// 壁纸能直接用原文件的格式
bool Shell::wallpaperDirectOk(const QString& path)
{
    const QString e = QFileInfo(path).suffix().toLower();
    return e == QLatin1String("jpg") || e == QLatin1String("jpeg") || e == QLatin1String("png") || e == QLatin1String("bmp");
}

// 设为桌面壁纸
bool Shell::setWallpaper(const QString& imagePath, QString* error)
{
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(QFileInfo(imagePath).absoluteFilePath());
    std::wstring w = native.toStdWString();
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(w.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        if (error != nullptr) *error = QStringLiteral("SystemParametersInfo failed (%1)").arg(static_cast<int>(GetLastError()));
        return false;
    }
    return true;
#else
    Q_UNUSED(imagePath);
    if (error != nullptr) *error = QStringLiteral("only supported on Windows");
    return false;
#endif
}
