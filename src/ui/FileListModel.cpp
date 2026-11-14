#include "FileListModel.h"
#include "core/Config.h"
#include "core/Strings.h"
#include "core/FileOps.h"
#include "core/ThumbnailCache.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QTimer>
#include <QBrush>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// 该文件夹是否在固定磁盘上(非 Windows 一律视为是)
bool isFixedDrive(const QString& folder)
{
#ifdef Q_OS_WIN
    const QString root = QDir::toNativeSeparators(QFileInfo(folder).absoluteFilePath().left(3));
    return GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16())) == DRIVE_FIXED;
#else
    Q_UNUSED(folder);
    return true;
#endif
}

bool isThumbMode(ViewMode m)
{
    return m == ViewMode::ThumbInfo || m == ViewMode::Thumbs;
}

} // namespace

// 构造
FileListModel::FileListModel(QObject* parent)
    : QAbstractTableModel(parent), list_(nullptr), parentRow_(false), mode_(ViewMode::Details),
      sizeInKB_(true), highlightImages_(true), useSystemIcons_(true), readInfoPolicy_(0),
      hasParentNow_(false), readInfoNow_(true), infoTips_(true), thumbShowFolders_(true), thumbShowAll_(false), showShortcuts_(true), descEditable_(false), extFilterOn_(false)
{
    highlightColor_ = QColor(0xE6, 0xF0, 0xFF);
    connect(&ThumbnailCache::instance(), &ThumbnailCache::ready, this, &FileListModel::onThumbReady);
    connect(&ThumbnailCache::instance(), &ThumbnailCache::failed, this,
            [this](const QString& path, const QString& error) {
                failed_.insert(path, error);
                onInfoReady(path);
            });
    applyOptions();
}

// 绑定文件夹列表
void FileListModel::setFolderList(FolderList* list)
{
    list_ = list;
    reload();
}

// 从 FolderList 重新装载数据
void FileListModel::reload()
{
    beginResetModel();
    rows_.clear();
    rowIndex_.clear();
    hasParentNow_ = false;
    readInfoNow_ = (readInfoPolicy_ == 1);
    descr_.reset();
    if (list_ != nullptr && !list_->folder().isEmpty()) {
        descr_.reset(new DescriptIon(list_->folder()));
        QString derr;
        if (!descr_->load(&derr)) {
            descr_.reset();
            if (descrErrFolder_ != list_->folder()) {
                descrErrFolder_ = list_->folder();
                FileOps::showError(QApplication::activeWindow(), derr);
            }
        }
        QDir d(list_->folder());
        hasParentNow_ = d.cdUp();
        if (readInfoPolicy_ == 0) readInfoNow_ = isFixedDrive(list_->folder());
        const QList<FileEntry>& src = list_->entries();
        for (int i = 0; i < src.size(); ++i) {
            if (visible(src.at(i))) rows_.append(src.at(i));
        }
    }
    const int off = hasParentRow() ? 1 : 0;
    for (int i = 0; i < rows_.size(); ++i) rowIndex_.insert(rows_.at(i).path, i + off);
    endResetModel();
}

// 重新读取配置
void FileListModel::applyOptions()
{
    Config& c = Config::instance();
    beginResetModel();
    sizeInKB_ = c.getBool(QStringLiteral("FileList/SizeInKB"));
    highlightImages_ = c.getBool(QStringLiteral("FileList/HighlightImages"));
    highlightColor_ = c.getColor(QStringLiteral("FileList/HighlightColor"));
    useSystemIcons_ = c.getBool(QStringLiteral("FileList/UseSystemIcons"));
    const QString pol = c.getEnum(QStringLiteral("FileList/ReadImageInfo"));
    readInfoPolicy_ = (pol == QLatin1String("FixedOnly")) ? 0 : (pol == QLatin1String("Always") ? 1 : 2);
    infoTips_ = c.getBool(QStringLiteral("Misc/ShowInfoTips"));
    thumbShowFolders_ = c.getBool(QStringLiteral("Thumbnails/ShowFolders"));
    thumbShowAll_ = c.getBool(QStringLiteral("Thumbnails/ShowAllFiles"));
    descEditable_ = c.getBool(QStringLiteral("Browse/DoubleClickEditDescription"));
    showShortcuts_ = c.getBool(QStringLiteral("Browse/ShowShortcuts"));
    iconCache_.clear();
    endResetModel();
    reload();
}

// 行数
int FileListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return rows_.size() + (hasParentRow() ? 1 : 0);
}

// 列数
int FileListModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return 4;
}

