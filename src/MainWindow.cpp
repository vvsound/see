#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QClipboard>
#include <QUrl>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QScreen>
#include <QGuiApplication>
#include <QProcess>
#include <QTransform>
#include <QStandardPaths>
#include <QPrinter>
#include <QPrintDialog>
#include <QPainter>
#include <QAction>
#include <QFrame>
#include <QInputDialog>
#include <QLineEdit>
#include <QPainter>
#include <functional>
#include "core/Theme.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

#include "core/Config.h"
#include "core/State.h"
#include "core/Strings.h"
#include "core/Commands.h"
#include "core/CustomCommands.h"
#include "core/FileOps.h"
#include "core/DescriptIon.h"
#include "core/ThumbnailCache.h"
#include "platform/FileAssoc.h"
#include "ui/OptionsDialog.h"
#include "ui/ExtensionsDialog.h"

namespace {

// 文件大小文字(B / KB / MB / GB)
QString sizeText(qint64 bytes)
{
    if (bytes < 1024) return S("status.sizeB").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024.0) return S("status.sizeKB").arg(QString::number(kb, 'f', 1));
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return S("status.sizeMB").arg(QString::number(mb, 'f', 1));
    return S("status.sizeGB").arg(QString::number(mb / 1024.0, 'f', 1));
}

// 窗口几何 <-> 字符串 "x,y,w,h,max"
QString rectToString(const QRect& r, bool maximized)
{
    return QStringLiteral("%1,%2,%3,%4,%5").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(maximized ? 1 : 0);
}

bool rectFromString(const QString& s, QRect* r, bool* maximized)
{
    const QStringList p = s.split(QLatin1Char(','));
    if (p.size() != 5) return false;
    int v[5];
    for (int i = 0; i < 5; ++i) {
        bool ok = false;
        v[i] = p.at(i).trimmed().toInt(&ok);
        if (!ok) return false;
    }
    if (v[2] < 50 || v[3] < 50) return false;
    *r = QRect(v[0], v[1], v[2], v[3]);
    *maximized = (v[4] != 0);
    return true;
}

} // namespace

// 构造:建立界面、注册命令处理函数、连接信号
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), stack_(nullptr), browser_(nullptr), viewer_(nullptr), toolbar_(nullptr),
      mode_(WindowMode::Browser), currentIndex_(-1), fullscreen_(false), hideCursorTimer_(nullptr),
      browserMenuBarShown_(true)
{
    hintLabel_ = nullptr;
    settleTimer_ = nullptr;
    for (int i = 0; i < 6; ++i) statusLabels_[i] = nullptr;

    stack_ = new QStackedWidget(this);
    browser_ = new BrowserWidget(stack_);
    viewer_ = new ViewerWidget(stack_);
    stack_->addWidget(browser_);
    stack_->addWidget(viewer_);
    setCentralWidget(stack_);

    toolbar_ = new QToolBar(this);
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(24, 24));
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    addToolBar(Qt::TopToolBarArea, toolbar_);

    hideCursorTimer_ = new QTimer(this);
    hideCursorTimer_->setSingleShot(true);
    hideCursorTimer_->setInterval(2000);
    connect(hideCursorTimer_, &QTimer::timeout, this, &MainWindow::onHideCursorTimer);

    settleTimer_ = new QTimer(this);
    settleTimer_->setSingleShot(true);
    connect(settleTimer_, &QTimer::timeout, this, [this]() { settleFlip(); });

    setAcceptDrops(true);

    // 所有命令的 action 都加到窗口上,这样菜单里没有的命令快捷键也有效
    Commands& cmds = Commands::instance();
    const QStringList ids = cmds.ids();
    for (int i = 0; i < ids.size(); ++i) {
        QAction* a = cmds.action(ids.at(i));
        if (a != nullptr) addAction(a);
    }
    registerHandlers();
    {   // 右侧信息面板顶部的 4 个图标开关(预览/基本信息/EXIF/直方图)
        static const char* const kIds[] = {"ToggleInfoPreview", "ToggleInfoBasic", "ToggleInfoExif", "ToggleInfoHistogram"};
        QList<QAction*> acts;
        for (int i = 0; i < 4; ++i) {
            const QString id = QLatin1String(kIds[i]);
            QAction* a = cmds.action(id);
            if (a != nullptr) a->setToolTip(cmds.tipOf(id));
            acts.append(a);
        }
        browser_->infoPanel()->setToggleActions(acts);
    }
    viewer_->setZoomLock(Config::instance().getBool(QStringLiteral("Viewer/ZoomLock")));

    connect(browser_, &BrowserWidget::imageActivated, this, &MainWindow::onBrowserImageActivated);
    connect(browser_, &BrowserWidget::folderChanged, this, &MainWindow::onBrowserFolderChanged);
    connect(browser_, &BrowserWidget::selectionChanged, this, &MainWindow::onBrowserSelectionChanged);
    connect(browser_, &BrowserWidget::focusAreaChanged, this, [this]() { updateHint(); });
    connect(browser_, &BrowserWidget::commandRequested, this, &MainWindow::onCommandRequested);
    connect(browser_, &BrowserWidget::contextMenuRequested, this, &MainWindow::onContextMenu);
    connect(viewer_, &ViewerWidget::zoomChanged, this, &MainWindow::onViewerZoomChanged);
    connect(viewer_, &ViewerWidget::commandRequested, this, &MainWindow::onCommandRequested);
    connect(viewer_, &ViewerWidget::contextMenuRequested, this, &MainWindow::onContextMenu);
    connect(viewer_, &ViewerWidget::mouseMoved, this, [this]() {
        if (!fullscreen_) return;
        viewer_->setCursorHidden(false);
        if (Config::instance().getBool(QStringLiteral("Viewer/HideMouseInFullscreen"))) hideCursorTimer_->start();
    });
    connect(&ImageLoader::instance(), &ImageLoader::loaded, this, &MainWindow::onImageLoaded);
    connect(browser_->folderList(), &FolderList::changed, this, &MainWindow::onFolderListChanged);

    buildStatusBar();
    setMode(WindowMode::Browser);
    applyTitleBar();
}

// 启动:命令行没有路径则打开最近文件夹;有路径则直接查看
void MainWindow::startWith(const QStringList& paths)
{
    Config& cfg = Config::instance();
    browser_->restoreLayout();
    if (paths.isEmpty()) {
        QString folder;
        if (cfg.getBool(QStringLiteral("Browse/StartInLastFolder"))) {
            const QStringList rec = browser_->recentFolders();
            if (!rec.isEmpty() && QDir(rec.first()).exists()) folder = rec.first();
        }
        if (folder.isEmpty()) folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (folder.isEmpty()) folder = QDir::homePath();
        browser_->openFolder(folder);
    } else {
        openPaths(paths, true);
        if (browser_->currentFolder().isEmpty()) browser_->openFolder(QDir::homePath());
    }
    if (mode_ == WindowMode::Browser) browser_->focusFileArea();  // 否则刚启动时方向键选不了文件,要先点一下才行
    show();
}

// 处理其他进程转交或拖入的路径
void MainWindow::openExternalPaths(const QStringList& paths, bool isDrop)
{
    if (paths.isEmpty()) return;
    Config& cfg = Config::instance();
    const QString action = isDrop ? cfg.getEnum(QStringLiteral("Misc/OnDropFile"))
                                  : cfg.getEnum(QStringLiteral("Misc/OnStartFile"));
    if (action == QLatin1String("NewWindow")) {
        QStringList args;
        args << QStringLiteral("--new-window") << paths;
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args)) {
            showError(S("err.startup").arg(QCoreApplication::applicationFilePath()));
        }
        return;
    }
    if (isMinimized()) showNormal();
    raise();
    activateWindow();
    openPaths(paths, action == QLatin1String("Replace"));
}

// 打开路径列表
void MainWindow::openPaths(const QStringList& paths, bool replaceList)
{
    if (paths.isEmpty()) return;
    const QString first = QDir::fromNativeSeparators(QFileInfo(paths.first()).absoluteFilePath());
    const QFileInfo fi(first);
    if (!fi.exists()) {
        showError(S("err.readFile").arg(first).arg(S("err.fileNotFound")));
        return;
    }
    if (fi.isDir()) {
        if (mode_ == WindowMode::Viewer) leaveViewer();
        browser_->openFolder(first);
        return;
    }
    FolderList* fl = browser_->folderList();
    if (replaceList) {
        fl->clearExtraFiles();
    }
    for (int i = 0; i < paths.size(); ++i) {
        const QString p = QDir::fromNativeSeparators(QFileInfo(paths.at(i)).absoluteFilePath());
        if (QFileInfo(p).isDir()) continue;
        // 替换列表时,第一个文件的文件夹成为列表,其余文件夹不同的才作为外部文件添加
        const bool isFirst = (i == 0);
        if (replaceList && isFirst) continue;
        if (!fl->folder().isEmpty() && fl->imageIndexOf(p) < 0) {
            QString err;
            if (!fl->addExtraFile(p, &err)) {
                showError(err);
                return;
            }
        }
    }
    enterViewer(first);
}

