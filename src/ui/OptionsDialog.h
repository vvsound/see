#pragma once
#include <QDialog>
#include <QTabWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSlider>
#include <QPushButton>
#include <QList>
#include <QString>

// 选项对话框(7 页:浏览/文件列表/缩略图/视图/显示/归档/杂项),控件与配置项一一绑定
class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    // 构造
    explicit OptionsDialog(QWidget* parent);

signals:
    // 点了"应用"或"确定"并已写入配置
    void optionsApplied();

private slots:
    // 确定
    void onOk();
    // 应用
    void onApply();
    // "设置文件关联"按钮
    void onFileAssoc();
    // 颜色选择按钮
    void onPickColor();

private:
    // 一个控件与一个配置键的绑定
    struct Binding {
        QString key;      // 如 "Browse/ShowToolbar"
        QString type;     // bool/int/double/enum/string/color
        QWidget* widget;
    };
    // 建立各页
    QWidget* buildBrowsePage();
    QWidget* buildFileListPage();
    QWidget* buildThumbnailsPage();
    QWidget* buildViewerPage();
    QWidget* buildDisplayPage();
    QWidget* buildArchivePage();
    QWidget* buildMiscPage();
    // 往布局里加一个控件并绑定配置键;文字取 opt.<键> 的界面文字
    QCheckBox* addCheck(QVBoxLayout* layout, const QString& key);
    QSpinBox* addSpin(QVBoxLayout* layout, const QString& key);
    QDoubleSpinBox* addDoubleSpin(QVBoxLayout* layout, const QString& key);
    QComboBox* addEnum(QVBoxLayout* layout, const QString& key);
    QLineEdit* addText(QVBoxLayout* layout, const QString& key);
    QPushButton* addColor(QVBoxLayout* layout, const QString& key);
    // 把配置值装入控件
    void loadValues();
    // 把控件值写入配置(逐项校验,失败写 error)
    bool storeValues(QString* error);

    QTabWidget* tabs_;
    QList<Binding> bindings_;
};
