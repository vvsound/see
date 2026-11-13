#pragma once
#include <QString>
#include <QStringList>
#include <QList>

// 自定义命令:读 exe 旁的 custom_commands.ini,每段 [Custom.命令ID] 是一条“运行某个程序”的命令。
// 个数不限、用途不限;注册后和内置命令一样可被快捷键/菜单/工具栏/右键菜单/鼠标引用。
// 文件不存在 = 没有自定义命令(合法);文件存在但写错 = 直接报错(含行号),不兜底。
class CustomCommands {
public:
    struct Item {
        QString id;       // 命令 ID(字母开头,字母数字下划线)
        QString name;     // 显示名
        QString program;  // 要运行的程序
        QString args;     // 参数,可含 {file} {folder} {selected}
        QString scope;    // Browser / Viewer / Both
        QString icon;     // 图标文件名(xxx.svg),可空
        QString keys;     // 快捷键,逗号分隔,可空
    };

    // 全局唯一实例
    static CustomCommands& instance();
    // custom_commands.ini 的完整路径
    static QString path();
    // 读取文件并把每条命令注册进 Commands(要在 registerDefaults 之后、createActions 之前调用)
    bool load(QString* error);
    // 是否是自定义命令
    bool has(const QString& id) const;
    // 取一条(必须存在)
    Item item(const QString& id) const;
    // 把一条命令展开成 程序 + 参数列表;缺少需要的值时返回 false 并写明原因
    bool build(const QString& id, const QString& file, const QString& folder,
               const QStringList& selected, QString* program, QStringList* args, QString* error) const;

private:
    CustomCommands() {}
    QList<Item> items_;
};