// 关闭窗口:保存窗口位置和布局
void MainWindow::closeEvent(QCloseEvent* e)
{
    saveGeometry(mode_);
    browser_->saveLayout();
    State::instance().save();
    e->accept();
}

// 拖入文件:只接受本地文件
void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

// 放下文件
void MainWindow::dropEvent(QDropEvent* e)
{
    QStringList paths;
    const QList<QUrl> urls = e->mimeData()->urls();
    for (int i = 0; i < urls.size(); ++i) {
        if (urls.at(i).isLocalFile()) paths.append(urls.at(i).toLocalFile());
    }
    if (paths.isEmpty()) return;
    e->acceptProposedAction();
    openExternalPaths(paths, true);
}

// 事件过滤:关闭工具提示选项(Misc/ShowToolTips)时吞掉工具栏按钮的提示事件
bool MainWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::ToolTip && !Config::instance().getBool(QStringLiteral("Misc/ShowToolTips"))) {
        return true;
    }
    return QMainWindow::eventFilter(obj, e);
}

// 查看窗口里一张图载入完成(忽略不是当前图的结果,例如预加载)
void MainWindow::onImageLoaded(const LoadResult& result)
{
    if (mode_ != WindowMode::Viewer || result.path != currentPath_) return;
    viewer_->showResult(result);
    updateViewerStatus(result);
    QTimer::singleShot(0, this, [this]() {
        if (mode_ != WindowMode::Viewer) return;
        autoResizeToImage();
        viewer_->applyInitialZoom();
    });
}

// 浏览窗口打开了图片
void MainWindow::onBrowserImageActivated(const QString& path)
{
    enterViewer(path);
}

// 浏览窗口文件夹变化
void MainWindow::onBrowserFolderChanged(const QString& path)
{
    lastBrowserFolder_ = path;
    if (mode_ == WindowMode::Browser) {
        updateTitle();
        updateBrowserStatus();
    }
}

// 浏览窗口选择变化
void MainWindow::onBrowserSelectionChanged()
{
    if (mode_ == WindowMode::Browser) updateBrowserStatus();
}

// 鼠标/视图请求执行命令
void MainWindow::onCommandRequested(const QString& id)
{
    Commands::instance().trigger(id);
}

// 缩放比例变化
void MainWindow::onViewerZoomChanged(double zoom)
{
    if (statusLabels_[4] != nullptr) statusLabels_[4]->setText(S("status.zoom").arg(qRound(zoom * 100.0)));
    updateHint();
}

// 弹出右键菜单(内容来自 [ContextMenu])
void MainWindow::onContextMenu(const QPoint& globalPos)
{
    Config& cfg = Config::instance();
    Commands& cmds = Commands::instance();
    const QString key = (mode_ == WindowMode::Browser) ? QStringLiteral("ContextMenu/Browser")
                                                       : QStringLiteral("ContextMenu/Viewer");
    QMenu menu(this);
    const QStringList items = cfg.getList(key);
    for (int i = 0; i < items.size(); ++i) {
        const QString id = items.at(i);
        if (id == QLatin1String("|")) {
            menu.addSeparator();
            continue;
        }
        if (!cmds.has(id)) Config::fatal(S("err.unknownCommand").arg(id).arg(key));
        menu.addAction(cmds.action(id));
    }
    menu.exec(globalPos);
}

// 文件夹列表内容变化:查看模式下重建翻页列表
void MainWindow::onFolderListChanged()
{
    if (mode_ == WindowMode::Browser) {
        updateBrowserStatus();
        return;
    }
    FolderList* fl = browser_->folderList();
    if (Config::instance().getBool(QStringLiteral("Viewer/ViewAllInFolder"))) {
        viewList_ = fl->imageEntries();
    } else {
        // 只看这一张:文件还在就保留,不在了就退出
        if (!QFileInfo(currentPath_).isFile()) viewList_.clear();
    }
    if (viewList_.isEmpty()) {
        leaveViewer();
        return;
    }
    const int idx = (viewList_.size() == 1 && !Config::instance().getBool(QStringLiteral("Viewer/ViewAllInFolder")))
                        ? 0 : fl->imageIndexOf(currentPath_);
    if (idx >= 0) {
        currentIndex_ = idx;
        if (statusLabels_[0] != nullptr) {
            statusLabels_[0]->setText(S("status.index").arg(currentIndex_ + 1).arg(viewList_.size()));
        }
        updateTitle();
        return;
    }
    // 当前图不在列表里了(被删/移走):显示原位置的下一张
    int next = currentIndex_;
    if (next >= viewList_.size()) next = viewList_.size() - 1;
    if (next < 0) next = 0;
    gotoImage(next);
}

// 全屏时光标静止一段时间后隐藏
void MainWindow::onHideCursorTimer()
{
    if (fullscreen_ && Config::instance().getBool(QStringLiteral("Viewer/HideMouseInFullscreen"))) {
        viewer_->setCursorHidden(true);
    }
}

// 选项已应用:重新读取并应用所有显示设置
void MainWindow::onOptionsApplied()
{
    viewer_->applyOptions();
    browser_->applyOptions();
    applyVisibility();
    syncCheckStates();
    toggleAlwaysOnTop();
    if (fullscreen_ != Config::instance().getBool(QStringLiteral("Viewer/Fullscreen")) && mode_ == WindowMode::Viewer) {
        setFullscreen(Config::instance().getBool(QStringLiteral("Viewer/Fullscreen")));
    }
    updateTitle();
    if (mode_ == WindowMode::Viewer) viewer_->applyInitialZoom();
    else updateBrowserStatus();
}

