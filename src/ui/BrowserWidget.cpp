#include "BrowserWidget.h"

#include <QKeyEvent>
#include <QStyle>
#include <QFocusEvent>
#include "core/Config.h"
#include "core/State.h"
#include "core/Strings.h"
#include "core/ImageLoader.h"
#include "core/ThumbnailCache.h"
#include "core/Theme.h"

#include <QVBoxLayout>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPainterPath>
#include <QFileInfo>
#include <QDir>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QItemSelectionModel>
#include <QDesktopServices>
#include <QUrl>
#include <QApplication>
#include <QFontMetrics>
#include <algorithm>
#include <QStandardPaths>
#include <QFileIconProvider>
#include <QDrag>
#include <QMimeData>
#include <QPointer>
#include <QTimer>

namespace {

// 文件大小文字
QString bytesText(qint64 bytes)
{
    if (bytes < 1024) return S("status.sizeB").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024.0) return S("status.sizeKB").arg(QString::number(kb, 'f', 1));
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return S("status.sizeMB").arg(QString::number(mb, 'f', 1));
    return S("status.sizeGB").arg(QString::number(mb / 1024.0, 'f', 1));
}

// 改名时只选中文件名主体(不含 .jpg 等扩展名),不会一不小心把扩展名也删了
class NameEditDelegate : public QStyledItemDelegate {
public:
    explicit NameEditDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        QStyledItemDelegate::setEditorData(editor, index);
        QLineEdit* le = qobject_cast<QLineEdit*>(editor);
        if (le == nullptr || index.data(FileListModel::IsDirRole).toBool()) return;
        const int dot = le->text().lastIndexOf(QLatin1Char('.'));
        if (dot <= 0) return;
        QPointer<QLineEdit> guard(le);
        QTimer::singleShot(0, le, [guard, dot]() {   // 等编辑框显示完再选,否则会被"全选"盖掉
            if (!guard.isNull()) guard->setSelection(0, dot);
        });
    }
};

// 缩略图方式的单元格绘制:上面缩略图,下面(缩略图+信息方式)宽x高x色深和文件名
class ThumbDelegate : public NameEditDelegate {
public:
    explicit ThumbDelegate(QObject* parent)
        : NameEditDelegate(parent), withText_(true), border_(6), showArea_(true), showSize_(false),
          cellRadius_(8), imageRadius_(4), imageBorder_(false), hover_(true), focusWidth_(2) {}

    // 外观:圆角/边框/悬停/焦点框粗细来自 ini,颜色来自主题(取不到主题变量会直接报错)
    void configureStyle(int cellRadius, int imageRadius, bool imageBorder, bool hover, int focusWidth)
    {
        cellRadius_ = cellRadius;
        imageRadius_ = imageRadius;
        imageBorder_ = imageBorder;
        hover_ = hover;
        focusWidth_ = focusWidth;
        selColor_ = Theme::color(QStringLiteral("accent_soft"));
        selBorder_ = Theme::color(QStringLiteral("accent"));
        hoverColor_ = Theme::color(QStringLiteral("hover"));
        focusColor_ = Theme::color(QStringLiteral("focus"));
        textColor_ = Theme::color(QStringLiteral("fg"));
        dimColor_ = Theme::color(QStringLiteral("fg_dim"));
        borderColor_ = Theme::color(QStringLiteral("border"));
        dangerColor_ = Theme::color(QStringLiteral("danger"));
    }

    // 设置:是否带文字、边界、是否显示图片面积、是否显示文件大小
    void configure(bool withText, int border, bool showArea, bool showSize)
    {
        withText_ = withText;
        border_ = border;
        showArea_ = showArea;
        showSize_ = showSize;
    }

    // 单元格大小
    QSize cellSize() const
    {
        const QSize ts = ThumbnailCache::instance().thumbSize();
        const int lh = QFontMetrics(QApplication::font()).height();
        return QSize(ts.width() + 2 * border_, ts.height() + 2 * border_ + (withText_ ? 2 * lh + 4 : 0));
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return cellSize(); }

    // 重命名编辑框放在文件名那一行
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        const int lh = QFontMetrics(option.font).height();
        editor->setGeometry(QRect(option.rect.left(), option.rect.bottom() - lh - 6, option.rect.width(), lh + 6));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const bool sel = (option.state & QStyle::State_Selected) != 0;
        const bool hot = hover_ && (option.state & QStyle::State_MouseOver) != 0;
        const bool focus = (option.state & QStyle::State_HasFocus) != 0;
        const QRect cell = option.rect;
        const QRectF box = QRectF(cell).adjusted(1.5, 1.5, -1.5, -1.5);
        if (sel || hot) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(sel ? selColor_ : hoverColor_);
            painter->drawRoundedRect(box, cellRadius_, cellRadius_);
        }
        if (focus && focusWidth_ > 0) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(focusColor_, focusWidth_));
            const qreal h = focusWidth_ / 2.0;
            painter->drawRoundedRect(box.adjusted(h, h, -h, -h), cellRadius_, cellRadius_);
        } else if (sel) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(selBorder_, 1));
            painter->drawRoundedRect(box, cellRadius_, cellRadius_);
        }
        const QSize ts = ThumbnailCache::instance().thumbSize();
        const QRect imgArea(cell.left() + border_, cell.top() + border_, cell.width() - 2 * border_, ts.height());
        const bool isImage = index.data(FileListModel::IsImageRole).toBool();
        const QImage th = isImage ? index.data(FileListModel::ThumbRole).value<QImage>() : QImage();
        if (!th.isNull()) {
            QRect tr(QPoint(0, 0), th.size());
            tr.moveCenter(imgArea.center());
            painter->save();
            QPainterPath clip;
            clip.addRoundedRect(QRectF(tr), imageRadius_, imageRadius_);
            painter->setClipPath(clip);
            painter->drawImage(tr, th);
            painter->restore();
            if (imageBorder_) {
                painter->setBrush(Qt::NoBrush);
                painter->setPen(QPen(borderColor_, 1));
                painter->drawRoundedRect(QRectF(tr).adjusted(0.5, 0.5, -0.5, -0.5), imageRadius_, imageRadius_);
            }
        } else if (isImage) {
            const QString err = index.data(FileListModel::ErrorRole).toString();
            QFont f = option.font;
            f.setPointSize(f.pointSize() * 2);
            painter->setFont(f);
            painter->setPen(err.isEmpty() ? dimColor_ : dangerColor_);
            painter->drawText(imgArea, Qt::AlignCenter, err.isEmpty() ? QStringLiteral("...") : QStringLiteral("x"));
        } else {
            const QIcon ic = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            // 仅缩略图方式没有文字行:文件夹/\"..\"/其他文件的名字画在格子下部,否则分不出是哪个
            const int nlh = QFontMetrics(option.font).height();
            QRect iconArea = imgArea;
            if (!withText_) iconArea.setBottom(iconArea.bottom() - nlh - 2);
            const int side = qMin(48, qMin(iconArea.width(), iconArea.height()));
            QRect ir(0, 0, side, side);
            ir.moveCenter(iconArea.center());
            ic.paint(painter, ir);
            if (!withText_) {
                painter->setFont(option.font);
                painter->setPen(textColor_);
                const QRect nr(cell.left() + 2, imgArea.bottom() - nlh, cell.width() - 4, nlh);
                painter->drawText(nr, Qt::AlignHCenter | Qt::AlignVCenter,
                                  QFontMetrics(option.font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, nr.width()));
            }
        }
        if (withText_) {
            const QFontMetrics fm(option.font);
            const int lh = fm.height();
            painter->setFont(option.font);
            const int y0 = cell.top() + border_ + ts.height() + 2;
            QString info;
            if (isImage) {
                if (showArea_) info = index.data(FileListModel::InfoLineRole).toString();
                if (showSize_) {
                    const QString sz = index.sibling(index.row(), 1).data(Qt::DisplayRole).toString();
                    if (!sz.isEmpty()) info += (info.isEmpty() ? QString() : QStringLiteral(" ")) + sz;
                }
            }
            const QRect l1(cell.left() + 2, y0, cell.width() - 4, lh);
            const QRect l2(cell.left() + 2, y0 + lh, cell.width() - 4, lh);
            painter->setPen(dimColor_);
            painter->drawText(l1, Qt::AlignHCenter | Qt::AlignVCenter, fm.elidedText(info, Qt::ElideRight, l1.width()));
            painter->setPen(textColor_);
            const QString name = index.data(Qt::DisplayRole).toString();
            painter->drawText(l2, Qt::AlignHCenter | Qt::AlignVCenter, fm.elidedText(name, Qt::ElideMiddle, l2.width()));
        }
        painter->restore();
    }

