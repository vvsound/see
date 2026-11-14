#pragma once
#include <QString>
#include <QStringList>
#include <QColor>

// 主题:启动时读 exe 旁 theme\\current.qss 并应用。主题怎么生成由 MoonManager 负责,这里只管读和应用。
// current.qss 里形如 /*@var 名字=值*/ 的行是给程序本体绘制用的主题变量(缩略图选中色等),这里负责解析。
class Theme {
public:
    // theme\\current.qss 的完整路径
    static QString path();
    // 读取样式表文本;文件不存在或读不了返回 false 并写明原因(不兜底)
    static bool read(QString* qss, QString* error);
    // 读取并应用到整个程序(同时解析 @var 变量);失败返回 false
    static bool apply(QString* error);
    // 程序本体绘制要用的主题变量名
    static QStringList requiredVars();
    // 解析 qss 里的 @var 行并检查 requiredVars 是否齐全、颜色是否合法;有问题返回 false 并写明
    static bool checkVars(const QString& qss, QString* error);
    // 取主题变量;不存在直接 fatal(说明缺哪个变量)
    static QString var(const QString& name);
    // 取主题颜色变量(#RRGGBB);不存在或写错直接 fatal
    static QColor color(const QString& name);
};