// 注册所有命令的处理函数
void MainWindow::registerHandlers()
{
    Commands& c = Commands::instance();
    c.setHandler(QStringLiteral("OpenFile"), [this]() { cmdOpenFile(); });
    c.setHandler(QStringLiteral("OpenSelected"), [this]() { browser_->activateCurrent(); });
    c.setHandler(QStringLiteral("Exit"), [this]() { close(); });
    c.setHandler(QStringLiteral("GoUp"), [this]() { browser_->goUp(); });
    c.setHandler(QStringLiteral("Refresh"), [this]() {
        if (mode_ == WindowMode::Browser) browser_->refresh();
        else browser_->folderList()->refresh();
    });
    c.setHandler(QStringLiteral("NextImage"), [this]() { nextImage(); });
    c.setHandler(QStringLiteral("PrevImage"), [this]() { prevImage(); });
    c.setHandler(QStringLiteral("FirstImage"), [this]() { firstImage(); });
    c.setHandler(QStringLiteral("LastImage"), [this]() { lastImage(); });
    c.setHandler(QStringLiteral("BackToBrowser"), [this]() { leaveViewer(); });
    c.setHandler(QStringLiteral("ZoomIn"), [this]() { viewer_->zoomIn(); });
    c.setHandler(QStringLiteral("ZoomOut"), [this]() { viewer_->zoomOut(); });
    c.setHandler(QStringLiteral("FitWindow"), [this]() { viewer_->fitToWindow(); });
    c.setHandler(QStringLiteral("ActualSize"), [this]() { viewer_->actualSize(); });
    c.setHandler(QStringLiteral("RotateLeft"), [this]() { viewer_->rotateLeft(); });
    c.setHandler(QStringLiteral("RotateRight"), [this]() { viewer_->rotateRight(); });
    c.setHandler(QStringLiteral("Fullscreen"), [this]() { toggleFullscreen(); });
    c.setHandler(QStringLiteral("Delete"), [this]() { cmdDelete(); });
    c.setHandler(QStringLiteral("Rename"), [this]() { cmdRename(); });
    c.setHandler(QStringLiteral("CopyTo"), [this]() { cmdCopyTo(); });
    c.setHandler(QStringLiteral("MoveTo"), [this]() { cmdMoveTo(); });
    c.setHandler(QStringLiteral("CopyFile"), [this]() { cmdCopyFile(); });
    c.setHandler(QStringLiteral("CopyImage"), [this]() { cmdCopyImage(); });
    c.setHandler(QStringLiteral("SelectAll"), [this]() { browser_->selectAll(); });
    c.setHandler(QStringLiteral("Properties"), [this]() { cmdProperties(); });
    c.setHandler(QStringLiteral("Print"), [this]() { cmdPrint(); });
    c.setHandler(QStringLiteral("ViewThumbInfo"), [this]() { browser_->setViewMode(ViewMode::ThumbInfo); syncCheckStates(); updateBrowserStatus(); });
    c.setHandler(QStringLiteral("ViewThumbs"), [this]() { browser_->setViewMode(ViewMode::Thumbs); syncCheckStates(); updateBrowserStatus(); });
    c.setHandler(QStringLiteral("ViewIcons"), [this]() { browser_->setViewMode(ViewMode::Icons); syncCheckStates(); updateBrowserStatus(); });
    c.setHandler(QStringLiteral("ViewList"), [this]() { browser_->setViewMode(ViewMode::List); syncCheckStates(); updateBrowserStatus(); });
    c.setHandler(QStringLiteral("ArrowLeft"), [this]() { arrowKey(-1, 0); });
    c.setHandler(QStringLiteral("ArrowRight"), [this]() { arrowKey(1, 0); });
    c.setHandler(QStringLiteral("ArrowUp"), [this]() { arrowKey(0, -1); });
    c.setHandler(QStringLiteral("ArrowDown"), [this]() { arrowKey(0, 1); });
    c.setHandler(QStringLiteral("PrevFolder"), [this]() { jumpFolder(-1); });
    c.setHandler(QStringLiteral("NextFolder"), [this]() { jumpFolder(1); });
    c.setHandler(QStringLiteral("ToggleKeyHints"), [this]() {
        Config::instance().setBool(mode_ == WindowMode::Browser ? QStringLiteral("Browse/ShowKeyHints") : QStringLiteral("Viewer/ShowKeyHints"),
                                   Commands::instance().action(QStringLiteral("ToggleKeyHints"))->isChecked());
        buildStatusBar();
        updateHint();
    });
    c.setHandler(QStringLiteral("FocusNext"), [this]() { browser_->focusCycle(true); });
    c.setHandler(QStringLiteral("FocusPrev"), [this]() { browser_->focusCycle(false); });
    c.setHandler(QStringLiteral("FocusPathBar"), [this]() {
        if (!browser_->pathBarShown()) { showError(S("err.pathBarHidden")); return; }
        browser_->focusPathBar();
    });
    c.setHandler(QStringLiteral("FocusTree"), [this]() {
        if (!browser_->treeShown()) { showError(S("err.treeHidden")); return; }
        browser_->focusTree();
    });
    c.setHandler(QStringLiteral("FocusFiles"), [this]() { browser_->focusFileArea(); });
    c.setHandler(QStringLiteral("HistoryBack"), [this]() { browser_->goBack(); });
    c.setHandler(QStringLiteral("HistoryForward"), [this]() { browser_->goForward(); });
    c.setHandler(QStringLiteral("ThumbBigger"), [this]() { browser_->adjustThumbSize(1); });
    c.setHandler(QStringLiteral("ThumbSmaller"), [this]() { browser_->adjustThumbSize(-1); });
    c.setHandler(QStringLiteral("FilterNext"), [this]() { browser_->filterStep(1); });
    c.setHandler(QStringLiteral("FilterPrev"), [this]() { browser_->filterStep(-1); });
    c.setHandler(QStringLiteral("FilterAll"), [this]() { browser_->filterAll(); });
    c.setHandler(QStringLiteral("SortCycle"), [this]() { browser_->cycleSort(); });
    c.setHandler(QStringLiteral("SortReverse"), [this]() { browser_->reverseSort(); });
    c.setHandler(QStringLiteral("ToggleViewStyle"), [this]() { browser_->toggleViewStyle(); });
    c.setHandler(QStringLiteral("ViewDetails"), [this]() { browser_->setViewMode(ViewMode::Details); syncCheckStates(); updateBrowserStatus(); });

    // 勾选类命令:改配置后重新应用可见性
    c.setHandler(QStringLiteral("ToggleToolbar"), [this]() {
        const bool v = Commands::instance().action(QStringLiteral("ToggleToolbar"))->isChecked();
        Config::instance().setBool(mode_ == WindowMode::Browser ? QStringLiteral("Browse/ShowToolbar") : QStringLiteral("Viewer/ShowToolbar"), v);
        applyVisibility();
    });
    c.setHandler(QStringLiteral("ToggleStatusBar"), [this]() {
        const bool v = Commands::instance().action(QStringLiteral("ToggleStatusBar"))->isChecked();
        Config::instance().setBool(mode_ == WindowMode::Browser ? QStringLiteral("Browse/ShowStatusBar") : QStringLiteral("Viewer/ShowStatusBar"), v);
        applyVisibility();
    });
    c.setHandler(QStringLiteral("ToggleMenu"), [this]() {
        const bool v = Commands::instance().action(QStringLiteral("ToggleMenu"))->isChecked();
        Config::instance().setBool(mode_ == WindowMode::Browser ? QStringLiteral("Browse/ShowMenu") : QStringLiteral("Viewer/ShowMenu"), v);
        applyVisibility();
    });
    c.setHandler(QStringLiteral("ToggleInfo"), [this]() {
        Config::instance().setBool(QStringLiteral("Browse/ShowInfoPanel"), Commands::instance().action(QStringLiteral("ToggleInfo"))->isChecked());
        applyVisibility();
    });
    {   // 信息面板各部分的显示开关:改 [Info] 里对应的 ShowXxx,并立刻刷新面板
        static const char* const kInfoToggles[][2] = {
            {"ToggleInfoPreview", "Info/ShowPreview"}, {"ToggleInfoBasic", "Info/ShowBasic"},
            {"ToggleInfoExif", "Info/ShowExif"}, {"ToggleInfoHistogram", "Info/ShowHistogram"}};
        for (int i = 0; i < 4; ++i) {
            const QString id = QLatin1String(kInfoToggles[i][0]);
            const QString key = QLatin1String(kInfoToggles[i][1]);
            c.setHandler(id, [this, id, key]() {
                Config::instance().setBool(key, Commands::instance().action(id)->isChecked());
                browser_->infoPanel()->applyOptions();
            });
        }
    }
    c.setHandler(QStringLiteral("TogglePathBar"), [this]() {
        Config::instance().setBool(QStringLiteral("Browse/ShowPathBar"), Commands::instance().action(QStringLiteral("TogglePathBar"))->isChecked());
        applyVisibility();
    });
    c.setHandler(QStringLiteral("ToggleTree"), [this]() {
        Config::instance().setBool(QStringLiteral("Browse/ShowTree"), Commands::instance().action(QStringLiteral("ToggleTree"))->isChecked());
        applyVisibility();
    });
    c.setHandler(QStringLiteral("TogglePreview"), [this]() {
        Config::instance().setBool(QStringLiteral("Browse/ShowPreview"), Commands::instance().action(QStringLiteral("TogglePreview"))->isChecked());
        applyVisibility();
    });
    c.setHandler(QStringLiteral("AlwaysOnTop"), [this]() {
        Config::instance().setBool(QStringLiteral("Viewer/AlwaysOnTop"), Commands::instance().action(QStringLiteral("AlwaysOnTop"))->isChecked());
        toggleAlwaysOnTop();
    });
    c.setHandler(QStringLiteral("Options"), [this]() { cmdOptions(); });
    c.setHandler(QStringLiteral("ManageExtensions"), [this]() { cmdManageExtensions(); });
    c.setHandler(QStringLiteral("FileAssoc"), [this]() { cmdFileAssoc(); });
    c.setHandler(QStringLiteral("About"), [this]() { cmdAbout(); });
    c.setHandler(QStringLiteral("CutFile"), [this]() { cmdCutFile(); });
    c.setHandler(QStringLiteral("Paste"), [this]() { cmdPaste(); });
    c.setHandler(QStringLiteral("NewFolder"), [this]() { cmdNewFolder(); });
    c.setHandler(QStringLiteral("ThumbReset"), [this]() { browser_->resetThumbSize(); });
    c.setHandler(QStringLiteral("CopyImageSel"), [this]() { cmdCopyImage(); });
    c.setHandler(QStringLiteral("EditDescription"), [this]() { cmdEditDescription(); });
    c.setHandler(QStringLiteral("ReloadImage"), [this]() { cmdReloadImage(); });
    c.setHandler(QStringLiteral("SaveAs"), [this]() { cmdSaveAs(); });
    c.setHandler(QStringLiteral("CloseImage"), [this]() { leaveViewer(); });
    c.setHandler(QStringLiteral("ZoomLock"), [this]() {
        const bool v = Commands::instance().action(QStringLiteral("ZoomLock"))->isChecked();
        Config::instance().setBool(QStringLiteral("Viewer/ZoomLock"), v);
        viewer_->setZoomLock(v);
    });
    c.setHandler(QStringLiteral("RotateBy"), [this]() { cmdRotateBy(); });

    // 自定义命令:个数不限,统一走 runCustomCommand
    const QStringList all = c.ids();
    for (int i = 0; i < all.size(); ++i) {
        const QString id = all.at(i);
        if (CustomCommands::instance().has(id)) {
            c.setHandler(id, [this, id]() { runCustomCommand(id); });
        }
    }
}