private:
    bool withText_;
    int border_;
    bool showArea_;
    bool showSize_;
    int cellRadius_;
    int imageRadius_;
    bool imageBorder_;
    bool hover_;
    int focusWidth_;
    QColor selColor_, selBorder_, hoverColor_, focusColor_, textColor_, dimColor_, borderColor_, dangerColor_;
};

} // namespace

// 构造
BrowserWidget::BrowserWidget(QWidget* parent)
    : QWidget(parent), pathBox_(nullptr), hSplit_(nullptr), vSplit_(nullptr), tree_(nullptr), treeModel_(nullptr),
      preview_(nullptr), info_(nullptr), infoVisible_(true), centerPane_(nullptr), filterBar_(nullptr), viewStack_(nullptr), listView_(nullptr), detailView_(nullptr), model_(nullptr),
      folderList_(nullptr), previewTimer_(nullptr), treeTimer_(nullptr), focusArea_(FocusArea::Files),
      histPos_(-1), navigatingHistory_(false), syncingTree_(false), thumbDelegate_(nullptr), plainDelegate_(nullptr),
      mode_(ViewMode::Details), treeVisible_(true), previewVisible_(true), pathBarVisible_(true), pathAction_(nullptr)
{
    buildUi();
    applyOptions();
}

// 建立界面
void BrowserWidget::buildUi()
{
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    pathBox_ = new QComboBox(this);
    pathBox_->setEditable(true);
    pathBox_->setInsertPolicy(QComboBox::NoInsert);
    pathBox_->setMaxVisibleItems(20);
    pathBox_->setToolTip(S("browse.pathBar.tip"));
    lay->addWidget(pathBox_);

    hSplit_ = new QSplitter(Qt::Horizontal, this);
    lay->addWidget(hSplit_, 1);
    vSplit_ = new QSplitter(Qt::Vertical, hSplit_);
    tree_ = new QTreeView(vSplit_);
    preview_ = new PreviewPane(vSplit_);
    centerPane_ = new QWidget(hSplit_);
    QVBoxLayout* centerLay = new QVBoxLayout(centerPane_);
    centerLay->setContentsMargins(0, 0, 0, 0);
    centerLay->setSpacing(0);
    filterBar_ = new FilterBar(centerPane_);
    centerLay->addWidget(filterBar_);
    viewStack_ = new QStackedWidget(centerPane_);
    centerLay->addWidget(viewStack_, 1);
    info_ = new InfoPanel(hSplit_);
    info_->setMinimumWidth(150);
    hSplit_->setStretchFactor(0, 0);
    hSplit_->setStretchFactor(1, 1);
    hSplit_->setStretchFactor(2, 0);
    hSplit_->setSizes(QList<int>() << 216 << 800 << Config::instance().getInt(QStringLiteral("Info/PanelWidth")));
    vSplit_->setStretchFactor(0, 1);
    vSplit_->setStretchFactor(1, 0);

    // 文件夹树
    treeModel_ = new QStandardItemModel(this);
    tree_->setModel(treeModel_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);

    // 文件区
    folderList_ = new FolderList(this);
    model_ = new FileListModel(this);
    model_->setFolderList(folderList_);

    detailView_ = new QTreeView(viewStack_);
    detailView_->setModel(model_);
    detailView_->setRootIsDecorated(false);
    detailView_->setItemsExpandable(false);
    detailView_->setUniformRowHeights(true);
    detailView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    detailView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    detailView_->setIconSize(QSize(16, 16));
    detailView_->header()->setSectionsClickable(true);
    detailView_->header()->setSortIndicatorShown(true);
    detailView_->header()->setStretchLastSection(true);
    detailView_->setColumnWidth(0, 280);
    detailView_->setColumnWidth(1, 90);
    detailView_->setColumnWidth(2, 190);

    listView_ = new QListView(viewStack_);
    listView_->setModel(model_);
    listView_->setSelectionModel(detailView_->selectionModel());  // 两种视图共用选择
    listView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    listView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    listView_->setModelColumn(0);
    listView_->setMovement(QListView::Static);                 // 缩略图不能在窗口里被拖着乱放
    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(listView_), static_cast<QAbstractItemView*>(detailView_)}) {
        v->setDragEnabled(true);                               // 可以拖出去:到资源管理器、别的程序
        v->setDragDropMode(QAbstractItemView::DragOnly);
        v->setDefaultDropAction(Qt::CopyAction);
    }

    thumbDelegate_ = new ThumbDelegate(this);
    plainDelegate_ = new NameEditDelegate(this);
    detailView_->setItemDelegate(new NameEditDelegate(this));

    viewStack_->addWidget(listView_);
    viewStack_->addWidget(detailView_);
    viewStack_->setCurrentWidget(detailView_);

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(30);

    // 树键盘移动后延迟载入目录(毫秒数见 Browse/TreeLoadDelayMs)
    treeTimer_ = new QTimer(this);
    treeTimer_->setSingleShot(true);
    treeTimer_->setInterval(Config::instance().getInt(QStringLiteral("Browse/TreeLoadDelayMs")));
    connect(treeTimer_, &QTimer::timeout, this, &BrowserWidget::onTreeTimer);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { onTreeCurrentChanged(); });

    // 焦点区域跟踪:这四个部件获得/失去焦点时更新焦点框
    tree_->installEventFilter(this);
    listView_->installEventFilter(this);
    detailView_->installEventFilter(this);
    pathBox_->lineEdit()->installEventFilter(this);

    // 右键:自定义菜单策略必须设在视图本身上(设在 viewport 上不会发出信号,右键就没有任何反应);
    // 视口上的事件过滤只用来在右键按下时先选中被点的项
    listView_->setContextMenuPolicy(Qt::CustomContextMenu);
    detailView_->setContextMenuPolicy(Qt::CustomContextMenu);
    listView_->viewport()->installEventFilter(this);
    detailView_->viewport()->installEventFilter(this);
    connect(listView_, &QWidget::customContextMenuRequested, this, &BrowserWidget::onContextMenuRequested);
    connect(detailView_, &QWidget::customContextMenuRequested, this, &BrowserWidget::onContextMenuRequested);

    // 文件夹树右键:弹出文件夹菜单(重命名、新建、删除等)
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, &BrowserWidget::onTreeContextMenu);

    connect(tree_, &QTreeView::expanded, this, &BrowserWidget::onTreeExpanded);
    connect(tree_, &QTreeView::clicked, this, &BrowserWidget::onTreeClicked);
    connect(tree_, &QTreeView::activated, this, &BrowserWidget::onTreeClicked);
    connect(pathBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { onPathActivated(); });
    connect(pathBox_->lineEdit(), &QLineEdit::returnPressed, this, &BrowserWidget::onPathActivated);
    connect(listView_, &QAbstractItemView::activated, this, &BrowserWidget::onItemActivated);
    connect(detailView_, &QAbstractItemView::activated, this, &BrowserWidget::onItemActivated);
    connect(detailView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { onSelectionChanged(); });
    connect(detailView_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { onSelectionChanged(); });
    connect(detailView_->header(), &QHeaderView::sectionClicked, this, &BrowserWidget::onHeaderClicked);
    connect(folderList_, &FolderList::changed, this, &BrowserWidget::onFolderListChanged);
    connect(filterBar_, &FilterBar::filterChanged, this, &BrowserWidget::onFilterChanged);
    connect(filterBar_, &FilterBar::sortChanged, this, &BrowserWidget::onFilterSort);
    connect(filterBar_, &FilterBar::viewCommandRequested, this, [this](const QString& id) { emit commandRequested(id); });
    connect(folderList_, &FolderList::newFilesFound, this, [this](const QStringList& paths) {
        if (Config::instance().getBool(QStringLiteral("FileList/AutoSelectNewFiles")) && !paths.isEmpty()) {
            selectPath(paths.last());
        }
    });
    connect(model_, &FileListModel::renamed, this, [this](const QString&, const QString& newPath) { selectPath(newPath); });
    connect(previewTimer_, &QTimer::timeout, this, &BrowserWidget::onPreviewTimer);

    // 路径栏历史
    Config& cfg = Config::instance();
    if (cfg.getBool(QStringLiteral("Browse/SaveRecentFolders"))) {
        const QStringList rec = State::instance().getList(QStringLiteral("recentFolders"));
        for (int i = 0; i < rec.size(); ++i) pathBox_->addItem(QDir::toNativeSeparators(rec.at(i)));
        pathBox_->setCurrentIndex(-1);
    }
    // 位置记忆:成对读入(文件夹、文件名)
    positions_ = State::instance().getList(QStringLiteral("folderPositions"));
    if (positions_.size() % 2 != 0) {
        Config::fatal(S("err.stateFile").arg(State::statePath()).arg(QStringLiteral("folderPositions")));
    }
}

