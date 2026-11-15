#pragma once
#include <QString>
#include <QStringList>

// 文件关联:默认写当前用户注册表 HKCU(不需要管理员权限);可选再写 HKLM(本机所有用户,需要管理员)
class FileAssoc {
public:
    // 把扩展名列表全部关联到本程序;失败写 error
    static bool registerAll(const QStringList& extensions, QString* error);
    // 取消本程序对这些扩展名的关联
    static bool unregisterAll(const QStringList& extensions, QString* error);
    // 之后的 registerAll / unregisterAll 改写本机 HKLM(需要管理员权限);默认写当前用户 HKCU
    static void setMachineWide(bool on);
    // 弹出管理员确认(UAC),用提权的自己再启动一次(参数 --assoc-machine)把关联写进 HKLM;用户取消或失败返回 false 并写 error
    static bool registerAllUsers(QString* error);
    // 清除当前用户在系统里为这些扩展名选过的"默认应用"(UserChoice),否则它会压住我们的注册;返回成功清除的个数
    static int clearUserChoice(const QStringList& extensions);
    // 打开系统"设置 → 默认应用",并定位到本程序
    static void openDefaultAppsSettings();
    // 通知资源管理器刷新图标和关联
    static void notifyShell();
};