// 运行一条自定义命令:{file}=当前图片(浏览窗口=当前文件)  {folder}=当前文件夹  {selected}=选中的全部文件
void MainWindow::runCustomCommand(const QString& id)
{
    QString file;
    QString folder;
    const QStringList sel = targetPaths();
    if (mode_ == WindowMode::Viewer) {
        file = currentPath_;
        if (!file.isEmpty()) folder = QFileInfo(file).absolutePath();
    } else {
        file = browser_->currentFilePath();
        if (file.isEmpty() && !sel.isEmpty()) file = sel.first();
        folder = browser_->folderList()->folder();
    }
    QString program;
    QStringList args;
    QString error;
    if (!CustomCommands::instance().build(id, file, folder, sel, &program, &args, &error)) {
        showError(error);
        return;
    }
    if (!QProcess::startDetached(program, args, folder)) {
        showError(S("err.customRun").arg(program).arg(id));
    }
}

// 按 [MenuBrowser]/[MenuViewer] 重建菜单栏
void MainWindow::rebuildMenus()
{
    Config& cfg = Config::instance();
    Commands& cmds = Commands::instance();
    const QString sec = (mode_ == WindowMode::Browser) ? QStringLiteral("MenuBrowser") : QStringLiteral("MenuViewer");
    QMenuBar* bar = menuBar();
    const QList<QMenu*> old = bar->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(old);
    bar->clear();
    const QStringList order = cfg.getList(sec + QStringLiteral("/Order"));
    for (int i = 0; i < order.size(); ++i) {
        const QString name = order.at(i);
        QMenu* m = bar->addMenu(S(QStringLiteral("menu.") + name));
        const QString where = sec + QLatin1Char('/') + name;
        const QStringList items = cfg.getList(where);
        for (int k = 0; k < items.size(); ++k) {
            const QString id = items.at(k);
            if (id == QLatin1String("|")) {
                m->addSeparator();
                continue;
            }
            if (!cmds.has(id)) Config::fatal(S("err.unknownCommand").arg(id).arg(where));
            m->addAction(cmds.action(id));
        }
    }
}

// 按 [Toolbar] 重建工具栏
void MainWindow::rebuildToolbar()
{
    Config& cfg = Config::instance();
    Commands& cmds = Commands::instance();
    const QString key = (mode_ == WindowMode::Browser) ? QStringLiteral("Toolbar/Browser") : QStringLiteral("Toolbar/Viewer");
    browser_->undockPathBar();  // 先把路径栏取出来,否则 clear 会把它一起删掉
    toolbar_->clear();
    toolbar_->setIconSize(QSize(cfg.getInt(QStringLiteral("Toolbar/IconSize")), cfg.getInt(QStringLiteral("Toolbar/IconSize"))));
    const QStringList items = cfg.getList(key);
    for (int i = 0; i < items.size(); ++i) {
        const QString id = items.at(i);
        if (id == QLatin1String("|")) {
            toolbar_->addSeparator();
            continue;
        }
        if (id == QLatin1String("@PathBar")) {
            if (mode_ != WindowMode::Browser) Config::fatal(S("err.toolbarPathBarViewer").arg(Config::iniPath()).arg(cfg.lineNumber(key)).arg(key));
            browser_->dockPathBar(toolbar_);
            continue;
        }
        if (id == QLatin1String("@Stretch")) {
            QWidget* sp = new QWidget(toolbar_);
            sp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            toolbar_->addWidget(sp);
            continue;
        }
        if (!cmds.has(id)) Config::fatal(S("err.unknownCommand").arg(id).arg(key));
        QAction* a = cmds.action(id);
        QString tip = cmds.tipOf(id);
        const QString keys = a->shortcut().toString(QKeySequence::NativeText);
        if (!keys.isEmpty()) tip += QStringLiteral(" (") + keys + QStringLiteral(")");
        a->setToolTip(tip);
        toolbar_->addAction(a);
        QWidget* w = toolbar_->widgetForAction(a);
        if (w != nullptr) w->installEventFilter(this);
    }
}

// 建立状态栏:浏览 4 个字段,查看 6 个字段
void MainWindow::buildStatusBar()
{
    QStatusBar* bar = statusBar();
    bar->setSizeGripEnabled(true);
    for (int i = 0; i < 6; ++i) {
        if (statusLabels_[i] == nullptr) {
            statusLabels_[i] = new QLabel(this);
            statusLabels_[i]->setFrameStyle(Config::instance().getBool(QStringLiteral("StatusBar/Frames")) ? (QFrame::StyledPanel | QFrame::Sunken) : QFrame::NoFrame);
            statusLabels_[i]->setMargin(2);
        } else {
            bar->removeWidget(statusLabels_[i]);
        }
    }
    // 左侧按键提示条(不强制撑宽窗口;关闭时隐藏,伸缩权交还给文件名栏)
    if (hintLabel_ == nullptr) {
        hintLabel_ = new QLabel(this);
        hintLabel_->setObjectName(QStringLiteral("hintLabel"));
        hintLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        hintLabel_->setMargin(2);
    } else {
        bar->removeWidget(hintLabel_);
    }
    const bool hints = hintsVisible();
    bar->addWidget(hintLabel_, hints ? 1 : 0);
    hintLabel_->setVisible(hints);
    if (mode_ == WindowMode::Browser) {
        // 提示 | 总计 | 选中大小和时间 | 选中文件名(拉伸) | 图片信息
        bar->addWidget(statusLabels_[0], 0);
        bar->addWidget(statusLabels_[1], 0);
        bar->addWidget(statusLabels_[2], hints ? 0 : 1);
        bar->addWidget(statusLabels_[3], 0);
        statusLabels_[0]->setMinimumWidth(170);
        statusLabels_[1]->setMinimumWidth(190);
        statusLabels_[3]->setMinimumWidth(190);
        statusLabels_[0]->show();
        statusLabels_[1]->show();
        statusLabels_[2]->show();
        statusLabels_[3]->show();
    } else {
        // 序号 | 文件名(拉伸) | 大小 | 图片信息 | 缩放 | 载入时间
        bar->addWidget(statusLabels_[0], 0);
        bar->addWidget(statusLabels_[1], hints ? 0 : 1);
        bar->addWidget(statusLabels_[2], 0);
        bar->addWidget(statusLabels_[3], 0);
        bar->addWidget(statusLabels_[4], 0);
        bar->addWidget(statusLabels_[5], 0);
        statusLabels_[0]->setMinimumWidth(70);
        statusLabels_[1]->setMinimumWidth(0);
        statusLabels_[2]->setMinimumWidth(80);
        statusLabels_[3]->setMinimumWidth(190);
        statusLabels_[4]->setMinimumWidth(60);
        statusLabels_[5]->setMinimumWidth(110);
        for (int i = 0; i < 6; ++i) statusLabels_[i]->show();
    }
}

// 切换模式
void MainWindow::setMode(WindowMode mode)
{
    if (isVisible() && !fullscreen_) saveGeometry(mode_);
    mode_ = mode;
    stack_->setCurrentWidget(mode == WindowMode::Browser ? static_cast<QWidget*>(browser_) : static_cast<QWidget*>(viewer_));
    Commands::instance().setMode(mode);
    rebuildMenus();
    rebuildToolbar();
    buildStatusBar();
    applyVisibility();
    toggleAlwaysOnTop();
    syncCheckStates();
    restoreGeometry(mode);
    updateTitle();
    updateHint();
    if (mode == WindowMode::Browser) {
        updateBrowserStatus();
        browser_->focusFileArea();  // 浏览窗口本身 setFocus 没用,方向键选文件要靠里面真正的视图拿到焦点
    } else {
        viewer_->setFocus();
    }
}

// 打开 path 所在文件夹并生成查看列表(enterViewer 和以后的跳转共用)
bool MainWindow::prepareViewList(const QString& path, int* index)
{
    Config& cfg = Config::instance();
    const QFileInfo fi(path);
    FolderList* fl = browser_->folderList();
    if (fl->imageIndexOf(path) < 0) {
        if (!browser_->openFolder(fi.absolutePath())) return false;  // 浏览窗口已弹出错误
        if (fl->imageIndexOf(path) < 0) {
            QString err;
            if (!fl->addExtraFile(path, &err)) {
                showError(err);
                return false;
            }
        }
    }
    int idx = 0;
    if (cfg.getBool(QStringLiteral("Viewer/ViewAllInFolder"))) {
        viewList_ = fl->imageEntries();
        idx = fl->imageIndexOf(path);
        if (idx < 0) {
            showError(S("err.imageLoad").arg(path).arg(S("err.fileNotFound")));
            return false;
        }
    } else {
        FileEntry e;
        e.path = path;
        e.name = fi.fileName();
        e.isImage = true;
        e.size = fi.size();
        e.modified = fi.lastModified();
        viewList_.clear();
        viewList_.append(e);
    }
    lastBrowserFolder_ = fl->folder();
    *index = idx;
    return true;
}