// 进入文件夹;失败弹出错误并返回 false
bool BrowserWidget::openFolder(const QString& path)
{
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    QString err;
    treeTimer_->stop();
    rememberPosition();  // 离开前记住当前文件夹里选中的项
    preview_->clear();
    ThumbnailCache::instance().cancelAll();
    if (!folderList_->setFolder(p, &err)) {
        QMessageBox::critical(window(), S("dlg.error.title"), err);
        if (!folderList_->folder().isEmpty()) updatePathBar(folderList_->folder());
        return false;
    }
    folder_ = folderList_->folder();
    updatePathBar(folder_);
    syncTreeTo(folder_);
    rememberFolder(folder_);
    pushHistory(folder_);
    restorePosition();
    emit folderChanged(folder_);
    return true;
}

// 当前文件夹
QString BrowserWidget::currentFolder() const
{
    return folderList_->folder();
}

// 焦点项的路径
QString BrowserWidget::currentFilePath() const
{
    const QModelIndex cur = activeView()->currentIndex();
    if (cur.isValid() && !cur.sibling(cur.row(), 0).data(FileListModel::IsParentRole).toBool()) {
        return cur.sibling(cur.row(), 0).data(FileListModel::PathRole).toString();
    }
    const QStringList sel = selectedPaths();
    return sel.isEmpty() ? QString() : sel.first();
}

// 选中的全部路径(不含 "..")
QStringList BrowserWidget::selectedPaths() const
{
    QList<int> rows;
    const QModelIndexList idx = detailView_->selectionModel()->selectedIndexes();
    for (int i = 0; i < idx.size(); ++i) {
        if (idx.at(i).column() != 0) continue;
        if (idx.at(i).data(FileListModel::IsParentRole).toBool()) continue;
        rows.append(idx.at(i).row());
    }
    std::sort(rows.begin(), rows.end());
    QStringList out;
    for (int i = 0; i < rows.size(); ++i) {
        out.append(model_->index(rows.at(i), 0).data(FileListModel::PathRole).toString());
    }
    return out;
}

// 选中的数量
int BrowserWidget::selectedCount() const
{
    return selectedPaths().size();
}

// 选中指定路径并滚动到可见
void BrowserWidget::selectPath(const QString& path)
{
    const int row = model_->rowOfPath(QDir::fromNativeSeparators(path));
    if (row < 0) return;
    const QModelIndex idx = model_->index(row, 0);
    detailView_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    activeView()->scrollTo(idx);
}

