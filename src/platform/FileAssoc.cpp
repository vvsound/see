#include "FileAssoc.h"
#include "core/Strings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const wchar_t* kProgId = L"QuickView.Image";
const wchar_t* kPrevValue = L"QuickView.PrevProgId";

bool g_machine = false;   // true:写 HKLM(本机所有用户)
HKEY rootKey() { return g_machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

// 写 <根键>\<full> 下的字符串值(valueName 为空表示默认值)
bool writeAbs(const QString& full, const QString& valueName, const QString& data, QString* error)
{
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(rootKey(), reinterpret_cast<const wchar_t*>(full.utf16()), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (error != nullptr) *error = QStringLiteral("%1 (%2)").arg(full).arg(rc);
        return false;
    }
    const wchar_t* name = valueName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(valueName.utf16());
    rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.utf16()),
                        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        if (error != nullptr) *error = QStringLiteral("%1 (%2)").arg(full).arg(rc);
        return false;
    }
    return true;
}

// 写 <根键>\Software\Classes\<subKey> 下的字符串值(valueName 为空表示默认值)
bool writeString(const QString& subKey, const QString& valueName, const QString& data, QString* error)
{
    return writeAbs(QStringLiteral("Software\\Classes\\") + subKey, valueName, data, error);
}

// 读字符串值;不存在返回空串
QString readString(const QString& subKey, const QString& valueName)
{
    const QString full = QStringLiteral("Software\\Classes\\") + subKey;
    HKEY key = nullptr;
    if (RegOpenKeyExW(rootKey(), reinterpret_cast<const wchar_t*>(full.utf16()), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return QString();
    }
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const wchar_t* name = valueName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(valueName.utf16());
    QString out;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS && type == REG_SZ) {
        out = QString::fromWCharArray(buf);
    }
    RegCloseKey(key);
    return out;
}

// 删除整棵键(不存在也算成功)
bool deleteTree(const QString& subKey, QString* error)
{
    const QString full = QStringLiteral("Software\\Classes\\") + subKey;
    const LONG rc = RegDeleteTreeW(rootKey(), reinterpret_cast<const wchar_t*>(full.utf16()));
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        if (error != nullptr) *error = QStringLiteral("%1 (%2)").arg(full).arg(rc);
        return false;
    }
    return true;
}

// 删除 HKCU\\<full> 整棵键(不存在也算成功)
void deleteTreeAbs(const QString& full)
{
    RegDeleteTreeW(rootKey(), reinterpret_cast<const wchar_t*>(full.utf16()));
}

// 删除 HKCU\\<full> 下的某个值
void deleteValueAbs(const QString& full, const QString& valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(rootKey(), reinterpret_cast<const wchar_t*>(full.utf16()), 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, reinterpret_cast<const wchar_t*>(valueName.utf16()));
        RegCloseKey(key);
    }
}

// 界面文字有就用,没有(exe 旁边的旧 lang 文件还没补上新键)就用后面的默认文字,不让程序因此退出
QString textOr(const char* key, const QString& fallback)
{
    const QString k = QString::fromLatin1(key);
    return Strings::instance().has(k) ? Strings::instance().get(k) : fallback;
}

// 删除某个值
void deleteValue(const QString& subKey, const QString& valueName)
{
    const QString full = QStringLiteral("Software\\Classes\\") + subKey;
    HKEY key = nullptr;
    if (RegOpenKeyExW(rootKey(), reinterpret_cast<const wchar_t*>(full.utf16()), 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, reinterpret_cast<const wchar_t*>(valueName.utf16()));
        RegCloseKey(key);
    }
}

} // namespace
#endif