// 进入查看模式并显示 path
void MainWindow::enterViewer(const QString& pathIn)
{
    Config& cfg = Config::instance();
    const QString path = QDir::fromNativeSeparators(QFileInfo(pathIn).absoluteFilePath());
    const QFileInfo fi(path);
    if (!fi.isFile()) {
        showError(S("err.readFile").arg(path).arg(S("err.fileNotFound")));
        return;
    }
    int idx = 0;
    if (!prepareViewList(path, &idx)) return;
    setMode(WindowMode::Viewer);
    if (cfg.getBool(QStringLiteral("Viewer/Fullscreen"))) setFullscreen(true);
    gotoImage(idx);
}

// 回到浏览模式
void MainWindow::leaveViewer()
{
    if (mode_ != WindowMode::Viewer) return;
    const QString p = currentPath_;
    settleTimer_->stop();
    if (fullscreen_) setFullscreen(false);
    ImageLoader::instance().cancelPending(QStringList());
    viewer_->clear();
    viewList_.clear();
    currentIndex_ = -1;
    currentPath_.clear();
    setMode(WindowMode::Browser);
    if (!p.isEmpty()) {
        // 停在刚才看的那张并滚动到可见;窗口尺寸恢复后再滚一次,避免布局未完成时没滚到位
        browser_->selectPath(p);
        QTimer::singleShot(0, this, [this, p]() { if (mode_ == WindowMode::Browser) browser_->selectPath(p); });
    }
}

// 跳到第 index 张并预加载前后
void MainWindow::gotoImage(int index)
{
    if (index < 0 || index >= viewList_.size()) return;
    currentIndex_ = index;
    currentPath_ = viewList_.at(index).path;
    loadTimer_.start();
    ImageLoader& loader = ImageLoader::instance();
    const int n = Config::instance().getInt(QStringLiteral("Viewer/PreloadCount"));
    QStringList pre;
    for (int k = 1; k <= n; ++k) {
        if (index + k < viewList_.size()) pre.append(viewList_.at(index + k).path);
        if (index - k >= 0) pre.append(viewList_.at(index - k).path);
    }
    QStringList keep = pre;
    keep.append(currentPath_);
    loader.cancelPending(keep);
    updateTitle();
    if (statusLabels_[0] != nullptr) {
        statusLabels_[0]->setText(S("status.index").arg(currentIndex_ + 1).arg(viewList_.size()));
        statusLabels_[1]->setText(viewList_.at(index).name);
        statusLabels_[5]->setText(S("status.loading"));
    }
    LoadResult r;
    if (loader.getCached(currentPath_, &r)) onImageLoaded(r);
    // 没缓存的图和预加载都等"停稳"后再做:按住方向键连续翻页时,中间的图不会被解码
    const int delay = viewer_->hasImage() ? Config::instance().getInt(QStringLiteral("Viewer/FlipDecodeDelayMs")) : 0;
    if (delay <= 0) settleFlip();
    else settleTimer_->start(delay);
}

// 停稳后真正请求当前图(没缓存时)并预加载前后几张
void MainWindow::settleFlip()
{
    if (mode_ != WindowMode::Viewer || currentIndex_ < 0 || currentIndex_ >= viewList_.size()) return;
    ImageLoader& loader = ImageLoader::instance();
    LoadResult r;
    if (!loader.getCached(currentPath_, &r)) loader.request(currentPath_, 10);
    const int n = Config::instance().getInt(QStringLiteral("Viewer/PreloadCount"));
    QStringList pre;
    for (int k = 1; k <= n; ++k) {
        if (currentIndex_ + k < viewList_.size()) pre.append(viewList_.at(currentIndex_ + k).path);
        if (currentIndex_ - k >= 0) pre.append(viewList_.at(currentIndex_ - k).path);
    }
    if (!pre.isEmpty()) loader.prefetch(pre);
}

// 下一张;到末尾按 Viewer/Wrap 循环回第一张
void MainWindow::nextImage()
{
    if (viewList_.size() <= 1) return;
    if (currentIndex_ + 1 < viewList_.size()) gotoImage(currentIndex_ + 1);
    else if (Config::instance().getBool(QStringLiteral("Viewer/Wrap"))) gotoImage(0);
}

// 上一张;到第一张按 Viewer/Wrap 循环到最后一张
void MainWindow::prevImage()
{
    if (viewList_.size() <= 1) return;
    if (currentIndex_ > 0) gotoImage(currentIndex_ - 1);
    else if (Config::instance().getBool(QStringLiteral("Viewer/Wrap"))) gotoImage(viewList_.size() - 1);
}

// 方向键:图片在该方向比窗口大(且 Viewer/ArrowPan 开)就平移,否则翻页
void MainWindow::arrowKey(int dx, int dy)
{
    Config& cfg = Config::instance();
    if (cfg.getBool(QStringLiteral("Viewer/ArrowPan")) && viewer_->canPan(dx != 0)) {
        const int step = cfg.getInt(QStringLiteral("Viewer/PanStep"));
        viewer_->panBy(-dx * step, -dy * step);
        return;
    }
    if (dx < 0 || dy < 0) prevImage();
    else nextImage();
}

// 跳到上/下一个有图片的文件夹(同一父文件夹下按名称排序),显示它的第一张;到头按 Viewer/Wrap 循环
void MainWindow::jumpFolder(int dir)
{
    if (mode_ != WindowMode::Viewer) return;
    Config& cfg = Config::instance();
    const QDir cur(browser_->currentFolder());
    QDir parent(cur);
    if (!parent.cdUp()) {
        showError(S("err.noParentFolder").arg(QDir::toNativeSeparators(cur.absolutePath())));
        return;
    }
    QDir::Filters f = QDir::Dirs | QDir::NoDotAndDotDot;
    const bool hidden = cfg.getBool(QStringLiteral("FileList/ShowHidden"));
    if (hidden) f |= QDir::Hidden;
    const QStringList dirs = parent.entryList(f, QDir::Name | QDir::IgnoreCase);
    const int n = dirs.size();
    int at = -1;
    for (int i = 0; i < n; ++i) {
        if (dirs.at(i).compare(cur.dirName(), Qt::CaseInsensitive) == 0) at = i;
    }
    if (at < 0) {
        showError(S("err.noOtherImageFolder"));
        return;
    }
    const bool wrap = cfg.getBool(QStringLiteral("Viewer/Wrap"));
    for (int step = 1; step < n; ++step) {
        int k = at + dir * step;
        if (k < 0 || k >= n) {
            if (!wrap) break;
            k = ((k % n) + n) % n;
        }
        const QString folder = parent.absoluteFilePath(dirs.at(k));
        QDir::Filters ff = QDir::Files;
        if (hidden) ff |= QDir::Hidden;
        const QStringList files = QDir(folder).entryList(ff);
        bool hasImage = false;
        for (int j = 0; j < files.size() && !hasImage; ++j) hasImage = FolderList::isImagePath(files.at(j));
        if (!hasImage) continue;
        if (!browser_->openFolder(folder)) return;  // 浏览窗口已弹出错误
        const QList<FileEntry> ents = browser_->folderList()->imageEntries();
        if (ents.isEmpty()) continue;
        viewList_.clear();
        if (cfg.getBool(QStringLiteral("Viewer/ViewAllInFolder"))) viewList_ = ents;
        else viewList_.append(ents.first());
        lastBrowserFolder_ = browser_->currentFolder();
        gotoImage(0);
        return;
    }
    showError(S("err.noOtherImageFolder"));
}

// 第一张
void MainWindow::firstImage()
{
    gotoImage(0);
}

// 最后一张
void MainWindow::lastImage()
{
    gotoImage(viewList_.size() - 1);
}

// 当前查看的图片路径
QString MainWindow::currentImagePath() const
{
    if (mode_ != WindowMode::Viewer) return QString();
    return currentPath_;
}

// 全屏命令:改配置(下次进入查看窗口仍全屏)并应用
void MainWindow::toggleFullscreen()
{
    const bool v = Commands::instance().action(QStringLiteral("Fullscreen"))->isChecked();
    Config::instance().setBool(QStringLiteral("Viewer/Fullscreen"), v);
    setFullscreen(v);
}