// 设置显示方式
void BrowserWidget::setViewMode(ViewMode mode)
{
    const QStringList sel = selectedPaths();
    const QString cur = currentFilePath();
    mode_ = mode;
    // 所有显示方式都带 ".." 返回项(像 ACDSee:缩略图方式里它是第一格,双击回上一级)
    model_->setParentRowVisible(true);
    model_->setViewMode(mode);
    if (mode == ViewMode::Details) {
        viewStack_->setCurrentWidget(detailView_);
    } else {
        configureListView();
        viewStack_->setCurrentWidget(listView_);
    }
    updateLeftPane();
    filterBar_->setGrid(mode == ViewMode::ThumbInfo || mode == ViewMode::Thumbs || mode == ViewMode::Icons);
    if (updateFilterBar()) model_->reload();
    State::instance().setString(QStringLiteral("browser.viewMode"), viewModeToString(mode));
    // 恢复选择
    detailView_->selectionModel()->blockSignals(true);
    detailView_->selectionModel()->clearSelection();
    for (int i = 0; i < sel.size(); ++i) {
        const int row = model_->rowOfPath(sel.at(i));
        if (row >= 0) detailView_->selectionModel()->select(model_->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    const int crow = cur.isEmpty() ? -1 : model_->rowOfPath(cur);
    if (crow >= 0) {
        detailView_->selectionModel()->setCurrentIndex(model_->index(crow, 0), QItemSelectionModel::NoUpdate);
        activeView()->scrollTo(model_->index(crow, 0));
    }
    detailView_->selectionModel()->blockSignals(false);
    onSelectionChanged();
}

// 当前显示方式
ViewMode BrowserWidget::viewMode() const
{
    return mode_;
}

// 重新读取配置并应用
void BrowserWidget::applyOptions()
{
    Config& cfg = Config::instance();
    ThumbnailCache::instance().configure();
    model_->applyOptions();
    preview_->applyOptions();
    info_->applyOptions();
    info_->setVisible(infoVisible_);
    filterBar_->applyOptions();
    folderList_->applyOptions();

    // 目录树
    rebuildTree();
    syncTreeTo(folderList_->folder());

    // 详细视图
    QAbstractItemView::SelectionBehavior beh = cfg.getBool(QStringLiteral("FileList/FullRowSelect"))
                                                   ? QAbstractItemView::SelectRows : QAbstractItemView::SelectItems;
    detailView_->setSelectionBehavior(beh);
    detailView_->setAllColumnsShowFocus(cfg.getBool(QStringLiteral("FileList/FullRowSelect")));
    // 网格线的样子由主题(theme\current.qss)里的 QTreeView[showGrid="true"]::item 决定,这里只告诉它开没开
    detailView_->setProperty("showGrid", cfg.getBool(QStringLiteral("FileList/ShowGrid")));
    detailView_->style()->unpolish(detailView_);
    detailView_->style()->polish(detailView_);
    detailView_->update();
    const bool autoW = cfg.getBool(QStringLiteral("FileList/AutoColumnWidth"));
    for (int i = 0; i < 3; ++i) {
        detailView_->header()->setSectionResizeMode(i, autoW ? QHeaderView::ResizeToContents : QHeaderView::Interactive);
    }
    // 单击编辑文件名
    const QAbstractItemView::EditTriggers et = cfg.getBool(QStringLiteral("Browse/SingleClickRename"))
                                                   ? QAbstractItemView::SelectedClicked : QAbstractItemView::NoEditTriggers;
    detailView_->setEditTriggers(et);
    listView_->setEditTriggers(et);

    // 预览大小
    const int ph = cfg.getInt(QStringLiteral("FileList/PreviewHeight"));
    const int total = vSplit_->height() > 0 ? vSplit_->height() : 600;
    QList<int> sizes;
    sizes << qMax(50, total - ph) << ph;
    vSplit_->setSizes(sizes);

    if (mode_ != ViewMode::Details) configureListView();
    updateLeftPane();
    if (!folderList_->folder().isEmpty()) folderList_->refresh();
}

// 刷新当前文件夹
void BrowserWidget::refresh()
{
    ThumbnailCache::instance().clearMemory();
    model_->clearFailed();
    folderList_->refresh();
}

// 重建文件夹树并定位到当前文件夹
void BrowserWidget::refreshTree()
{
    rebuildTree();
    syncTreeTo(folderList_->folder());
}

// 树里右键:选中被点的文件夹(不跳转),让主窗口弹菜单
void BrowserWidget::onTreeContextMenu(const QPoint& pos)
{
    const QModelIndex idx = tree_->indexAt(pos);
    if (!idx.isValid()) return;
    const QString p = idx.data(Qt::UserRole + 1).toString();
    if (p.isEmpty()) return;   // 虚拟节点(此电脑、库)没有菜单
    syncingTree_ = true;
    tree_->setCurrentIndex(idx);
    syncingTree_ = false;
    emit treeContextMenuRequested(p, tree_->viewport()->mapToGlobal(pos));
}

// 上一级:光标落在刚离开的那个文件夹上
void BrowserWidget::goUp()
{
    QDir d(folderList_->folder());
    if (folderList_->folder().isEmpty() || !d.cdUp()) return;
    const QString child = folderList_->folder();
    if (openFolder(d.absolutePath())) {
        selectPath(child);
        focusFileArea();
    }
}

// 把焦点交给当前视图(缩略图/图标/列表/详细),方向键才能选文件;从路径栏回来时恢复路径栏文字
void BrowserWidget::focusFileArea()
{
    if (pathBox_->lineEdit()->hasFocus() && !folder_.isEmpty()) {
        pathBox_->lineEdit()->setText(QDir::toNativeSeparators(folder_));
    }
    activeView()->setFocus();
}

// 当前焦点区域
FocusArea BrowserWidget::focusArea() const
{
    return focusArea_;
}

// 焦点交给文件夹树
void BrowserWidget::focusTree()
{
    if (!treeVisible_) return;
    tree_->setFocus();
}

// 焦点交给路径栏并全选文字
void BrowserWidget::focusPathBar()
{
    if (!pathBarVisible_) return;
    pathBox_->lineEdit()->setFocus();
    pathBox_->lineEdit()->selectAll();
}

// 在可见区域间循环:路径栏 -> 文件夹树 -> 文件区
void BrowserWidget::focusCycle(bool forward)
{
    QList<FocusArea> order;
    if (pathBarVisible_) order.append(FocusArea::PathBar);
    if (treeVisible_) order.append(FocusArea::Tree);
    order.append(FocusArea::Files);
    int i = order.indexOf(focusArea_);
    if (i < 0) i = order.size() - 1;
    const int n = order.size();
    i = (i + (forward ? 1 : n - 1)) % n;
    switch (order.at(i)) {
    case FocusArea::PathBar: focusPathBar(); break;
    case FocusArea::Tree: focusTree(); break;
    case FocusArea::Files: focusFileArea(); break;
    }
}

// 在历史里走一步
bool BrowserWidget::navigateHistory(int delta)
{
    const int np = histPos_ + delta;
    if (np < 0 || np >= history_.size()) return false;
    navigatingHistory_ = true;
    const bool ok = openFolder(history_.at(np));
    navigatingHistory_ = false;
    if (ok) {
        histPos_ = np;
        focusFileArea();
    }
    return ok;
}

// 后退 / 前进
bool BrowserWidget::goBack()
{
    return navigateHistory(-1);
}

bool BrowserWidget::goForward()
{
    return navigateHistory(1);
}

// 进入新文件夹时记入历史(丢弃"前进"那一段;数量见 Browse/HistoryCount)
void BrowserWidget::pushHistory(const QString& path)
{
    if (navigatingHistory_) return;
    if (histPos_ >= 0 && histPos_ < history_.size() && history_.at(histPos_) == path) return;
    while (history_.size() > histPos_ + 1) history_.removeLast();
    history_.append(path);
    const int limit = Config::instance().getInt(QStringLiteral("Browse/HistoryCount"));
    while (history_.size() > limit) history_.removeFirst();
    histPos_ = history_.size() - 1;
}

// 记住当前文件夹里的焦点文件(数量上限 Browse/PositionMemoryCount,0 表示不记)
void BrowserWidget::rememberPosition()
{
    const int limit = Config::instance().getInt(QStringLiteral("Browse/PositionMemoryCount"));
    if (limit <= 0 || folder_.isEmpty()) return;
    const QString cur = currentFilePath();
    if (cur.isEmpty()) return;
    for (int i = 0; i + 1 < positions_.size(); i += 2) {
        if (positions_.at(i) == folder_) {
            positions_.removeAt(i + 1);
            positions_.removeAt(i);
            break;
        }
    }
    positions_.prepend(QFileInfo(cur).fileName());
    positions_.prepend(folder_);
    while (positions_.size() > limit * 2) {
        positions_.removeLast();
        positions_.removeLast();
    }
}

// 进入文件夹后恢复上次选中项;没有记忆就选第一项(Browse/EnterSelectFirst)
void BrowserWidget::restorePosition()
{
    for (int i = 0; i + 1 < positions_.size(); i += 2) {
        if (positions_.at(i) != folder_) continue;
        const QString path = QDir(folder_).filePath(positions_.at(i + 1));
        if (model_->rowOfPath(path) >= 0) {
            selectPath(path);
            return;
        }
        break;
    }
    if (!Config::instance().getBool(QStringLiteral("Browse/EnterSelectFirst"))) return;
    int row = model_->hasParentRow() ? 1 : 0;
    if (row >= model_->rowCount(QModelIndex())) row = 0;  // 文件夹里只有 ".." 时选中它,回车即可返回上一级
    if (row >= model_->rowCount(QModelIndex())) return;
    const QModelIndex idx = model_->index(row, 0);
    detailView_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    activeView()->scrollTo(idx);
}

// 缩略图放大/缩小一步
// 恢复默认缩略图大小(设置 Thumbnails/DefaultSize)
void BrowserWidget::resetThumbSize()
{
    setThumbSize(Config::instance().getInt(QStringLiteral("Thumbnails/DefaultSize")));
}

void BrowserWidget::adjustThumbSize(int direction)
{
    Config& c0 = Config::instance();
    const int step = c0.getInt(QStringLiteral("Thumbnails/SizeStep"));
    setThumbSize(c0.getInt(QStringLiteral("Thumbnails/Width")) + (direction > 0 ? step : -step));
}

void BrowserWidget::setThumbSize(int size)
{
    if (mode_ != ViewMode::ThumbInfo && mode_ != ViewMode::Thumbs) return;
    Config& cfg = Config::instance();
    const int lo = cfg.getInt(QStringLiteral("Thumbnails/MinSize"));
    const int hi = cfg.getInt(QStringLiteral("Thumbnails/MaxSize"));
    const int cur = cfg.getInt(QStringLiteral("Thumbnails/Width"));
    const int next = std::max(lo, std::min(hi, size));
    if (next == cur) return;
    cfg.setInt(QStringLiteral("Thumbnails/Width"), next);
    cfg.setInt(QStringLiteral("Thumbnails/Height"), next);
    ThumbnailCache::instance().configure();
    configureListView();
    listView_->viewport()->update();
    if (listView_->currentIndex().isValid()) listView_->scrollTo(listView_->currentIndex());
}

// 筛选条:切换标签
void BrowserWidget::filterStep(int delta)
{
    filterBar_->cycleTag(delta);
}

// 筛选条:回到"全部"
void BrowserWidget::filterAll()
{
    filterBar_->selectAllTag();
}

// 筛选条:下一种排序依据
void BrowserWidget::cycleSort()
{
    if (!filterBar_->barEnabled()) return;
    filterBar_->cycleSort();
}

// 筛选条:反转排序方向
void BrowserWidget::reverseSort()
{
    if (!filterBar_->barEnabled()) return;
    filterBar_->reverseSort();
}

// 切换网格/列表
void BrowserWidget::toggleViewStyle()
{
    if (!filterBar_->barEnabled()) return;
    emit commandRequested(filterBar_->toggleCommand());
}

// 筛选标签变了:重新装载并保持选择
void BrowserWidget::onFilterChanged()
{
    model_->setExtFilter(filterBar_->filterActive(), filterBar_->filterExts());
    onFolderListChanged();
}

// 筛选条改了排序
void BrowserWidget::onFilterSort(SortKey key, bool desc)
{
    folderList_->setSort(key, desc);
    State::instance().setInt(QStringLiteral("sort.key"), static_cast<int>(key));
    State::instance().setBool(QStringLiteral("sort.desc"), desc);
    syncSortIndicators();
}

// 把排序状态同步到表头和筛选条
void BrowserWidget::syncSortIndicators()
{
    const SortKey key = folderList_->sortKey();
    const bool desc = folderList_->sortDescending();
    int section = 0;
    if (key == SortKey::Size) section = 1;
    else if (key == SortKey::Type) section = 2;
    else if (key == SortKey::Date) section = 3;
    detailView_->header()->setSortIndicator(section, desc ? Qt::DescendingOrder : Qt::AscendingOrder);
    filterBar_->setSort(key, desc);
}

// 用当前文件夹的内容刷新筛选条;模型的筛选条件因此改变时返回 true(调用者要重新装载模型)
bool BrowserWidget::updateFilterBar()
{
    const bool changedFolder = (filterFolder_ != folderList_->folder());
    filterFolder_ = folderList_->folder();
    const bool oldOn = filterBar_->filterActive();
    const QStringList oldExts = filterBar_->filterExts();
    // 数量不受筛选影响,所以直接向模型要
    filterBar_->setExtensions(model_->extensionCounts(), changedFolder);
    const bool newOn = filterBar_->filterActive();
    const QStringList newExts = filterBar_->filterExts();
    model_->setExtFilter(newOn, newExts);
    return oldOn != newOn || oldExts != newExts;
}

// 全选(不含 "..")
void BrowserWidget::selectAll()
{
    activeView()->selectAll();
    if (model_->hasParentRow()) {
        detailView_->selectionModel()->select(model_->index(0, 0), QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
    }
}

// 打开当前项;焦点在路径栏时,回车表示前往输入的路径
void BrowserWidget::activateCurrent()
{
    if (pathBox_->lineEdit() != nullptr && pathBox_->lineEdit()->hasFocus()) {
        onPathActivated();
        return;
    }
    if (tree_->hasFocus()) {
        activateTreeItem();
        return;
    }
    const QModelIndex cur = activeView()->currentIndex();
    if (!cur.isValid()) return;
    onItemActivated(cur);
}

// 对当前项开始重命名
void BrowserWidget::startRename()
{
    QModelIndex cur = activeView()->currentIndex();
    if (!cur.isValid()) return;
    cur = cur.sibling(cur.row(), 0);
    if (cur.data(FileListModel::IsParentRole).toBool()) return;
    activeView()->edit(cur);
}

// 文件夹列表对象
FolderList* BrowserWidget::folderList() const
{
    return folderList_;
}

// 状态栏四个字段:总计 | 选中大小和时间 | 选中文件名 | 图片信息
QStringList BrowserWidget::statusFields() const
{
    QStringList f;
    f << S("status.total").arg(folderList_->fileCount()).arg(bytesText(folderList_->totalBytes()));
    const QStringList sel = selectedPaths();
    if (sel.isEmpty()) {
        f << QString() << QString() << QString();
    } else if (sel.size() == 1) {
        const QFileInfo fi(sel.first());
        const QString date = fi.lastModified().toString(QStringLiteral("yyyy/M/d H:mm"));
        if (fi.isDir()) {
            f << date << fi.fileName() << QString();
        } else {
            f << S("status.fileSizeDate").arg(bytesText(fi.size())).arg(date) << fi.fileName();
            QString infoText;
            if (FolderList::isImagePath(sel.first())) {
                ImageInfo info = ThumbnailCache::instance().cachedInfo(sel.first());
                if (!info.valid) {
                    QString err;
                    if (!ImageLoader::instance().readInfo(sel.first(), &info, &err)) infoText = err;
                }
                if (info.valid) infoText = imageInfoLine(info, true);
            }
            f << infoText;
        }
    } else {
        qint64 sum = 0;
        for (int i = 0; i < sel.size(); ++i) {
            const QFileInfo fi(sel.at(i));
            if (fi.isFile()) sum += fi.size();
        }
        f << S("status.selected").arg(sel.size()).arg(bytesText(sum)) << QString() << QString();
    }
    return f;
}

// 目录树显示/隐藏
void BrowserWidget::setTreeVisible(bool v)
{
    treeVisible_ = v;
    updateLeftPane();
    if (!v && focusArea_ == FocusArea::Tree) focusFileArea();  // 焦点区域被隐藏:回到文件区
}

// 预览显示/隐藏
void BrowserWidget::setPreviewVisible(bool v)
{
    previewVisible_ = v;
    updateLeftPane();
}

// 信息面板显示/隐藏
void BrowserWidget::setInfoVisible(bool v)
{
    infoVisible_ = v;
    info_->setVisible(v);
    if (v) previewTimer_->start();
}

// 路径栏显示/隐藏
void BrowserWidget::setPathBarVisible(bool v)
{
    pathBarVisible_ = v;
    if (pathAction_ != nullptr) pathAction_->setVisible(v);
    else pathBox_->setVisible(v);
    if (!v && focusArea_ == FocusArea::PathBar) focusFileArea();
}

// 路径栏放进工具栏:拉伸占满剩余宽度
void BrowserWidget::dockPathBar(QToolBar* bar)
{
    if (pathAction_ != nullptr) return;
    QVBoxLayout* lay = qobject_cast<QVBoxLayout*>(layout());
    if (lay != nullptr) lay->removeWidget(pathBox_);
    pathBox_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pathAction_ = bar->addWidget(pathBox_);
    pathAction_->setVisible(pathBarVisible_);
    pathBox_->setVisible(true);
}

// 路径栏放回浏览窗口顶部(必须在工具栏 clear 之前调用,否则路径栏会随工具栏被删除)
void BrowserWidget::undockPathBar()
{
    if (pathAction_ == nullptr) return;
    pathAction_ = nullptr;
    pathBox_->setParent(this);
    pathBox_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    QVBoxLayout* lay = qobject_cast<QVBoxLayout*>(layout());
    if (lay != nullptr) lay->insertWidget(0, pathBox_);
    pathBox_->setVisible(pathBarVisible_);
}

// 路径栏历史
QStringList BrowserWidget::recentFolders() const
{
    QStringList out;
    for (int i = 0; i < pathBox_->count(); ++i) out.append(QDir::fromNativeSeparators(pathBox_->itemText(i)));
    return out;
}

// 保存分隔条位置、显示方式、排序
void BrowserWidget::saveLayout()
{
    State& st = State::instance();
    QStringList h;
    const QList<int> hs = hSplit_->sizes();
    for (int i = 0; i < hs.size(); ++i) h << QString::number(hs.at(i));
    st.setList(QStringLiteral("layout.hsplit"), h);
    QStringList v;
    const QList<int> vs = vSplit_->sizes();
    for (int i = 0; i < vs.size(); ++i) v << QString::number(vs.at(i));
    st.setList(QStringLiteral("layout.vsplit"), v);
    rememberPosition();
    st.setList(QStringLiteral("folderPositions"), positions_);
    st.setString(QStringLiteral("browser.viewMode"), viewModeToString(mode_));
    st.setInt(QStringLiteral("sort.key"), static_cast<int>(folderList_->sortKey()));
    st.setBool(QStringLiteral("sort.desc"), folderList_->sortDescending());
    st.save();
}

// 恢复分隔条位置、显示方式、排序
void BrowserWidget::restoreLayout()
{
    State& st = State::instance();
    struct Item { const char* key; QSplitter* sp; };
    const Item items[] = { { "layout.hsplit", hSplit_ }, { "layout.vsplit", vSplit_ } };
    for (int k = 0; k < 2; ++k) {
        const QStringList l = st.getList(QString::fromLatin1(items[k].key));
        if (l.isEmpty()) continue;
        QList<int> sizes;
        for (int i = 0; i < l.size(); ++i) {
            bool ok = false;
            const int n = l.at(i).toInt(&ok);
            if (!ok || n < 0) {
                Config::fatal(S("err.stateFile").arg(State::statePath()).arg(QString::fromLatin1(items[k].key)));
            }
            sizes.append(n);
        }
        if (sizes.size() == items[k].sp->count()) items[k].sp->setSizes(sizes);
    }
    const QString vm = st.getString(QStringLiteral("browser.viewMode"), viewModeToString(ViewMode::Details));
    ViewMode m = ViewMode::Details;
    if (!viewModeFromString(vm, &m)) Config::fatal(S("err.viewMode").arg(vm));
    const int sk = st.getInt(QStringLiteral("sort.key"), 0);
    if (sk < 0 || sk > 3) Config::fatal(S("err.stateFile").arg(State::statePath()).arg(QStringLiteral("sort.key")));
    const bool desc = st.getBool(QStringLiteral("sort.desc"), false);
    folderList_->setSort(static_cast<SortKey>(sk), desc);
    syncSortIndicators();
    setViewMode(m);
}

// 事件过滤:右键时先选中光标下的项
bool BrowserWidget::eventFilter(QObject* obj, QEvent* e)
{
    // 焦点跟踪:获得焦点时记下区域并画焦点框(窗口失活造成的失焦不算)
    if (e->type() == QEvent::FocusIn || e->type() == QEvent::FocusOut) {
        QWidget* w = qobject_cast<QWidget*>(obj);
        const bool isEdit = (w == pathBox_->lineEdit());
        if (w != nullptr && (w == tree_ || w == listView_ || w == detailView_ || isEdit)) {
            const bool in = (e->type() == QEvent::FocusIn);
            if (!in) {
                const Qt::FocusReason r = static_cast<QFocusEvent*>(e)->reason();
                if (r == Qt::ActiveWindowFocusReason || r == Qt::PopupFocusReason) return QWidget::eventFilter(obj, e);
            }
            if (in) {
                focusArea_ = (w == tree_) ? FocusArea::Tree : (isEdit ? FocusArea::PathBar : FocusArea::Files);
                emit focusAreaChanged();
            }
            if (Config::instance().getBool(QStringLiteral("Browse/FocusFrame"))) setFocusFrame(isEdit ? static_cast<QWidget*>(pathBox_) : w, in);
            return QWidget::eventFilter(obj, e);
        }
    }
    // 只用方向键走遍全程:缩略图最左一列按 ←  → 回文件夹树;树里没有子文件夹时按 → 进入缩略图区
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(e);
        const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
        if (mods == Qt::NoModifier) {
            Config& cfg = Config::instance();
            if (obj == tree_ && ke->key() == Qt::Key_Right && cfg.getBool(QStringLiteral("Browse/RightToFiles"))) {
                const QModelIndex idx = tree_->currentIndex();
                if (idx.isValid() && treeModel_->rowCount(idx) == 0) {
                    activateTreeItem();  // 载入该文件夹并把焦点交给缩略图区
                    return true;
                }
            }
            if ((obj == listView_ || obj == detailView_) && ke->key() == Qt::Key_Left && treeVisible_
                && cfg.getBool(QStringLiteral("Browse/LeftToTree"))) {
                QAbstractItemView* v = static_cast<QAbstractItemView*>(obj);
                const QModelIndex cur = v->currentIndex();
                bool edge = true;
                if (obj == listView_ && cur.isValid() && cur.row() > 0) {
                    const QModelIndex first = v->model()->index(0, 0);
                    edge = v->visualRect(cur).left() <= v->visualRect(first).left() + 1;
                }
                if (edge) {
                    focusTree();
                    return true;
                }
            }
        }
    }
    // Ctrl+滚轮:缩略图方式下放大/缩小缩略图(步长和范围见 [Thumbnails]);按滚轮累计量算格,触摸板也行
    if (e->type() == QEvent::Wheel && (obj == listView_->viewport() || obj == listView_)
        && (mode_ == ViewMode::ThumbInfo || mode_ == ViewMode::Thumbs)) {
        QWheelEvent* we = static_cast<QWheelEvent*>(e);
        if ((we->modifiers() & Qt::ControlModifier) != 0) {
            int dy = we->angleDelta().y();
            if (dy == 0) dy = we->angleDelta().x();
            wheelAcc_ += dy;
            while (wheelAcc_ >= 120) { adjustThumbSize(1); wheelAcc_ -= 120; }
            while (wheelAcc_ <= -120) { adjustThumbSize(-1); wheelAcc_ += 120; }
            return true;
        }
    }
    if (e->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::RightButton) {
            QAbstractItemView* v = (obj == listView_->viewport()) ? static_cast<QAbstractItemView*>(listView_)
                                                                  : static_cast<QAbstractItemView*>(detailView_);
            const QModelIndex idx = v->indexAt(me->pos());
            rightPressPos_ = me->pos();
            rightDragArmed_ = idx.isValid() && !idx.sibling(idx.row(), 0).data(FileListModel::IsParentRole).toBool();
            if (idx.isValid()) {
                if (!detailView_->selectionModel()->isSelected(idx.sibling(idx.row(), 0))) {
                    detailView_->selectionModel()->setCurrentIndex(idx.sibling(idx.row(), 0),
                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                }
            } else {
                detailView_->selectionModel()->clearSelection();
            }
        }
    }
    // 右键拖动:按住右键把文件拖到资源管理器/桌面,松开时由对方弹出"复制到此处/移动到此处"菜单
    if (e->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(e);
        if (rightDragArmed_ && (me->buttons() & Qt::RightButton) != 0
            && (me->pos() - rightPressPos_).manhattanLength() >= QApplication::startDragDistance()) {
            rightDragArmed_ = false;
            startFileDrag();
            return true;
        }
    } else if (e->type() == QEvent::MouseButtonRelease) {
        rightDragArmed_ = false;
    }
    return QWidget::eventFilter(obj, e);
}

// 开始把选中的文件拖出去(右键拖动用;左键拖动由视图自带的拖出处理)
void BrowserWidget::startFileDrag()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) return;
    QList<QUrl> urls;
    for (int i = 0; i < paths.size(); ++i) urls << QUrl::fromLocalFile(paths.at(i));
    QMimeData* md = new QMimeData();
    md->setUrls(urls);
    QDrag* drag = new QDrag(this);
    drag->setMimeData(md);
    const QModelIndex cur = activeView()->currentIndex();
    const QVariant dec = cur.sibling(cur.row(), 0).data(Qt::DecorationRole);
    QPixmap pm;
    if (dec.canConvert<QIcon>()) pm = qvariant_cast<QIcon>(dec).pixmap(48, 48);
    else if (dec.canConvert<QPixmap>()) pm = qvariant_cast<QPixmap>(dec);
    if (!pm.isNull()) {
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));
    }
    drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
    folderList_->refresh();   // 对方如果选了"移动",文件已经不在这里了
}