// 把扩展名列表全部关联到本程序
bool FileAssoc::registerAll(const QStringList& extensions, QString* error)
{
#ifdef Q_OS_WIN
    const QString progId = QString::fromWCharArray(kProgId);
    const QString prevName = QString::fromWCharArray(kPrevValue);
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString exeName = QFileInfo(exe).fileName();                 // 例如 XView.exe(安装后改过名的实际文件名)
    const QString baseName = QFileInfo(exe).completeBaseName();        // 例如 XView
    const QString regName = textOr("app.regName", QStringLiteral("XView 图片查看器"));
    const QString regDesc = textOr("app.regDesc", QStringLiteral("快速浏览、查看和整理图片"));
    const QString openCmd = QStringLiteral("\"%1\" \"%2\"").arg(exe).arg(QStringLiteral("%1"));
    const QString appKey = QStringLiteral("Applications\\") + exeName;
    const QString capKey = QStringLiteral("Software\\") + baseName + QStringLiteral("\\Capabilities");
    if (!writeString(progId, QString(), S("app.name"), error)) return false;
    // 应用本身的名字和图标:“默认应用”列表、“打开方式”菜单里显示的就是这里,不写就只显示文件名 + 空白图标
    if (!writeString(appKey, QStringLiteral("FriendlyAppName"), regName, error)) return false;
    if (!writeString(appKey + QStringLiteral("\\DefaultIcon"), QString(), exe + QStringLiteral(",0"), error)) return false;
    if (!writeString(appKey + QStringLiteral("\\shell\\open\\command"), QString(), openCmd, error)) return false;
    // 能力声明:让设置 → 默认应用 里按“应用”列出本程序,带名字、说明和图标
    if (!writeAbs(capKey, QStringLiteral("ApplicationName"), regName, error)) return false;
    if (!writeAbs(capKey, QStringLiteral("ApplicationDescription"), regDesc, error)) return false;
    if (!writeAbs(capKey, QStringLiteral("ApplicationIcon"), exe + QStringLiteral(",0"), error)) return false;
    if (!writeAbs(QStringLiteral("Software\\RegisteredApplications"), baseName, QStringLiteral("Software\\") + baseName + QStringLiteral("\\Capabilities"), error)) return false;
    if (!writeString(progId + QStringLiteral("\\DefaultIcon"), QString(), exe + QStringLiteral(",0"), error)) return false;
    if (!writeString(progId + QStringLiteral("\\shell\\open\\command"), QString(),
                     QStringLiteral("\"%1\" \"%2\"").arg(exe).arg(QStringLiteral("%1")), error)) return false;
    for (int i = 0; i < extensions.size(); ++i) {
        const QString dot = QStringLiteral(".") + extensions.at(i);
        const QString prev = readString(dot, QString());
        if (!prev.isEmpty() && prev != progId) {
            if (!writeString(dot, prevName, prev, error)) return false;  // 记下原来的关联,取消时恢复
        }
        if (!writeString(dot, QString(), progId, error)) return false;
        if (!writeString(dot + QStringLiteral("\\OpenWithProgids"), progId, QString(), error)) return false;
        if (!writeString(appKey + QStringLiteral("\\SupportedTypes"), dot, QString(), error)) return false;
        if (!writeAbs(capKey + QStringLiteral("\\FileAssociations"), dot, progId, error)) return false;
    }
    return true;
#else
    Q_UNUSED(extensions);
    if (error != nullptr) *error = QStringLiteral("only supported on Windows");
    return false;
#endif
}

// 取消本程序对这些扩展名的关联
bool FileAssoc::unregisterAll(const QStringList& extensions, QString* error)
{
#ifdef Q_OS_WIN
    const QString progId = QString::fromWCharArray(kProgId);
    const QString prevName = QString::fromWCharArray(kPrevValue);
    for (int i = 0; i < extensions.size(); ++i) {
        const QString dot = QStringLiteral(".") + extensions.at(i);
        if (readString(dot, QString()) == progId) {
            const QString prev = readString(dot, prevName);
            if (prev.isEmpty()) {
                if (!deleteTree(dot, error)) return false;
            } else {
                if (!writeString(dot, QString(), prev, error)) return false;
                deleteValue(dot, prevName);
            }
        }
    }
    {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString baseName = QFileInfo(exe).completeBaseName();
        deleteTreeAbs(QStringLiteral("Software\\Classes\\Applications\\") + QFileInfo(exe).fileName());
        deleteTreeAbs(QStringLiteral("Software\\") + baseName);
        deleteValueAbs(QStringLiteral("Software\\RegisteredApplications"), baseName);
    }
    return deleteTree(progId, error);
#else
    Q_UNUSED(extensions);
    if (error != nullptr) *error = QStringLiteral("only supported on Windows");
    return false;
#endif
}

// 改写本机 HKLM(所有用户)还是当前用户 HKCU
void FileAssoc::setMachineWide(bool on)
{
#ifdef Q_OS_WIN
    g_machine = on;
#else
    Q_UNUSED(on);
#endif
}

// 提权重启自己,把关联写进 HKLM
bool FileAssoc::registerAllUsers(QString* error)
{
#ifdef Q_OS_WIN
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";                       // 弹出管理员确认
    sei.lpFile = reinterpret_cast<const wchar_t*>(exe.utf16());
    sei.lpParameters = L"--assoc-machine";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        if (error != nullptr) *error = QStringLiteral("ShellExecute (%1)").arg(static_cast<int>(GetLastError()));   // 1223 = 用户点了\"否\"
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0 && error != nullptr) *error = QStringLiteral("exit code %1").arg(static_cast<int>(code));
    return code == 0;
#else
    if (error != nullptr) *error = QStringLiteral("only supported on Windows");
    return false;
#endif
}

// 清除 UserChoice:系统里\"这个扩展名始终用 XX 打开\"的选择,优先级比注册表里的默认关联高
int FileAssoc::clearUserChoice(const QStringList& extensions)
{
#ifdef Q_OS_WIN
    int cleared = 0;
    for (int i = 0; i < extensions.size(); ++i) {
        const QString full = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.")
                             + extensions.at(i) + QStringLiteral("\\UserChoice");
        if (RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(full.utf16())) == ERROR_SUCCESS) ++cleared;
    }
    return cleared;
#else
    Q_UNUSED(extensions);
    return 0;
#endif
}

// 打开 设置 → 默认应用,定位到本程序(用 RegisteredApplications 里的名字)
void FileAssoc::openDefaultAppsSettings()
{
    const QString baseName = QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps?registeredAppUser=") + baseName));
}

// 通知资源管理器刷新
void FileAssoc::notifyShell()
{
#ifdef Q_OS_WIN
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}