// 进入/退出全屏
void MainWindow::setFullscreen(bool on)
{
    if (on == fullscreen_) return;
    if (on) {
        if (isVisible()) saveGeometry(WindowMode::Viewer);
        fullscreen_ = true;
        showFullScreen();
        if (Config::instance().getBool(QStringLiteral("Viewer/HideMouseInFullscreen"))) hideCursorTimer_->start();
    } else {
        fullscreen_ = false;
        hideCursorTimer_->stop();
        viewer_->setCursorHidden(false);
        showNormal();
        restoreGeometry(mode_);
    }
    applyVisibility();
    syncCheckStates();
    // 窗口大小变了,ViewerWidget 在缩放是自动算出的情况下会自己重新适应;手动缩放过的保持不变
}

// 按配置应用总在最上(只在查看模式生效)
void MainWindow::toggleAlwaysOnTop()
{
    const bool want = Config::instance().getBool(QStringLiteral("Viewer/AlwaysOnTop")) && mode_ == WindowMode::Viewer;
    const bool has = (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
    if (want == has) return;
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, want);
    if (wasVisible) show();
}

// 窗口按图片尺寸自动调整
void MainWindow::autoResizeToImage()
{
    if (!Config::instance().getBool(QStringLiteral("Viewer/AutoResizeWindow"))) return;
    if (fullscreen_ || isMaximized() || mode_ != WindowMode::Viewer || !isVisible()) return;
    const QSize img = viewer_->displaySizeAt100();
    if (img.isEmpty()) return;
    QScreen* sc = screen();
    if (sc == nullptr) sc = QGuiApplication::primaryScreen();
    const QRect avail = sc->availableGeometry();
    const QSize frame = frameGeometry().size() - geometry().size();
    const QSize extra = size() - viewer_->size();
    QSize target = img + extra;
    target = target.boundedTo(avail.size() - frame);
    target = target.expandedTo(QSize(320, 200));
    resize(target);
    if (Config::instance().getBool(QStringLiteral("Viewer/CenterOnResize"))) {
        const QSize total = target + frame;
        move(avail.center() - QPoint(total.width() / 2, total.height() / 2));
    }
}

// 更新标题栏
void MainWindow::updateTitle()
{
    Config& cfg = Config::instance();
    QString name;
    if (mode_ == WindowMode::Browser) {
        const QString f = browser_->currentFolder();
        if (cfg.getBool(QStringLiteral("Browse/FullPathInTitle"))) {
            name = QDir::toNativeSeparators(f);
        } else {
            name = QFileInfo(f).fileName();
            if (name.isEmpty()) name = QDir::toNativeSeparators(f);
        }
    } else if (!currentPath_.isEmpty()) {
        if (cfg.getBool(QStringLiteral("Viewer/ShowFullPath"))) name = QDir::toNativeSeparators(currentPath_);
        else name = QFileInfo(currentPath_).fileName();
    }
    if (name.isEmpty()) {
        setWindowTitle(S("app.name"));
        return;
    }
    if (cfg.getBool(QStringLiteral("Misc/TitleShowVersion"))) {
        setWindowTitle(S("app.titleWithVersion").arg(name).arg(S("app.version")));
    } else {
        setWindowTitle(S("app.titleFormat").arg(name));
    }
}

// 更新浏览状态栏
void MainWindow::updateBrowserStatus()
{
    const QStringList f = browser_->statusFields();
    for (int i = 0; i < 4; ++i) {
        statusLabels_[i]->setText(i < f.size() ? f.at(i) : QString());
    }
}

// 更新查看状态栏(6 个字段)
void MainWindow::updateViewerStatus(const LoadResult& r)
{
    const QFileInfo fi(r.path);
    statusLabels_[0]->setText(S("status.index").arg(currentIndex_ + 1).arg(viewList_.size()));
    statusLabels_[1]->setText(fi.fileName());
    const qint64 bytes = r.info.valid ? r.info.fileSize : fi.size();
    statusLabels_[2]->setText(sizeText(bytes));
    statusLabels_[3]->setText(r.ok && r.info.valid ? imageInfoLine(r.info, true) : QString());
    statusLabels_[4]->setText(S("status.zoom").arg(qRound(viewer_->zoom() * 100.0)));
    const double sec = loadTimer_.isValid() ? loadTimer_.nsecsElapsed() / 1.0e9 : r.seconds;
    statusLabels_[5]->setText(S("status.loadTime").arg(QString::number(sec, 'f', 1)));
}

// 应用显示设置:工具栏/状态栏/菜单栏/路径栏/目录树/预览
void MainWindow::applyVisibility()
{
    Config& cfg = Config::instance();
    if (mode_ == WindowMode::Browser) {
        menuBar()->setVisible(cfg.getBool(QStringLiteral("Browse/ShowMenu")));
        toolbar_->setVisible(cfg.getBool(QStringLiteral("Browse/ShowToolbar")));
        statusBar()->setVisible(cfg.getBool(QStringLiteral("Browse/ShowStatusBar")));
        browser_->setPathBarVisible(cfg.getBool(QStringLiteral("Browse/ShowPathBar")));
        browser_->setTreeVisible(cfg.getBool(QStringLiteral("Browse/ShowTree")));
        browser_->setPreviewVisible(cfg.getBool(QStringLiteral("Browse/ShowPreview")));
        browser_->setInfoVisible(cfg.getBool(QStringLiteral("Browse/ShowInfoPanel")));
    } else {
        const bool fs = fullscreen_;
        menuBar()->setVisible(!fs && cfg.getBool(QStringLiteral("Viewer/ShowMenu")));
        toolbar_->setVisible(!fs && cfg.getBool(QStringLiteral("Viewer/ShowToolbar")));
        statusBar()->setVisible(!fs && cfg.getBool(QStringLiteral("Viewer/ShowStatusBar")));
    }
}

// Windows 10/11 深色标题栏:Display/DarkTitleBar = Auto(按主题背景色深浅)/On/Off;其他系统不处理
void MainWindow::applyTitleBar()
{
#ifdef Q_OS_WIN
    const QString mode = Config::instance().getEnum(QStringLiteral("Display/DarkTitleBar"));
    bool dark = false;
    if (mode == QLatin1String("On")) {
        dark = true;
    } else if (mode == QLatin1String("Auto")) {
        dark = Theme::color(QStringLiteral("bg")).lightnessF() < 0.5;
    }
    const BOOL v = dark ? TRUE : FALSE;
    const HWND h = reinterpret_cast<HWND>(winId());
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE(旧 Win10 内部版本用 19,系统不认时返回失败,这里再试一次)
    if (FAILED(DwmSetWindowAttribute(h, 20, &v, sizeof(v)))) DwmSetWindowAttribute(h, 19, &v, sizeof(v));
#endif
}

// 删除(浏览:选中项;查看:当前图)
void MainWindow::cmdDelete()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    QString err;
    if (!FileOps::deleteFiles(this, paths, &err)) {
        if (!err.isEmpty()) showError(err);
        return;
    }
    if (mode_ == WindowMode::Browser) {
        browser_->refresh();
    } else {
        browser_->folderList()->refresh();
        if (Config::instance().getBool(QStringLiteral("Viewer/ExitAfterDelete"))) leaveViewer();
    }
}

// 重命名
void MainWindow::cmdRename()
{
    if (mode_ == WindowMode::Browser) {
        browser_->startRename();
        return;
    }
    if (currentPath_.isEmpty()) return;
    QString newPath;
    QString err;
    if (!FileOps::renameWithDialog(this, currentPath_, &newPath, &err)) {
        if (!err.isEmpty()) showError(err);
        return;
    }
    currentPath_ = newPath;  // 先改当前路径,刷新后列表里能找到它
    browser_->folderList()->refresh();
    updateTitle();
}

// 复制到
void MainWindow::cmdCopyTo()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    QString err;
    if (!FileOps::copyOrMoveWithDialog(this, paths, false, &err) && !err.isEmpty()) showError(err);
    else browser_->folderList()->refresh();
}

// 移动到
void MainWindow::cmdMoveTo()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    QString err;
    if (!FileOps::copyOrMoveWithDialog(this, paths, true, &err)) {
        if (!err.isEmpty()) showError(err);
        return;
    }
    browser_->folderList()->refresh();
}

// 复制文件到剪贴板
void MainWindow::cmdCopyFile()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    FileOps::copyFilesToClipboard(paths);
}

// 复制图片到剪贴板:查看窗口为当前图(含旋转);浏览窗口为选中的第一张
void MainWindow::cmdCopyImage()
{
    QString path;
    if (mode_ == WindowMode::Viewer) {
        path = currentPath_;
    } else {
        const QStringList t = targetPaths();
        if (!t.isEmpty()) path = t.first();
    }
    if (path.isEmpty() || !FolderList::isImagePath(path)) return;
    LoadResult r;
    if (!ImageLoader::instance().getCached(path, &r)) r = ImageLoader::instance().loadNow(path);
    if (!r.ok) {
        showError(S("err.clipboard").arg(r.error));
        return;
    }
    QImage img = r.image;
    if (mode_ == WindowMode::Viewer && viewer_->rotation() != 0) img = img.transformed(QTransform().rotate(viewer_->rotation()));
    FileOps::copyImageToClipboard(img);
}

