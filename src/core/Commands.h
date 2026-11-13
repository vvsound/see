#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QHash>
#include <QAction>
#include <QIcon>
#include <functional>
#include "Types.h"

// 命令适用的窗口模式
enum class CmdScope { Browser, Viewer, Both };

// 命令定义:ID、名称、默认键、图标
struct CommandDef {
    QString id;           // 命令 ID(一旦确定不许改)
    QString nameKey;      // 界面文字键,如 cmd.NextImage
    QString defaultKeys;  // 默认快捷键(逗号分隔)
    QString iconFile;     // 图标文件名,在 resources/icons/ 下,如 next.svg;空表示无图标
    bool checkable;       // 是否可勾选
    CmdScope scope;       // 适用的窗口模式
};

// 命令注册表:菜单、工具栏、快捷键、右键菜单都由它和配置生成
class Commands : public QObject {
    Q_OBJECT
public:
    // 全局唯一实例
    static Commands& instance();
    // 注册全部内置命令(在这里加新命令)
    void registerDefaults();
    // 注册一个命令
    void registerCommand(const CommandDef& def);
    // 按 [Keys] 配置为所有命令创建 QAction(之后菜单和工具栏取用)
    void createActions(QObject* parent);
    // 取命令对应的 QAction(不存在返回 nullptr)
    QAction* action(const QString& id) const;
    // 命令是否已注册
    bool has(const QString& id) const;
    // 取命令定义
    CommandDef def(const QString& id) const;
    // 所有命令 ID(注册顺序)
    QStringList ids() const;
    // 设置命令的处理函数
    void setHandler(const QString& id, std::function<void()> fn);
    // 执行命令(命令被禁用或没有处理函数时:没处理函数会报错)
    void trigger(const QString& id);
    // 按窗口模式启用/禁用命令(scope 不符的命令禁用,快捷键随之失效)
    void setMode(WindowMode mode);
    // 命令的图标(先找 exe 旁 icons/ 文件夹,再找内置)
    QIcon iconFor(const QString& id) const;
    // 命令的显示名称
    QString nameOf(const QString& id) const;
    // 把 "Ctrl+O,F2" 解析成快捷键列表;写错会 fatal 并指出命令 ID
    QList<QKeySequence> parseKeys(const QString& id, const QString& text) const;

signals:
    // 任一命令被触发(调试和统计用)
    void commandTriggered(const QString& id);

private slots:
    // 所有 QAction 的 triggered 都连到这里
    void onActionTriggered();

private:
    Commands();
    QList<CommandDef> defs_;
    QMap<QString, int> defIndex_;
    QMap<QString, QAction*> actions_;
    QHash<QString, std::function<void()> > handlers_;
};
