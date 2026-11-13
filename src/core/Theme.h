#pragma once
#include <QString>

// 主题:启动时读 exe 旁 theme\current.qss 并应用。主题怎么生成由 MoonManager 负责,这里只管读和应用。
class Theme {
public:
    // theme\current.qss 的完整路径
    static QString path();
    // 读取样式表文本;文件不存在或读不了返回 false 并写明原因(不兜底)
    static bool read(QString* qss, QString* error);
    // 读取并应用到整个程序;失败返回 false
    static bool apply(QString* error);
};
