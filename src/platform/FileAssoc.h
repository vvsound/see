#pragma once
#include <QString>
#include <QStringList>

// 文件关联:写当前用户注册表 HKCU(不需要管理员权限)
class FileAssoc {
public:
    // 把扩展名列表全部关联到本程序;失败写 error
    static bool registerAll(const QStringList& extensions, QString* error);
    // 取消本程序对这些扩展名的关联
    static bool unregisterAll(const QStringList& extensions, QString* error);
    // 通知资源管理器刷新图标和关联
    static void notifyShell();
};
