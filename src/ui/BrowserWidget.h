#pragma once
#include <QWidget>
#include <QSplitter>
#include <QTreeView>
#include <QListView>
#include <QComboBox>
#include <QToolBar>
#include <QAction>
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
#include "ui/InfoPanel.h"
#include "ui/FilterBar.h"

// 键盘焦点所在的区域(同一时刻只有一个,用主题里的焦点框标出)
enum class FocusArea { Tree, Files, PathBar };

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
    // 把键盘焦点交给当前显示方式对应的文件区视图(缩略图/图标/列表/详细),这样方向键才能选文件。
    // 从查看窗口返回、程序刚启动、上一级之后都要调这个,不要直接对 BrowserWidget 本身 setFocus(没用)。
    void focusFileArea();
    // 当前焦点区域
    FocusArea focusArea() const;
    // 把焦点交给文件夹树 / 路径栏(区域被隐藏时什么也不做)
    void focusTree();
    void focusPathBar();
    // 路径栏 / 文件夹树当前是否显示
    bool pathBarShown() const { return pathBarVisible_; }
    bool treeShown() const { return treeVisible_; }
    // 在可见的区域之间循环(路径栏 -> 文件夹树 -> 文件区)
    void focusCycle(bool forward);
    // 目录历史:后退 / 前进;成功返回 true
    bool goBack();
    bool goForward();
    // 缩略图放大(direction>0)/缩小(direction<0),步长和范围见 [Thumbnails] 设置;非缩略图方式不处理
    void adjustThumbSize(int direction);
    // 筛选条:向后/向前切换扩展名标签、回到"全部"、切换排序依据、反转排序方向、切换网格/列表(条被隐藏时什么也不做)
    void filterStep(int delta);
    void filterAll();
    void cycleSort();
    void reverseSort();
    void toggleViewStyle();
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
    // 右侧信息面板显示/隐藏
    void setInfoVisible(bool v);
    void setPathBarVisible(bool v);
    // 把路径栏放进主窗口工具栏(工具栏列表里写 @PathBar);undock 放回浏览窗口顶部
    void dockPathBar(QToolBar* bar);
    void undockPathBar();
    // 路径栏历史
    QStringList recentFolders() const;
    // 保存/恢复分隔条位置(QuickView.state)
    void saveLayout();
    void restoreLayout();

signals:
    // 焦点区域变了(状态栏提示要跟着换)
    void focusAreaChanged();
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
    // 事件过滤:捕获列表上的右键、焦点变化、Ctrl+滚轮
    bool eventFilter(QObject* obj, QEvent* e) override;

private slots:
    // 树里点击了文件夹
    void onTreeClicked(const QModelIndex& index);
    // 树节点展开时读入子文件夹
    void onTreeExpanded(const QModelIndex& index);
    // 路径栏回车或选择
    void onPathActivated();
    // 树的当前项变化(键盘上下移动):延迟后载入该目录
    void onTreeCurrentChanged();
    // 延迟时间到,载入树当前项对应的目录
    void onTreeTimer();
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
    // 焦点框:给部件打上/去掉 mmFocus 属性,由主题 QSS 画框
    void setFocusFrame(QWidget* w, bool on);
    // 在历史里走 delta 步(-1 后退,+1 前进)
    bool navigateHistory(int delta);
    // 记录进入的文件夹(历史)
    void pushHistory(const QString& path);
    // 记住/恢复每个文件夹的选中项
    void rememberPosition();
    void restorePosition();
    // 筛选标签变了:重新装载文件区并保持选择
    void onFilterChanged();
    // 筛选条改了排序
    void onFilterSort(SortKey key, bool desc);
    // 用当前文件夹的内容刷新筛选条的标签,并把筛选交给模型(filterFolderChanged 为真表示刚换了文件夹)
    bool updateFilterBar();
    // 把排序状态同步到表头和筛选条
    void syncSortIndicators();
    // 树当前项是文件夹时立刻进入并把焦点交给文件区;虚拟节点则展开/折叠
    void activateTreeItem();

    QComboBox* pathBox_;
    QSplitter* hSplit_;
    QSplitter* vSplit_;
    QTreeView* tree_;
    QStandardItemModel* treeModel_;
    PreviewPane* preview_;
    InfoPanel* info_;
    bool infoVisible_;
    QWidget* centerPane_;
    FilterBar* filterBar_;
    QString filterFolder_;        // 筛选条上次对应的文件夹
    QStackedWidget* viewStack_;
    QListView* listView_;
    QTreeView* detailView_;
    FileListModel* model_;
    FolderList* folderList_;
    QTimer* previewTimer_;
    QTimer* treeTimer_;
    FocusArea focusArea_;
    QStringList history_;         // 目录历史
    int histPos_;                 // 当前在历史里的位置
    bool navigatingHistory_;      // 正在按历史走(不再往历史里添加)
    bool syncingTree_;            // 程序在同步树的选中项(不触发延迟载入)
    QStringList positions_;       // 位置记忆:文件夹、文件名 成对排列,最近的在前
    QAbstractItemDelegate* thumbDelegate_;
    QAbstractItemDelegate* plainDelegate_;
    ViewMode mode_;
    bool treeVisible_;
    bool previewVisible_;
    bool pathBarVisible_;
    QAction* pathAction_;
    QString folder_;
};