// 数据
QVariant FileListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount(QModelIndex())) return QVariant();
    const int row = index.row();
    const int col = index.column();
    const bool isParent = hasParentRow() && row == 0;
    if (isParent) {
        if (role == IsParentRole || role == IsDirRole) return true;
        if (role == IsImageRole) return false;
        if (col != 0) return QVariant();
        if (role == Qt::DisplayRole) return S("browse.parentDir");
        if (role == Qt::DecorationRole) return iconProvider_.icon(QFileIconProvider::Folder);
        return QVariant();
    }
    const FileEntry& e = rows_.at(row - (hasParentRow() ? 1 : 0));
    ThumbnailCache& tc = ThumbnailCache::instance();
    const bool image = e.isImage && !e.isDir;
    switch (role) {
    case PathRole: return e.path;
    case IsDirRole: return e.isDir;
    case IsImageRole: return image;
    case IsParentRole: return false;
    case ErrorRole: return failed_.value(e.path);
    case ThumbRole: {
        if (!image) return QVariant();
        const QImage img = tc.cached(e.path);
        if (img.isNull() && !failed_.contains(e.path)) tc.request(e.path);
        return img;
    }
    case InfoLineRole: {
        if (!image) return QString();
        const ImageInfo info = tc.cachedInfo(e.path);
        if (!info.valid) {
            if (!failed_.contains(e.path)) tc.request(e.path);
            return QString();
        }
        return imageInfoLine(info, false);
    }
    case Qt::DisplayRole:
        if (col == 0) return e.name;
        if (col == 1) return sizeText(e);
        if (col == 2) return infoText(e);
        if (col == 3 && descr_ && !e.isDir) return descr_->get(e.name);
        return QString();
    case Qt::EditRole:
        if (col == 0) return e.name;
        if (col == 3 && descr_ && !e.isDir) return descr_->get(e.name);
        return QVariant();
    case Qt::TextAlignmentRole:
        if (col == 1) return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::DecorationRole: {
        if (col != 0) return QVariant();
        if (!useSystemIcons_) {
            return iconProvider_.icon(e.isDir ? QFileIconProvider::Folder : QFileIconProvider::File);
        }
        if (e.isDir) {
            if (!iconCache_.contains(QStringLiteral(":dir"))) {
                iconCache_.insert(QStringLiteral(":dir"), iconProvider_.icon(QFileIconProvider::Folder));
            }
            return iconCache_.value(QStringLiteral(":dir"));
        }
        const QString suffix = QFileInfo(e.path).suffix().toLower();
        if (!iconCache_.contains(suffix)) iconCache_.insert(suffix, iconProvider_.icon(QFileInfo(e.path)));
        return iconCache_.value(suffix);
    }
    case Qt::BackgroundRole:
        if (highlightImages_ && image && !isThumbMode(mode_)) return QBrush(highlightColor_);
        return QVariant();
    case Qt::ToolTipRole: {
        if (!infoTips_) return QVariant();
        QString tip = e.name;
        if (!e.isDir) tip += QLatin1Char('\n') + sizeText(e);
        tip += QLatin1Char('\n') + e.modified.toString(QStringLiteral("yyyy/M/d H:mm"));
        if (image) {
            const ImageInfo info = tc.cachedInfo(e.path);
            if (info.valid) tip += QLatin1Char('\n') + imageInfoLine(info, true);
        }
        if (failed_.contains(e.path)) tip += QLatin1Char('\n') + failed_.value(e.path);
        return tip;
    }
    default:
        break;
    }
    return QVariant();
}

// 表头
QVariant FileListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
    switch (section) {
    case 0: return S("browse.col.name");
    case 1: return S("browse.col.size");
    case 2: return S("browse.col.info");
    case 3: return S("browse.col.desc");
    default: break;
    }
    return QVariant();
}

// 标志
Qt::ItemFlags FileListModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const bool isParent = hasParentRow() && index.row() == 0;
    if (index.column() == 0 && !isParent) f |= Qt::ItemIsEditable;
    if (index.column() == 3 && !isParent && descEditable_ && descr_ && !entryAt(index.row()).isDir) f |= Qt::ItemIsEditable;
    return f;
}

// 编辑:第 0 列为重命名,第 3 列为描述(写 descript.ion)
bool FileListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !index.isValid()) return false;
    if (hasParentRow() && index.row() == 0) return false;
    const FileEntry e = entryAt(index.row());
    if (index.column() == 3) {
        if (!descr_ || e.isDir) return false;
        descr_->set(e.name, value.toString());
        QString derr;
        if (!descr_->save(&derr)) {
            FileOps::showError(QApplication::activeWindow(), derr);
            return false;
        }
        emit dataChanged(index, index);
        return true;
    }
    if (index.column() != 0) return false;
    const QString newName = value.toString().trimmed();
    if (newName.isEmpty() || newName == e.name) return false;
    QString newPath;
    QString err;
    if (!FileOps::renameTo(e.path, newName, &newPath, &err)) {
        FileOps::showError(QApplication::activeWindow(), err);
        return false;
    }
    const QString oldPath = e.path;
    QTimer::singleShot(0, this, [this, oldPath, newPath]() {
        if (list_ != nullptr) list_->refresh();
        emit renamed(oldPath, newPath);
    });
    return true;
}

