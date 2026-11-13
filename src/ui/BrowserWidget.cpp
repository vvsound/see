#include "BrowserWidget.h"

#include <QStyle>
#include "core/Config.h"
#include "core/State.h"
#include "core/Strings.h"
#include "core/ImageLoader.h"
#include "core/ThumbnailCache.h"

#include <QVBoxLayout>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QPainter>
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

// 缩略图方式的单元格绘制:上面缩略图,下面(缩略图+信息方式)宽x高x色深和文件名
class ThumbDelegate : public QStyledItemDelegate {
public:
    explicit ThumbDelegate(QObject* parent)
        : QStyledItemDelegate(parent), withText_(true), border_(6), showArea_(true), showSize_(false) {}

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
        const bool sel = (option.state & QStyle::State_Selected) != 0;
        const QRect cell = option.rect;
        if (sel) painter->fillRect(cell, option.palette.highlight());
        const QSize ts = ThumbnailCache::instance().thumbSize();
        const QRect imgArea(cell.left() + border_, cell.top() + border_, cell.width() - 2 * border_, ts.height());
        const bool isImage = index.data(FileListModel::IsImageRole).toBool();
        const QImage th = isImage ? index.data(FileListModel::ThumbRole).value<QImage>() : QImage();
        if (!th.isNull()) {
            QRect tr(QPoint(0, 0), th.size());
            tr.moveCenter(imgArea.center());
            painter->drawImage(tr, th);
            painter->setPen(QColor(128, 128, 128));
            painter->drawRect(tr.adjusted(0, 0, -1, -1));
        } else if (isImage) {
            const QString err = index.data(FileListModel::ErrorRole).toString();
            QFont f = option.font;
            f.setPointSize(f.pointSize() * 2);
            painter->setFont(f);
            painter->setPen(err.isEmpty() ? QColor(150, 150, 150) : QColor(200, 50, 50));
            painter->drawText(imgArea, Qt::AlignCenter, err.isEmpty() ? QStringLiteral("...") : QStringLiteral("x"));
        } else {
            const QIcon ic = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            const int side = qMin(48, qMin(imgArea.width(), imgArea.height()));
            QRect ir(0, 0, side, side);
            ir.moveCenter(imgArea.center());
            ic.paint(painter, ir);
        }
        if (withText_) {
            const QFontMetrics fm(option.font);
            const int lh = fm.height();
            painter->setFont(option.font);
            painter->setPen(sel ? option.palette.highlightedText().color() : option.palette.text().color());
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
            painter->drawText(l1, Qt::AlignHCenter | Qt::AlignVCenter, fm.elidedText(info, Qt::ElideRight, l1.width()));
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
};

} // namespace

// 构造
BrowserWidget::BrowserWidget(QWidget* parent)
    : QWidget(parent), pathBox_(nullptr), hSplit_(nullptr), vSplit_(nullptr), tree_(nullptr), treeModel_(nullptr),
      preview_(nullptr), viewStack_(nullptr), listView_(nullptr), detailView_(nullptr), model_(nullptr),
      folderList_(nullptr), previewTimer_(nullptr), thumbDelegate_(nullptr), plainDelegate_(nullptr),
      mode_(ViewMode::Details), treeVisible_(true), previewVisible_(true), pathBarVisible_(true)
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
    viewStack_ = new QStackedWidget(hSplit_);
    hSplit_->setStretchFactor(0, 0);
    hSplit_->setStretchFactor(1, 1);
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

    thumbDelegate_ = new ThumbDelegate(this);
    plainDelegate_ = new QStyledItemDelegate(this);

    viewStack_->addWidget(listView_);
    viewStack_->addWidget(detailView_);
    viewStack_->setCurrentWidget(detailView_);

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(30);

    // 右键:在视口上用自定义菜单策略
    listView_->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    detailView_->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    listView_->viewport()->installEventFilter(this);
    detailView_->viewport()->installEventFilter(this);
    connect(listView_->viewport(), &QWidget::customContextMenuRequested, this, &BrowserWidget::onContextMenuRequested);
    connect(detailView_->viewport(), &QWidget::customContextMenuRequested, this, &BrowserWidget::onContextMenuRequested);

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
}