// 树里点击了文件夹
void BrowserWidget::onTreeClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    const QString p = index.data(Qt::UserRole + 1).toString();
    if (p.isEmpty()) return;  // 虚拟节点(此电脑、库)只展开不跳转
    if (QDir::cleanPath(p) == folderList_->folder()) return;
    openFolder(p);
}

// 焦点框:属性 mmFocus=true 时主题 QSS 画强调色边框
void BrowserWidget::setFocusFrame(QWidget* w, bool on)
{
    w->setProperty("mmFocus", on);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// 树里键盘移动:树有焦点时才算(程序同步选中项不算),延迟后载入
void BrowserWidget::onTreeCurrentChanged()
{
    if (syncingTree_ || !tree_->hasFocus()) return;
    treeTimer_->start();
}

// 延迟到:载入树当前项的目录(虚拟节点和当前文件夹跳过)
void BrowserWidget::onTreeTimer()
{
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid()) return;
    const QString p = idx.data(Qt::UserRole + 1).toString();
    if (p.isEmpty() || QDir::cleanPath(p) == folderList_->folder()) return;
    openFolder(p);
}

// Enter:目录节点立刻进入并把焦点交给文件区;"此电脑""库"这类虚拟节点只展开/折叠
void BrowserWidget::activateTreeItem()
{
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid()) return;
    const QString p = idx.data(Qt::UserRole + 1).toString();
    if (p.isEmpty()) {
        tree_->setExpanded(idx, !tree_->isExpanded(idx));
        return;
    }
    treeTimer_->stop();
    if (QDir::cleanPath(p) != folderList_->folder() && !openFolder(p)) return;
    focusFileArea();
}