// 某行对应的文件项
FileEntry FileListModel::entryAt(int row) const
{
    const int off = hasParentRow() ? 1 : 0;
    const int i = row - off;
    if (i < 0 || i >= rows_.size()) return FileEntry();
    return rows_.at(i);
}

// 某路径所在的行
int FileListModel::rowOfPath(const QString& path) const
{
    QHash<QString, int>::const_iterator it = rowIndex_.constFind(path);
    if (it != rowIndex_.constEnd()) return it.value();
#ifdef Q_OS_WIN
    const int off = hasParentRow() ? 1 : 0;
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_.at(i).path.compare(path, Qt::CaseInsensitive) == 0) return i + off;
    }
#endif
    return -1;
}

// 是否有 ".." 行
bool FileListModel::hasParentRow() const
{
    return parentRow_ && hasParentNow_;
}

// 设置是否显示 ".." 行
void FileListModel::setParentRowVisible(bool visible)
{
    if (parentRow_ == visible) return;
    parentRow_ = visible;
    reload();
}

// 设置显示方式
void FileListModel::setViewMode(ViewMode mode)
{
    mode_ = mode;
    reload();
}

// 设置扩展名筛选
void FileListModel::setExtFilter(bool on, const QStringList& exts)
{
    extFilterOn_ = on;
    extFilter_ = exts;
}

// 清除读取失败记录
void FileListModel::clearFailed()
{
    failed_.clear();
}

// 缩略图生成完毕
void FileListModel::onThumbReady(const QString& path)
{
    const int r = rowOfPath(path);
    if (r >= 0) emit dataChanged(index(r, 0), index(r, 2));
}

// 信息读取完毕/失败
void FileListModel::onInfoReady(const QString& path)
{
    const int r = rowOfPath(path);
    if (r >= 0) emit dataChanged(index(r, 0), index(r, 2));
}

// 行号转 FolderList 下标
int FileListModel::entryIndex(int row) const
{
    return row - (hasParentRow() ? 1 : 0);
}

// 图片属性文字(没有读到时先请求,读到后通过 dataChanged 刷新)
QString FileListModel::infoText(const FileEntry& e) const
{
    if (e.isDir || !e.isImage || !readInfoNow_) return QString();
    ThumbnailCache& tc = ThumbnailCache::instance();
    const ImageInfo info = tc.cachedInfo(e.path);
    if (info.valid) return imageInfoLine(info, true);
    if (failed_.contains(e.path)) return failed_.value(e.path);
    tc.request(e.path);
    return QString();
}

// 文件大小文字
QString FileListModel::sizeText(const FileEntry& e) const
{
    if (e.isDir) return QString();
    if (sizeInKB_) {
        const qint64 kb = (e.size + 1023) / 1024;
        return S("status.sizeKB").arg(QLocale().toString(kb));
    }
    return S("status.sizeB").arg(QLocale().toString(e.size));
}

// 当前模式下是否显示此项
bool FileListModel::visible(const FileEntry& e) const
{
    if (!visibleIgnoringFilter(e)) return false;
    if (extFilterOn_ && !e.isDir && !extFilter_.contains(QFileInfo(e.name).suffix().toLower())) return false;
    return true;
}

// 不考虑扩展名筛选时是否显示此项
bool FileListModel::visibleIgnoringFilter(const FileEntry& e) const
{
    // Browse/ShowShortcuts 关闭时隐藏 .lnk 快捷方式文件
    if (!e.isDir && !showShortcuts_ && e.name.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive)) return false;
    if (isThumbMode(mode_)) {
        if (e.isDir) return thumbShowFolders_;
        if (!e.isImage) return thumbShowAll_;
    }
    return true;
}

// 每种扩展名的文件数
QMap<QString, int> FileListModel::extensionCounts() const
{
    QMap<QString, int> out;
    if (list_ == nullptr) return out;
    const QList<FileEntry>& src = list_->entries();
    for (int i = 0; i < src.size(); ++i) {
        const FileEntry& e = src.at(i);
        if (e.isDir || !visibleIgnoringFilter(e)) continue;
        out[QFileInfo(e.name).suffix().toLower()] += 1;
    }
    return out;
}