// 进入文件夹;失败弹出错误并返回 false
bool BrowserWidget::openFolder(const QString& path)
{
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    QString err;
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
    const bool thumbs = (mode == ViewMode::ThumbInfo || mode == ViewMode::Thumbs);
    model_->setParentRowVisible(!thumbs);
    model_->setViewMode(mode);
    if (mode == ViewMode::Details) {
        viewStack_->setCurrentWidget(detailView_);
    } else {
        configureListView();
        viewStack_->setCurrentWidget(listView_);
    }
    updateLeftPane();
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

// 上一级
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

// 把焦点交给当前视图(缩略图/图标/列表/详细),方向键才能选文件
void BrowserWidget::focusFileArea()
{
    activeView()->setFocus();
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
}

// 预览显示/隐藏
void BrowserWidget::setPreviewVisible(bool v)
{
    previewVisible_ = v;
    updateLeftPane();
}

// 路径栏显示/隐藏
void BrowserWidget::setPathBarVisible(bool v)
{
    pathBarVisible_ = v;
    pathBox_->setVisible(v);
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
    detailView_->header()->setSortIndicator(sk, desc ? Qt::DescendingOrder : Qt::AscendingOrder);
    setViewMode(m);
}

// 事件过滤:右键时先选中光标下的项
bool BrowserWidget::eventFilter(QObject* obj, QEvent* e)
{
    // Ctrl+滚轮:缩略图方式下放大/缩小缩略图(每格 16 像素,范围 32~256)
    if (e->type() == QEvent::Wheel && obj == listView_->viewport()
        && (mode_ == ViewMode::ThumbInfo || mode_ == ViewMode::Thumbs)) {
        QWheelEvent* we = static_cast<QWheelEvent*>(e);
        if ((we->modifiers() & Qt::ControlModifier) != 0 && we->angleDelta().y() != 0) {
            Config& cfg = Config::instance();
            const int cur = cfg.getInt(QStringLiteral("Thumbnails/Width"));
            const int next = std::max(32, std::min(256, cur + (we->angleDelta().y() > 0 ? 16 : -16)));
            if (next != cur) {
                cfg.setInt(QStringLiteral("Thumbnails/Width"), next);
                cfg.setInt(QStringLiteral("Thumbnails/Height"), next);
                ThumbnailCache::instance().configure();
                configureListView();
                listView_->viewport()->update();
            }
            return true;
        }
    }
    if (e->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::RightButton) {
            QAbstractItemView* v = (obj == listView_->viewport()) ? static_cast<QAbstractItemView*>(listView_)
                                                                  : static_cast<QAbstractItemView*>(detailView_);
            const QModelIndex idx = v->indexAt(me->pos());
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
    return QWidget::eventFilter(obj, e);
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
}

// 预览延迟更新
void BrowserWidget::onPreviewTimer()
{
    if (!preview_->isVisible()) return;
    const QString cur = currentFilePath();
    if (cur.isEmpty() || !FolderList::isImagePath(cur) || !QFileInfo(cur).isFile()) {
        preview_->clear();
        return;
    }
    preview_->showFile(cur);
}

// 右键:通知主窗口弹菜单
void BrowserWidget::onContextMenuRequested(const QPoint& pos)
{
    QWidget* w = qobject_cast<QWidget*>(sender());
    if (w == nullptr) return;
    emit contextMenuRequested(w->mapToGlobal(pos));
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
    const QModelIndex idx = treeModel_->indexFromItem(found);
    tree_->setCurrentIndex(idx);
    tree_->scrollTo(idx);
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