// 路径栏回车或选择
void BrowserWidget::onPathActivated()
{
    const QString text = pathBox_->currentText().trimmed();
    if (text.isEmpty()) return;
    if (openFolder(text)) focusFileArea();
}

// 文件区选择变化
void BrowserWidget::onSelectionChanged()
{
    previewTimer_->start();
    emit selectionChanged();
}

// 双击/回车
void BrowserWidget::onItemActivated(const QModelIndex& index)
{
    if (!index.isValid()) return;
    // 双击"描述"列:按 Browse/DoubleClickEditDescription 进入描述编辑
    if (index.column() == 3 && mode_ == ViewMode::Details
        && Config::instance().getBool(QStringLiteral("Browse/DoubleClickEditDescription"))
        && (model_->flags(index) & Qt::ItemIsEditable)) {
        detailView_->edit(index);
        return;
    }
    const QModelIndex i0 = index.sibling(index.row(), 0);
    if (i0.data(FileListModel::IsParentRole).toBool()) {
        goUp();
        return;
    }
    const QString path = i0.data(FileListModel::PathRole).toString();
    if (i0.data(FileListModel::IsDirRole).toBool()) {
        openFolder(path);
        return;
    }
    if (i0.data(FileListModel::IsImageRole).toBool()) {
        emit imageActivated(path);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::critical(window(), S("dlg.error.title"), S("err.readFile").arg(path).arg(S("err.noDecoder").arg(path)));
    }
}

