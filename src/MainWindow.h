#pragma once
#include <QMainWindow>
#include <QStackedWidget>
#include <QToolBar>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include "core/Types.h"
#include "core/ImageLoader.h"
#include "core/FolderList.h"
#include "ui/BrowserWidget.h"
#include "ui/ViewerWidget.h"

// 主窗口:同一个窗口在"浏览"和"查看"两种模式间切换;菜单/工具栏/快捷键全部由命令注册表与配置生成
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // 构造
    explicit MainWindow(QWidget* parent);
    // 启动:根据命令行路径决定进入浏览还是查看(paths 为空则打开最近文件夹)
    void startWith(const QStringList& paths);
    // 处理其他进程转交或拖入的路径;isDrop 为 true 用 Misc/OnDropFile,否则用 Misc/OnStartFile
    void openExternalPaths(const QStringList& paths, bool isDrop);

protected:
    // 关闭:保存窗口和状态
    void closeEvent(QCloseEvent* e) override;
    // 拖入文件
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    // 事件过滤:全屏时鼠标静止隐藏光标
    bool eventFilter(QObject* obj, QEvent* e) override;

private slots:
    // 查看窗口里一张图载入完成
    void onImageLoaded(const LoadResult& result);
    // 浏览窗口打开了图片
    void onBrowserImageActivated(const QString& path);
    // 浏览窗口文件夹变化
    void onBrowserFolderChanged(const QString& path);
    // 浏览窗口选择变化(更新状态栏)
    void onBrowserSelectionChanged();
    // 浏览/查看窗口里的鼠标/视图请求执行命令
    void onCommandRequested(const QString& id);
    // 缩放比例变化(更新状态栏)
    void onViewerZoomChanged(double zoom);
    // 弹出右键菜单
    void onContextMenu(const QPoint& globalPos);
    void onTreeContextMenu(const QString& path, const QPoint& globalPos);
    bool createFolderIn(const QString& dirPath);
    // 文件夹列表内容变化(文件被删/新增)
    void onFolderListChanged();
    // 全屏时隐藏光标的计时器
    void onHideCursorTimer();
    // 选项已应用:重新读取并应用所有显示设置
    void onOptionsApplied();

private:
    // 注册所有命令的处理函数
    void registerHandlers();
    // 运行一条自定义命令
    void runCustomCommand(const QString& id);
    // 按 [MenuBrowser]/[MenuViewer] 配置重建菜单栏
    void rebuildMenus();
    // 按 [Toolbar] 配置重建工具栏
    void rebuildToolbar();
    // 建立状态栏
    void buildStatusBar();
    // 切换模式
    void setMode(WindowMode mode);
    // 进入查看模式并显示 path;path 所在文件夹成为当前列表
    void enterViewer(const QString& path);
    // 回到浏览模式(选中刚才查看的图片)
    void leaveViewer();
    // 按 Display/DarkTitleBar 设置 Windows 标题栏深浅
    void applyTitleBar();
    // 跳到第 index 张图(0 起),并预加载前后
    void gotoImage(int index);
    // 方向键:图片比窗口大时平移,否则翻页(见 Viewer/ArrowPan)
    void arrowKey(int dx, int dy);
    // 跳到上/下一个有图片的文件夹的第一张(dir=-1/+1)
    void jumpFolder(int dir);
    // 翻页停稳后才真正请求解码并预加载(按住方向键时中间的图不解码)
    void settleFlip();
    // 打开 path 所在文件夹并生成查看列表;失败已弹错误并返回 false
    bool prepareViewList(const QString& path, int* index);
    // 按当前模式/焦点区域更新状态栏左侧的按键提示
    void updateHint();
    // 把提示模板里的 {命令ID} 换成该命令当前的第一个快捷键
    QString expandHint(const QString& tpl) const;
    // 提示条是否显示(按模式读 Browse/ShowKeyHints 或 Viewer/ShowKeyHints)
    bool hintsVisible() const;
    // 翻页命令
    void nextImage();
    void prevImage();
    void firstImage();
    void lastImage();
    // 当前查看的图片路径,没有返回空
    QString currentImagePath() const;
    // 全屏切换
    void toggleFullscreen();
    // 按配置 Viewer/AlwaysOnTop 应用总在最上(仅查看模式生效;命令处理函数先改配置再调它)
    void toggleAlwaysOnTop();
    // 进入/退出全屏(不改配置)
    void setFullscreen(bool on);
    // 打开路径列表:replaceList 为 true 时替换当前列表,否则把外部文件添加到列表
    void openPaths(const QStringList& paths, bool replaceList);
    // 窗口按图片尺寸自动调整(Viewer/AutoResizeWindow)
    void autoResizeToImage();
    // 更新标题栏
    void updateTitle();
    // 更新状态栏
    void updateBrowserStatus();
    void updateViewerStatus(const LoadResult& r);
    // 应用显示设置:工具栏/状态栏/菜单栏可见性、工具提示等
    void applyVisibility();
    // 删除(浏览:选中项;查看:当前图)
    void cmdDelete();
    // 重命名
    void cmdRename();
    // 复制到 / 移动到
    void cmdCopyTo();
    void cmdMoveTo();
    // 复制文件 / 复制图片到剪贴板
    void cmdCopyFile();
    void cmdCopyImage();
    // 剪切文件到剪贴板
    void cmdCutFile();
    void cmdPaste();
    void cmdNewFolder();
    void cmdToggleSubfolders();
    void cmdShowInExplorer();
    void cmdSetWallpaper();
    // 编辑文件描述(descript.ion)
    void cmdEditDescription();
    // 重新载入当前图(不用缓存)
    void cmdReloadImage();
    // 另存为(当前图,含旋转)
    void cmdSaveAs();
    // 按对话框选择的方式旋转
    void cmdRotateBy();
    // 属性
    void cmdProperties();
    // 打印
    void cmdPrint();
    // 打开文件对话框
    void cmdOpenFile();
    // 选项 / 扩展名管理 / 文件关联 / 关于
    void cmdOptions();
    void cmdManageExtensions();
    void cmdFileAssoc();
    void cmdAbout();
    // 检查某命令对应的勾选状态并同步(视图菜单里的复选项)
    void syncCheckStates();
    // 保存/恢复窗口几何(按模式分别保存)
    void saveGeometry(WindowMode mode);
    void restoreGeometry(WindowMode mode);
    // 在命令行路径之外,当前操作的目标文件列表(浏览:选中项;查看:当前图)
    QStringList targetPaths() const;
    // 报告错误(对话框)
    void showError(const QString& message);

    QStackedWidget* stack_;
    BrowserWidget* browser_;
    ViewerWidget* viewer_;
    QToolBar* toolbar_;
    QLabel* statusLabels_[6];
    QLabel* hintLabel_;
    QTimer* settleTimer_;
    WindowMode mode_;
    QString currentPath_;        // 查看模式当前图片
    int currentIndex_;
    QList<FileEntry> viewList_;  // 查看模式的图片列表
    bool fullscreen_;
    QTimer* hideCursorTimer_;
    QElapsedTimer loadTimer_;
    QRect browserGeometry_;
    QRect viewerGeometry_;
    QString lastBrowserFolder_;
    bool browserMenuBarShown_;
};
