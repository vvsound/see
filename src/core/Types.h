#pragma once
#include <QString>

// 浏览窗口的 5 种显示方式(顺序对应工具栏最右 5 个按钮)
enum class ViewMode {
    ThumbInfo = 0,  // 缩略图+信息
    Thumbs = 1,     // 仅缩略图
    Icons = 2,      // 图标
    List = 3,       // 列表
    Details = 4     // 详细
};

// 窗口当前模式
enum class WindowMode { Browser, Viewer };

// ViewMode 与字符串互转(用于 QuickView.state)
inline QString viewModeToString(ViewMode m)
{
    switch (m) {
    case ViewMode::ThumbInfo: return QStringLiteral("ThumbInfo");
    case ViewMode::Thumbs: return QStringLiteral("Thumbs");
    case ViewMode::Icons: return QStringLiteral("Icons");
    case ViewMode::List: return QStringLiteral("List");
    case ViewMode::Details: return QStringLiteral("Details");
    }
    return QStringLiteral("Details");
}
inline bool viewModeFromString(const QString& s, ViewMode* out)
{
    if (s == QLatin1String("ThumbInfo")) { *out = ViewMode::ThumbInfo; return true; }
    if (s == QLatin1String("Thumbs")) { *out = ViewMode::Thumbs; return true; }
    if (s == QLatin1String("Icons")) { *out = ViewMode::Icons; return true; }
    if (s == QLatin1String("List")) { *out = ViewMode::List; return true; }
    if (s == QLatin1String("Details")) { *out = ViewMode::Details; return true; }
    return false;
}
