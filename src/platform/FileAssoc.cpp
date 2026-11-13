#include "FileAssoc.h"
#include "core/Strings.h"

#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const wchar_t* kProgId = L"QuickView.Image";
const wchar_t* kPrevValue = L"QuickView.PrevProgId";

// 写 HKCU\Software\Classes\<subKey> 下的字符串值(valueName 为空表示默认值)
bool writeString(const QString& subKey, const QString& valueName, const QString& data, QString* error)
{
    const QString full = QStringLiteral("Software\\Classes\\") + subKey;
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(full.utf16()), 0, nullptr,
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

// 读字符串值;不存在返回空串
QString readString(const QString& subKey, const QString& valueName)
{
    const QString full = QStringLiteral("Software\\Classes\\") + subKey;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(full.utf16()), 0, KEY_READ, &key) != ERROR_SUCCESS) {
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
    const LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(full.utf16()));
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        if (error != nullptr) *error = QStringLiteral("%1 (%2)").arg(full).arg(rc);
        return false;
    }
    return true;
}

// 删除某个值
void deleteValue(const QString& subKey, const QString& valueName)
{
    const QString full = QStringLiteral("Software\\Classes\\") + subKey;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(full.utf16()), 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
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
    if (!writeString(progId, QString(), S("app.name"), error)) return false;
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
    return deleteTree(progId, error);
#else
    Q_UNUSED(extensions);
    if (error != nullptr) *error = QStringLiteral("only supported on Windows");
    return false;
#endif
}

// 通知资源管理器刷新
void FileAssoc::notifyShell()
{
#ifdef Q_OS_WIN
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}