// 粘贴:把剪贴板里的文件复制/移动到当前文件夹(重名按设置 Archive/DuplicateAction 处理)
void MainWindow::cmdPaste()
{
    if (mode_ != WindowMode::Browser) return;
    const QMimeData* md = QApplication::clipboard()->mimeData();
    QStringList paths;
    if (md != nullptr && md->hasUrls()) {
        const QList<QUrl> urls = md->urls();
        for (int i = 0; i < urls.size(); ++i) {
            if (urls.at(i).isLocalFile()) paths << urls.at(i).toLocalFile();
        }
    }
    if (paths.isEmpty()) {
        showError(S("err.pasteEmpty"));
        return;
    }
    const QByteArray effect = md->data(QStringLiteral("Preferred DropEffect"));
    const bool move = !effect.isEmpty() && effect.at(0) == 2;  // 2 = 剪切
    QString err;
    if (!FileOps::copyOrMove(this, paths, browser_->currentFolder(), move, &err)) {
        if (!err.isEmpty()) showError(err);
        browser_->folderList()->refresh();
        return;
    }
    if (move) QApplication::clipboard()->clear();
    browser_->folderList()->refresh();
}

// 新建文件夹:在当前文件夹里建一个子文件夹
void MainWindow::cmdNewFolder()
{
    if (mode_ != WindowMode::Browser) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, S("dlg.newfolder.title"), S("dlg.newfolder.label"), QLineEdit::Normal,
                                               S("dlg.newfolder.default"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QDir dir(browser_->currentFolder());
    if (QFileInfo::exists(dir.absoluteFilePath(name))) {
        showError(S("err.folderExists").arg(QDir::toNativeSeparators(dir.absoluteFilePath(name))));
        return;
    }
    if (!dir.mkdir(name)) {
        showError(S("err.mkdir").arg(QDir::toNativeSeparators(dir.absoluteFilePath(name))));
        return;
    }
    browser_->refresh();
}

// 剪切文件到剪贴板
void MainWindow::cmdCutFile()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    FileOps::cutFilesToClipboard(paths);
}

// 编辑选中文件的描述(写入该文件夹的 descript.ion)
void MainWindow::cmdEditDescription()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    const QFileInfo fi(paths.first());
    if (!fi.isFile()) return;
    DescriptIon d(fi.absolutePath());
    QString err;
    if (!d.load(&err)) {
        showError(err);
        return;
    }
    bool ok = false;
    const QString text = QInputDialog::getText(this, S("dlg.desc.title"), S("dlg.desc.label"), QLineEdit::Normal, d.get(fi.fileName()), &ok);
    if (!ok) return;
    d.set(fi.fileName(), text);
    if (!d.save(&err)) {
        showError(err);
        return;
    }
    browser_->refresh();
}

// 重新载入当前图(清缓存后重新读文件)
void MainWindow::cmdReloadImage()
{
    if (mode_ != WindowMode::Viewer || currentPath_.isEmpty()) return;
    ImageLoader::instance().clearCache();
    loadTimer_.start();
    if (statusLabels_[5] != nullptr) statusLabels_[5]->setText(S("status.loading"));
    ImageLoader::instance().request(currentPath_, 10);
}

// 另存为:把当前图(含旋转)存成 PNG/JPEG/BMP
void MainWindow::cmdSaveAs()
{
    if (mode_ != WindowMode::Viewer || currentPath_.isEmpty()) return;
    LoadResult r;
    if (!ImageLoader::instance().getCached(currentPath_, &r)) r = ImageLoader::instance().loadNow(currentPath_);
    if (!r.ok) {
        showError(S("err.saveImage").arg(currentPath_).arg(r.error));
        return;
    }
    const QFileInfo fi(currentPath_);
    const QString start = fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() + QStringLiteral(".png");
    const QString dest = QFileDialog::getSaveFileName(this, S("dlg.saveAs.title"), start, S("dlg.saveAs.filter"));
    if (dest.isEmpty()) return;
    QImage img = r.image;
    if (viewer_->rotation() != 0) img = img.transformed(QTransform().rotate(viewer_->rotation()));
    if (!img.save(dest)) showError(S("err.saveImage").arg(dest).arg(S("err.saveFailed")));
}

// 旋转...:选择向右转 90/180/向左转 90(只改显示)
void MainWindow::cmdRotateBy()
{
    if (mode_ != WindowMode::Viewer) return;
    QStringList items;
    items << S("dlg.rotate.r90") << S("dlg.rotate.r180") << S("dlg.rotate.l90");
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, S("dlg.rotate.title"), S("dlg.rotate.label"), items, 0, false, &ok);
    if (!ok) return;
    if (pick == items.at(0)) {
        viewer_->rotateRight();
    } else if (pick == items.at(1)) {
        viewer_->rotateRight();
        viewer_->rotateRight();
    } else {
        viewer_->rotateLeft();
    }
}

// 属性
void MainWindow::cmdProperties()
{
    const QStringList paths = targetPaths();
    if (paths.isEmpty()) return;
    FileOps::showProperties(this, paths.first());
}

// 打印当前图(浏览:当前项)
void MainWindow::cmdPrint()
{
    QString path = (mode_ == WindowMode::Viewer) ? currentPath_ : browser_->currentFilePath();
    if (path.isEmpty() || !FolderList::isImagePath(path)) return;
    LoadResult r;
    if (!ImageLoader::instance().getCached(path, &r)) r = ImageLoader::instance().loadNow(path);
    if (!r.ok) {
        showError(S("err.print").arg(r.error));
        return;
    }
    QImage img = r.image;
    if (mode_ == WindowMode::Viewer && viewer_->rotation() != 0) {
        img = img.transformed(QTransform().rotate(viewer_->rotation()));
    }
    QPrinter printer(QPrinter::HighResolution);
    if (Config::instance().getBool(QStringLiteral("Misc/PrintShowSetup"))) {
        QPrintDialog dlg(&printer, this);
        dlg.setWindowTitle(S("dlg.print.title"));
        if (dlg.exec() != QDialog::Accepted) return;
    }
    QPainter painter;
    if (!painter.begin(&printer)) {
        showError(S("err.print").arg(QFileInfo(path).fileName()));
        return;
    }
    const QRect page = printer.pageRect(QPrinter::DevicePixel).toRect();
    const QSize sz = img.size().scaled(page.size(), Qt::KeepAspectRatio);
    const QRect target(page.x() + (page.width() - sz.width()) / 2, page.y() + (page.height() - sz.height()) / 2,
                       sz.width(), sz.height());
    painter.drawImage(target, img);
    painter.end();
}

// 打开文件对话框
void MainWindow::cmdOpenFile()
{
    QStringList pats;
    const QStringList exts = FolderList::imageExtensions();
    for (int i = 0; i < exts.size(); ++i) pats.append(QStringLiteral("*.") + exts.at(i));
    QString startDir = browser_->currentFolder();
    if (mode_ == WindowMode::Viewer && !currentPath_.isEmpty()) startDir = QFileInfo(currentPath_).absolutePath();
    const QString file = QFileDialog::getOpenFileName(this, S("dlg.openFile.title"), startDir,
                                                      S("dlg.openFile.filter").arg(pats.join(QLatin1Char(' '))));
    if (file.isEmpty()) return;
    openPaths(QStringList(file), true);
}

// 选项对话框
void MainWindow::cmdOptions()
{
    OptionsDialog dlg(this);
    connect(&dlg, &OptionsDialog::optionsApplied, this, &MainWindow::onOptionsApplied);
    dlg.exec();
}

// 管理图片扩展名:保存后立即重扫
void MainWindow::cmdManageExtensions()
{
    ExtensionsDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) browser_->folderList()->refresh();
}

