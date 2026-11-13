#pragma once
#include <QWidget>
#include <QSplitter>
#include <QTreeView>
#include <QListView>
#include <QComboBox>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QStackedWidget>
#include <QTimer>
#include <QStringList>
#include <QAbstractItemDelegate>
#include "core/Types.h"
#include "core/FolderList.h"
#include "ui/FileListModel.h"
#include "ui/PreviewPane.h"

// 浏览窗口主体:路径栏 + 文件夹树 + 预览窗格 + 文件区(5 种显示方式)
class BrowserWidget : public QWidget {
    Q_OBJECT
public:
    // 构造
    explicit BrowserWidget(QWidget* parent);
    // 进入文件夹;失败弹出错误并返回 false
    bool openFolder(const QString& path);
    // 当前文件夹
    QString currentFolder() const;
    // 当前(焦点)项的路径,没有返回空
    QString currentFilePath() const;
    // 选中的全部路径(不含 "..")
    QStringList selectedPaths() const;
    // 选中的文件数
    int selectedCount() const;
    // 选中指定路径并滚动到可见
    void selectPath(const QString& path);
    // 设置/获取显示方式
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const;
    // 重新读取配置并应用(显示/隐藏各部件、网格线、列宽、缩略图尺寸等)
    void applyOptions();
    // 刷新当前文件夹
    void refresh();
    // 上一级
    void goUp();
    // 全选
    void selectAll();
    // 打开当前项(文件夹则进入,图片则发 imageActivated)
    void activateCurrent();
    // 对当前项开始重命名
    void startRename();
    // 文件夹列表对象(查看窗口翻页用)
    FolderList* folderList() const;
    // 状态栏四个字段:总计、选中大小和时间、选中文件名、图片信息
    QStringList statusFields() const;
    // 设置各部件可见性(菜单"视图"里的开关)
    void setTreeVisible(bool v);
    void setPreviewVisible(bool v);
    void setPathBarVisible(bool v);
    // 路径栏历史
    QStringList recentFolders() const;
    // 保存/恢复分隔条位置(QuickView.state)
    void saveLayout();
    void restoreLayout();

signals:
    // 双击或回车打开了一张图片
    void imageActivated(const QString& path);
    // 当前文件夹改变
    void folderChanged(const QString& path);
    // 选择改变(状态栏需刷新)
    void selectionChanged();
    // 视图里的操作请求执行某命令(例如 F2 之外的右键)
    void commandRequested(const QString& id);
    // 请求弹出右键菜单
    void contextMenuRequested(const QPoint& globalPos);

protected:
    // 事件过滤:捕获列表上的右键、键盘
    bool eventFilter(QObject* obj, QEvent* e) override;

private slots:
    // 树里点击了文件夹
    void onTreeClicked(const QModelIndex& index);
    // 树节点展开时读入子文件夹
    void onTreeExpanded(const QModelIndex& index);
    // 路径栏回车或选择
    void onPathActivated();
    // 文件区选择变化
    void onSelectionChanged();
    // 文件区双击/回车
    void onItemActivated(const QModelIndex& index);
    // FolderList 内容变化
    void onFolderListChanged();
    // 详细模式点击表头排序
    void onHeaderClicked(int section);
    // 预览延迟更新
    void onPreviewTimer();
    // 右键
    void onContextMenuRequested(const QPoint& pos);

private:
    // 建立界面
    void buildUi();
    // 当前显示的是列表类视图还是详细视图
    QAbstractItemView* activeView() const;
    // 更新路径栏文字和历史
    void updatePathBar(const QString& path);
    // 在树里选中某路径
    void syncTreeTo(const QString& path);
    // 按选项重建文件夹树的顶层节点(用户名、此电脑、库)
    void rebuildTree();
    // 读入某树节点的子文件夹(只读一次)
    void populateNode(QStandardItem* item);
    // 新建一个树节点(path 为空表示虚拟节点)
    QStandardItem* makeTreeItem(const QString& text, const QString& path, bool expandable);
    // 在树节点下查找路径,找到则逐级展开并返回节点
    QStandardItem* findTreeItem(QStandardItem* parent, const QString& path);
    // 缩略图方式下是否应隐藏预览窗格
    bool previewShouldHide() const;
    // 保存最近文件夹列表
    void rememberFolder(const QString& path);
    // 按显示方式配置 listView_ 的网格和图标尺寸
    void configureListView();
    // 按各开关更新左侧(目录树+预览)的显示
    void updateLeftPane();

    QComboBox* pathBox_;
    QSplitter* hSplit_;
    QSplitter* vSplit_;
    QTreeView* tree_;
    QStandardItemModel* treeModel_;
    PreviewPane* preview_;
    QStackedWidget* viewStack_;
    QListView* listView_;
    QTreeView* detailView_;
    FileListModel* model_;
    FolderList* folderList_;
    QTimer* previewTimer_;
    QAbstractItemDelegate* thumbDelegate_;
    QAbstractItemDelegate* plainDelegate_;
    ViewMode mode_;
    bool treeVisible_;
    bool previewVisible_;
    bool pathBarVisible_;
    QString folder_;
};
