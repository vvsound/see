#pragma once
#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>

// 工具 -> 管理图片扩展名
class ExtensionsDialog : public QDialog {
    Q_OBJECT
public:
    // 构造
    explicit ExtensionsDialog(QWidget* parent);
    // 对话框里当前的扩展名列表(小写、不带点、去重)
    QStringList extensions() const;

private slots:
    // 添加输入框里的扩展名
    void onAdd();
    // 删除列表里选中的扩展名
    void onRemove();
    // 确定:保存到 Files/Extensions
    void onAccept();
    // 输入框内容变化时启用/禁用添加按钮
    void onTextChanged(const QString& text);

private:
    // 把用户输入规范化(去点、去空白、小写);不合法返回空串
    static QString normalize(const QString& text);
    QListWidget* list_;
    QLineEdit* edit_;
    QPushButton* addButton_;
    QPushButton* removeButton_;
};