// FolderList 内容变化:重载模型并尽量保持选择
void BrowserWidget::onFolderListChanged()
{
    const QStringList sel = selectedPaths();
    const QString cur = currentFilePath();
    updateFilterBar();  // 返回值不用:下一行本来就要重新装载
    model_->reload();
    QItemSelectionModel* sm = detailView_->selectionModel();
    sm->blockSignals(true);
    sm->clearSelection();
    for (int i = 0; i < sel.size(); ++i) {
        const int row = model_->rowOfPath(sel.at(i));
        if (row >= 0) sm->select(model_->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    if (!cur.isEmpty()) {
        const int crow = model_->rowOfPath(cur);
        if (crow >= 0) sm->setCurrentIndex(model_->index(crow, 0), QItemSelectionModel::NoUpdate);
    }
    sm->blockSignals(false);
    onSelectionChanged();
}

// 详细模式点击表头排序:名称/大小/图片属性(按类型)/描述(按日期)
void BrowserWidget::onHeaderClicked(int section)
{
    SortKey key = SortKey::Name;
    if (section == 1) key = SortKey::Size;
    else if (section == 2) key = SortKey::Type;
    else if (section == 3) key = SortKey::Date;  // TODO-CONFIRM: 描述列没有排序意义,暂按日期
    bool desc = false;
    if (key == folderList_->sortKey()) desc = !folderList_->sortDescending();
    detailView_->header()->setSortIndicator(section, desc ? Qt::DescendingOrder : Qt::AscendingOrder);
    folderList_->setSort(key, desc);
    State::instance().setInt(QStringLiteral("sort.key"), static_cast<int>(key));
    State::instance().setBool(QStringLiteral("sort.desc"), desc);
    filterBar_->setSort(key, desc);
}

// 预览延迟更新
void BrowserWidget::onPreviewTimer()
{
    const QString curSel = currentFilePath();
    if (info_->isVisible()) {
        if (!curSel.isEmpty() && FolderList::isImagePath(curSel) && QFileInfo(curSel).isFile()) info_->showFile(curSel);
        else info_->clear();
    }
    if (!preview_->isVisible()) return;
    const QString cur = curSel;
    if (cur.isEmpty() || !FolderList::isImagePath(cur) || !QFileInfo(cur).isFile()) {
        preview_->clear();
        return;
    }
    preview_->showFile(cur);
}

// 右键:通知主窗口弹菜单
void BrowserWidget::onContextMenuRequested(const QPoint& pos)
{
    QAbstractScrollArea* w = qobject_cast<QAbstractScrollArea*>(sender());
    if (w == nullptr) return;
    emit contextMenuRequested(w->viewport()->mapToGlobal(pos));   // pos 是视口坐标
}

// 当前显示的视图
QAbstractItemView* BrowserWidget::activeView() const
{
    if (viewStack_->currentWidget() == listView_) return listView_;
    return detailView_;
}

// 更新路径栏文字和历史
void BrowserWidget::updatePathBar(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path);
    const int limit = Config::instance().getInt(QStringLiteral("Browse/RecentFolderCount"));
    pathBox_->blockSignals(true);
    const int idx = pathBox_->findText(native, Qt::MatchFixedString);
    if (idx >= 0) pathBox_->removeItem(idx);
    pathBox_->insertItem(0, native);
    while (pathBox_->count() > limit) pathBox_->removeItem(pathBox_->count() - 1);
    pathBox_->setCurrentIndex(0);
    pathBox_->blockSignals(false);
}

// 在树里选中某路径(并展开上级)
void BrowserWidget::syncTreeTo(const QString& path)
{
    if (path.isEmpty()) return;
    QStandardItem* found = findTreeItem(treeModel_->invisibleRootItem(), QDir::cleanPath(path));
    if (found == nullptr) return;
    tree_->blockSignals(true);
    syncingTree_ = true;
    const QModelIndex idx = treeModel_->indexFromItem(found);
    tree_->setCurrentIndex(idx);
    tree_->scrollTo(idx);
    syncingTree_ = false;
    tree_->blockSignals(false);
}

// 新建树节点:UserRole+1 = 路径(空 = 虚拟节点),UserRole+2 = 是否已读入子项
QStandardItem* BrowserWidget::makeTreeItem(const QString& text, const QString& path, bool expandable)
{
    QStandardItem* it = new QStandardItem(text);
    it->setEditable(false);
    it->setData(path, Qt::UserRole + 1);
    it->setData(false, Qt::UserRole + 2);
    if (!path.isEmpty()) {
        static QFileIconProvider provider;
        it->setIcon(provider.icon(QFileInfo(path)));
    }
    if (expandable) {
        QStandardItem* ph = new QStandardItem(QString());
        ph->setData(true, Qt::UserRole + 3);  // 占位子项,让节点显示展开箭头
        it->appendRow(ph);
    }
    return it;
}

// 读入一个节点的子文件夹(已读入过则不再读)
void BrowserWidget::populateNode(QStandardItem* item)
{
    if (item == nullptr) return;
    if (item->data(Qt::UserRole + 2).toBool()) return;
    item->setData(true, Qt::UserRole + 2);
    const QString path = item->data(Qt::UserRole + 1).toString();
    if (path.isEmpty()) return;  // 虚拟节点的子项在 rebuildTree 里已建好
    item->removeRows(0, item->rowCount());
    QDir::Filters f = QDir::Dirs | QDir::NoDotAndDotDot;
    if (Config::instance().getBool(QStringLiteral("FileList/ShowHidden"))) f |= QDir::Hidden;
    const QFileInfoList subs = QDir(path).entryInfoList(f, QDir::Name | QDir::IgnoreCase);
    for (int i = 0; i < subs.size(); ++i) {
        const QFileInfo fi = subs.at(i);
        const bool hasSub = !QDir(fi.absoluteFilePath()).entryList(f).isEmpty();
        item->appendRow(makeTreeItem(fi.fileName(), fi.absoluteFilePath(), hasSub));
    }
}

// 在 parent 下找路径对应的节点,沿途展开
QStandardItem* BrowserWidget::findTreeItem(QStandardItem* parent, const QString& path)
{
    for (int i = 0; i < parent->rowCount(); ++i) {
        QStandardItem* ch = parent->child(i);
        if (ch->data(Qt::UserRole + 3).toBool()) continue;
        const QString cp = QDir::cleanPath(ch->data(Qt::UserRole + 1).toString());
        const bool virt = ch->data(Qt::UserRole + 1).toString().isEmpty();
        if (!virt) {
            if (cp.compare(path, Qt::CaseInsensitive) == 0) return ch;
            const QString prefix = cp.endsWith(QLatin1Char('/')) ? cp : cp + QLatin1Char('/');
            if (!path.startsWith(prefix, Qt::CaseInsensitive)) continue;
        }
        populateNode(ch);
        QStandardItem* r = findTreeItem(ch, path);
        if (r != nullptr) {
            tree_->expand(treeModel_->indexFromItem(ch));
            return r;
        }
    }
    return nullptr;
}

// 树节点展开:读入子文件夹
void BrowserWidget::onTreeExpanded(const QModelIndex& index)
{
    populateNode(treeModel_->itemFromIndex(index));
}

// 重建顶层节点:用户名、此电脑(桌面+各磁盘)、库
void BrowserWidget::rebuildTree()
{
    Config& cfg = Config::instance();
    treeModel_->clear();
    QStandardItem* root = treeModel_->invisibleRootItem();

    const QString home = QDir::homePath();
    QString userName = QFileInfo(home).fileName();
    if (userName.isEmpty()) userName = home;
    root->appendRow(makeTreeItem(userName, home, true));

    QStandardItem* pc = makeTreeItem(S("browse.tree.thisPC"), QString(), false);
    pc->setData(true, Qt::UserRole + 2);
    const QString desk = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!desk.isEmpty()) pc->appendRow(makeTreeItem(S("browse.tree.desktop"), desk, true));
    if (cfg.getBool(QStringLiteral("FileList/ShowLocalDrives"))) {
        const QFileInfoList drives = QDir::drives();
        for (int i = 0; i < drives.size(); ++i) {
            const QString dp = drives.at(i).absoluteFilePath();
            QString label = QDir::toNativeSeparators(dp);
            if (label.endsWith(QLatin1Char('\\'))) label.chop(1);
            pc->appendRow(makeTreeItem(label, dp, true));
        }
    }
    root->appendRow(pc);

    QStandardItem* lib = makeTreeItem(S("browse.tree.library"), QString(), false);
    lib->setData(true, Qt::UserRole + 2);
    QList<QStandardPaths::StandardLocation> locs;
    locs << QStandardPaths::DocumentsLocation << QStandardPaths::PicturesLocation
         << QStandardPaths::MusicLocation << QStandardPaths::MoviesLocation
         << QStandardPaths::DownloadLocation;
    for (int i = 0; i < locs.size(); ++i) {
        const QString lp = QStandardPaths::writableLocation(locs.at(i));
        if (lp.isEmpty() || !QFileInfo(lp).isDir()) continue;
        lib->appendRow(makeTreeItem(QFileInfo(lp).fileName(), lp, true));
    }
    root->appendRow(lib);
    // TODO-CONFIRM: "网络"节点未做(没有可靠的实现,不放假节点)

    tree_->expand(treeModel_->indexFromItem(pc));
}

