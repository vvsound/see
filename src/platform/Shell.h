#pragma once
#include <QString>

// 系统相关的小功能:在资源管理器里显示文件、设为桌面壁纸。非 Windows 下尽量做等价的事(开发时在 Linux 上也能编译)
class Shell {
public:
    // 在资源管理器里显示并选中该文件(文件夹则直接打开);失败返回 false
    static bool showInExplorer(const QString& path);
    // 把图片设为桌面壁纸;失败写 error。imagePath 必须是 jpg/png/bmp,调用前先转好
    static bool setWallpaper(const QString& imagePath, QString* error);
    // 壁纸能直接用的扩展名(小写,不带点)
    static bool wallpaperDirectOk(const QString& path);
};