// 设置文件关联
void MainWindow::cmdFileAssoc()
{
    if (QMessageBox::question(this, S("dlg.info.title"), S("dlg.assoc.confirm"),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    const QStringList exts = FolderList::imageExtensions();
    QString err;
    if (!FileAssoc::registerAll(exts, &err)) {
        showError(S("err.assoc").arg(err));
        return;
    }
    FileAssoc::notifyShell();
    QMessageBox::information(this, S("dlg.info.title"), S("dlg.assoc.done").arg(exts.size()));
}

// 关于
void MainWindow::cmdAbout()
{
    QMessageBox::about(this, S("dlg.about.title"), S("app.aboutText").arg(S("app.version")));
}

// 同步菜单里勾选类命令的状态
void MainWindow::syncCheckStates()
{
    Config& cfg = Config::instance();
    Commands& c = Commands::instance();
    const bool b = (mode_ == WindowMode::Browser);
    c.action(QStringLiteral("ToggleToolbar"))->setChecked(cfg.getBool(b ? QStringLiteral("Browse/ShowToolbar") : QStringLiteral("Viewer/ShowToolbar")));
    c.action(QStringLiteral("ToggleStatusBar"))->setChecked(cfg.getBool(b ? QStringLiteral("Browse/ShowStatusBar") : QStringLiteral("Viewer/ShowStatusBar")));
    c.action(QStringLiteral("ToggleMenu"))->setChecked(cfg.getBool(b ? QStringLiteral("Browse/ShowMenu") : QStringLiteral("Viewer/ShowMenu")));
    c.action(QStringLiteral("TogglePathBar"))->setChecked(cfg.getBool(QStringLiteral("Browse/ShowPathBar")));
    c.action(QStringLiteral("ToggleTree"))->setChecked(cfg.getBool(QStringLiteral("Browse/ShowTree")));
    c.action(QStringLiteral("TogglePreview"))->setChecked(cfg.getBool(QStringLiteral("Browse/ShowPreview")));
    c.action(QStringLiteral("ToggleInfo"))->setChecked(cfg.getBool(QStringLiteral("Browse/ShowInfoPanel")));
    c.action(QStringLiteral("ToggleInfoPreview"))->setChecked(cfg.getBool(QStringLiteral("Info/ShowPreview")));
    c.action(QStringLiteral("ToggleInfoBasic"))->setChecked(cfg.getBool(QStringLiteral("Info/ShowBasic")));
    c.action(QStringLiteral("ToggleInfoExif"))->setChecked(cfg.getBool(QStringLiteral("Info/ShowExif")));
    c.action(QStringLiteral("ToggleInfoHistogram"))->setChecked(cfg.getBool(QStringLiteral("Info/ShowHistogram")));
    c.action(QStringLiteral("ToggleKeyHints"))->setChecked(hintsVisible());
    c.action(QStringLiteral("AlwaysOnTop"))->setChecked(cfg.getBool(QStringLiteral("Viewer/AlwaysOnTop")));
    c.action(QStringLiteral("Fullscreen"))->setChecked(fullscreen_);
    c.action(QStringLiteral("ZoomLock"))->setChecked(cfg.getBool(QStringLiteral("Viewer/ZoomLock")));
    const ViewMode vm = browser_->viewMode();
    c.action(QStringLiteral("ViewThumbInfo"))->setChecked(vm == ViewMode::ThumbInfo);
    c.action(QStringLiteral("ViewThumbs"))->setChecked(vm == ViewMode::Thumbs);
    c.action(QStringLiteral("ViewIcons"))->setChecked(vm == ViewMode::Icons);
    c.action(QStringLiteral("ViewList"))->setChecked(vm == ViewMode::List);
    c.action(QStringLiteral("ViewDetails"))->setChecked(vm == ViewMode::Details);
}

// 提示条开关(按模式)
bool MainWindow::hintsVisible() const
{
    return Config::instance().getBool(mode_ == WindowMode::Browser ? QStringLiteral("Browse/ShowKeyHints")
                                                                    : QStringLiteral("Viewer/ShowKeyHints"));
}

// 模板里的 {命令ID} 换成该命令的第一个快捷键;命令不存在或没设键都明说
QString MainWindow::expandHint(const QString& tpl) const
{
    QString out;
    int i = 0;
    while (i < tpl.size()) {
        const int a = tpl.indexOf(QLatin1Char('{'), i);
        if (a < 0) { out += tpl.mid(i); break; }
        const int b = tpl.indexOf(QLatin1Char('}'), a);
        if (b < 0) Config::fatal(S("err.hintTemplate").arg(tpl));
        out += tpl.mid(i, a - i);
        const QString id = tpl.mid(a + 1, b - a - 1);
        if (!Commands::instance().has(id)) Config::fatal(S("err.hintCommand").arg(id).arg(tpl));
        const QList<QKeySequence> ks = Commands::instance().action(id)->shortcuts();
        out += ks.isEmpty() ? S("hint.noKey") : ks.first().toString(QKeySequence::NativeText);
        i = b + 1;
    }
    return out;
}

// 状态栏左侧提示:浏览模式按焦点区域,查看模式按图片是否放大到能平移
void MainWindow::updateHint()
{
    if (hintLabel_ == nullptr || !hintsVisible()) return;
    QString key;
    if (mode_ == WindowMode::Browser) {
        switch (browser_->focusArea()) {
        case FocusArea::Tree: key = QStringLiteral("hint.browser.tree"); break;
        case FocusArea::PathBar: key = QStringLiteral("hint.browser.path"); break;
        case FocusArea::Files: key = QStringLiteral("hint.browser.files"); break;
        }
    } else {
        const bool pan = Config::instance().getBool(QStringLiteral("Viewer/ArrowPan"))
                         && (viewer_->canPan(true) || viewer_->canPan(false));
        key = pan ? QStringLiteral("hint.viewer.zoomed") : QStringLiteral("hint.viewer");
    }
    hintLabel_->setText(expandHint(S(key)));
}

// 保存窗口几何(按模式分别保存到 QuickView.state)
void MainWindow::saveGeometry(WindowMode mode)
{
    if (!isVisible()) return;
    Config& cfg = Config::instance();
    const bool max = isMaximized();
    const QRect r = (max || isFullScreen()) ? normalGeometry() : geometry();
    if (mode == WindowMode::Browser) {
        if (cfg.getEnum(QStringLiteral("Browse/WindowGeometryMode")) == QLatin1String("None")) return;
        browserGeometry_ = r;
        State::instance().setString(QStringLiteral("geom.browser"), rectToString(r, max));
    } else {
        if (!cfg.getBool(QStringLiteral("Viewer/SaveWindowPos"))) return;
        viewerGeometry_ = r;
        State::instance().setString(QStringLiteral("geom.viewer"), rectToString(r, max));
    }
}

// 恢复窗口几何
void MainWindow::restoreGeometry(WindowMode mode)
{
    if (fullscreen_) return;
    Config& cfg = Config::instance();
    bool useState = false;
    bool usePos = false;
    QString key;
    QSize defSize;
    if (mode == WindowMode::Browser) {
        const QString gm = cfg.getEnum(QStringLiteral("Browse/WindowGeometryMode"));
        useState = (gm != QLatin1String("None"));
        usePos = (gm == QLatin1String("SizeAndPos"));
        key = QStringLiteral("geom.browser");
        defSize = QSize(1000, 700);
    } else {
        useState = cfg.getBool(QStringLiteral("Viewer/SaveWindowPos"));
        usePos = useState;
        key = QStringLiteral("geom.viewer");
        defSize = QSize(900, 650);
    }
    QRect r;
    bool max = false;
    const QString s = State::instance().getString(key, QString());
    const bool have = useState && !s.isEmpty();
    if (have && !rectFromString(s, &r, &max)) {
        Config::fatal(S("err.stateFile").arg(State::statePath()).arg(key + QLatin1Char('=') + s));
    }
    QScreen* primary = QGuiApplication::primaryScreen();
    const QRect avail = primary->availableGeometry();
    if (!have) {
        r = QRect(QPoint(0, 0), defSize);
        r.moveCenter(avail.center());
        max = false;
    } else if (!usePos) {
        r.moveCenter(avail.center());
    } else {
        bool onScreen = false;
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            if (screens.at(i)->availableGeometry().intersects(r)) onScreen = true;
        }
        if (!onScreen) r.moveCenter(avail.center());
    }
    setWindowState(windowState() & ~Qt::WindowMaximized);
    setGeometry(r);
    if (max) setWindowState(windowState() | Qt::WindowMaximized);
}

// 当前操作的目标文件(浏览:选中项,没有选中则当前项;查看:当前图)
QStringList MainWindow::targetPaths() const
{
    if (mode_ == WindowMode::Viewer) {
        QStringList out;
        if (!currentPath_.isEmpty()) out.append(currentPath_);
        return out;
    }
    // 焦点在文件夹树/路径栏时,文件操作不作用于文件区的选中项
    if (browser_->focusArea() != FocusArea::Files) return QStringList();
    QStringList sel = browser_->selectedPaths();
    if (sel.isEmpty()) {
        const QString cur = browser_->currentFilePath();
        if (!cur.isEmpty()) sel.append(cur);
    }
    return sel;
}

// 报告错误
void MainWindow::showError(const QString& message)
{
    QMessageBox::critical(this, S("dlg.error.title"), message);
}