// 缩略图方式下是否应隐藏预览窗格
bool BrowserWidget::previewShouldHide() const
{
    const bool thumbs = (mode_ == ViewMode::ThumbInfo || mode_ == ViewMode::Thumbs);
    return thumbs && Config::instance().getBool(QStringLiteral("Browse/HidePreviewInThumbMode"));
}

// 保存最近文件夹列表
void BrowserWidget::rememberFolder(const QString& path)
{
    Q_UNUSED(path);
    if (!Config::instance().getBool(QStringLiteral("Browse/SaveRecentFolders"))) return;
    State::instance().setList(QStringLiteral("recentFolders"), recentFolders());
}

// 按显示方式配置 listView_
void BrowserWidget::configureListView()
{
    Config& cfg = Config::instance();
    ThumbDelegate* td = static_cast<ThumbDelegate*>(thumbDelegate_);
    switch (mode_) {
    case ViewMode::ThumbInfo:
    case ViewMode::Thumbs: {
        td->configure(mode_ == ViewMode::ThumbInfo, cfg.getInt(QStringLiteral("Thumbnails/Border")),
                      cfg.getBool(QStringLiteral("Thumbnails/ShowImageArea")),
                      cfg.getBool(QStringLiteral("Thumbnails/ShowFileSize")));
        td->configureStyle(cfg.getInt(QStringLiteral("Thumbnails/CellRadius")), cfg.getInt(QStringLiteral("Thumbnails/ImageRadius")),
                           cfg.getBool(QStringLiteral("Thumbnails/ImageBorder")), cfg.getBool(QStringLiteral("Thumbnails/HoverHighlight")),
                           cfg.getInt(QStringLiteral("Thumbnails/FocusWidth")));
        listView_->setItemDelegate(thumbDelegate_);
        listView_->setViewMode(QListView::IconMode);
        listView_->setFlow(QListView::LeftToRight);
        listView_->setIconSize(ThumbnailCache::instance().thumbSize());
        listView_->setGridSize(td->cellSize());
        listView_->setWordWrap(false);
        break;
    }
    case ViewMode::Icons:
        listView_->setItemDelegate(plainDelegate_);
        listView_->setViewMode(QListView::IconMode);
        listView_->setFlow(QListView::LeftToRight);
        listView_->setIconSize(QSize(32, 32));
        listView_->setGridSize(QSize(96, 76));
        listView_->setWordWrap(true);
        break;
    case ViewMode::List:
    default:
        listView_->setItemDelegate(plainDelegate_);
        listView_->setViewMode(QListView::ListMode);
        listView_->setFlow(QListView::TopToBottom);
        listView_->setIconSize(QSize(16, 16));
        listView_->setGridSize(QSize());
        listView_->setWordWrap(false);
        break;
    }
    listView_->setWrapping(true);
    listView_->setResizeMode(QListView::Adjust);
    listView_->setMovement(QListView::Static);
    listView_->setUniformItemSizes(true);
    listView_->setSpacing(0);
    listView_->setModelColumn(0);
}

// 按各开关更新左侧(目录树+预览)的显示
void BrowserWidget::updateLeftPane()
{
    const bool showPreview = previewVisible_ && !previewShouldHide();
    tree_->setVisible(treeVisible_);
    preview_->setVisible(showPreview);
    vSplit_->setVisible(treeVisible_ || showPreview);
    if (showPreview) previewTimer_->start();
}
