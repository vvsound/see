#include "FolderList.h"
#include "Config.h"
#include "Strings.h"
#include "ImageLoader.h"

#include <QDir>
#include <QFileInfo>
#include <QCollator>
#include <QSet>
#include <algorithm>

// 两个路径是否指向同一文件(Windows 不区分大小写)
static bool samePath(const QString& a, const QString& b)
{
#ifdef Q_OS_WIN
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
}

// 文件名的扩展名是否在集合里(exts 为小写、无点)
static bool hasImageExt(const QString& name, const QSet<QString>& exts)
{
    int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) return false;
    return exts.contains(name.mid(dot + 1).toLower());
}

// 构造
FolderList::FolderList(QObject* parent) : QObject(parent)
{
    sortKey_ = SortKey::Name;
    sortDesc_ = false;
    fileCount_ = 0;
    totalBytes_ = 0;
    watcher_ = new QFileSystemWatcher(this);
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(Config::instance().getInt(QStringLiteral("Browse/AutoRefreshDelayMs")));
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, &FolderList::onDirectoryChanged);
    connect(refreshTimer_, &QTimer::timeout, this, &FolderList::onRefreshTimer);
}

// 进入文件夹并扫描
bool FolderList::setFolder(const QString& path, QString* error)
{
    QDir d(path);
    if (!d.exists()) {
        if (error) *error = S("err.folderOpen").arg(path, S("err.folderNotFound"));
        return false;
    }
    const QString newFolder = QDir::cleanPath(d.absolutePath());
    const QString oldFolder = folder_;
    folder_ = newFolder;
    if (!samePath(oldFolder, newFolder)) extras_.clear();
    if (!scan(error)) {
        folder_ = oldFolder;
        return false;
    }
    applyOptions();  // 内部会重建列表、更新监视并发出 changed
    return true;
}

// 当前文件夹路径
QString FolderList::folder() const
{
    return folder_;
}

// 重新扫描
void FolderList::refresh()
{
    if (folder_.isEmpty()) return;
    QString err;
    if (!scan(&err)) {
        all_.clear();  // 文件夹已不可读:列表清空
    }
    rebuildView();
    emit changed();
}

// 浏览窗口要显示的全部项
const QList<FileEntry>& FolderList::entries() const
{
    return entries_;
}

// 查看窗口要翻页的图片
QList<FileEntry> FolderList::imageEntries() const
{
    QList<FileEntry> out;
    for (int i = 0; i < entries_.size(); ++i) {
        const FileEntry& e = entries_.at(i);
        if (!e.isDir && e.isImage) out.append(e);
    }
    return out;
}

// 在 imageEntries 中查找某路径的下标
int FolderList::imageIndexOf(const QString& path) const
{
    int idx = 0;
    for (int i = 0; i < entries_.size(); ++i) {
        const FileEntry& e = entries_.at(i);
        if (e.isDir || !e.isImage) continue;
        if (samePath(e.path, path)) return idx;
        ++idx;
    }
    return -1;
}

// 图片总数
int FolderList::imageCount() const
{
    int n = 0;
    for (int i = 0; i < entries_.size(); ++i) {
        if (!entries_.at(i).isDir && entries_.at(i).isImage) ++n;
    }
    return n;
}

// 总文件数(不含文件夹)
int FolderList::fileCount() const
{
    return fileCount_;
}

// 总字节数
qint64 FolderList::totalBytes() const
{
    return totalBytes_;
}

// 设置排序
void FolderList::setSort(SortKey key, bool descending)
{
    sortKey_ = key;
    sortDesc_ = descending;
    rebuildView();
    emit changed();
}

SortKey FolderList::sortKey() const
{
    return sortKey_;
}

bool FolderList::sortDescending() const
{
    return sortDesc_;
}

// 添加外部文件到列表
bool FolderList::addExtraFile(const QString& path, QString* error)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (error) *error = S("err.readFile").arg(fi.fileName(), S("err.fileNotFound"));
        return false;
    }
    const QString full = fi.absoluteFilePath();
    // 已经在当前文件夹的图片列表里,不用再加
    if (samePath(fi.absolutePath(), folder_) && isImagePath(full)) return true;
    for (int i = 0; i < extras_.size(); ++i) {
        if (samePath(extras_.at(i).path, full)) return true;
    }
    if (!ImageLoader::instance().canDecodeFile(full)) {
        if (error) *error = S("err.noDecoder").arg(fi.fileName());
        return false;
    }
    FileEntry e;
    e.path = full;
    e.name = fi.fileName();
    e.isDir = false;
    e.isImage = true;
    e.isHidden = fi.isHidden();
    e.isExtra = true;
    e.size = fi.size();
    e.modified = fi.lastModified();
    extras_.append(e);
    rebuildView();
    emit changed();
    return true;
}

// 清除所有外部文件
void FolderList::clearExtraFiles()
{
    extras_.clear();
    rebuildView();
    emit changed();
}

// 某个路径是否为扩展名列表里的图片
bool FolderList::isImagePath(const QString& path)
{
    QStringList list = imageExtensions();
    QSet<QString> exts(list.begin(), list.end());
    return hasImageExt(path, exts);
}

// 当前图片扩展名列表(小写,不带点)
QStringList FolderList::imageExtensions()
{
    QStringList raw = Config::instance().getList(QStringLiteral("Files/Extensions"));
    QStringList out;
    for (int i = 0; i < raw.size(); ++i) {
        QString e = raw.at(i).trimmed().toLower();
        while (e.startsWith(QLatin1Char('.'))) e = e.mid(1);
        if (!e.isEmpty() && !out.contains(e)) out.append(e);
    }
    return out;
}

// 名称自然排序比较(file2 < file10,忽略大小写)
int FolderList::naturalCompare(const QString& a, const QString& b)
{
    static QCollator* col = nullptr;
    if (!col) {
        col = new QCollator();
        col->setNumericMode(true);
        col->setCaseSensitivity(Qt::CaseInsensitive);
    }
    return col->compare(a, b);
}

// 启用/停用文件夹变化自动刷新,并按当前选项重新过滤
void FolderList::applyOptions()
{
    QStringList dirs = watcher_->directories();
    if (!dirs.isEmpty()) watcher_->removePaths(dirs);
    if (!folder_.isEmpty() && Config::instance().getBool(QStringLiteral("FileList/AutoRefresh"))) {
        watcher_->addPath(folder_);
    }
    if (!folder_.isEmpty()) {
        rebuildView();
        emit changed();
    }
}

// 文件夹被系统通知有变化:合并短时间内的多次变化
void FolderList::onDirectoryChanged(const QString& path)
{
    Q_UNUSED(path);
    if (!Config::instance().getBool(QStringLiteral("FileList/AutoRefresh"))) return;
    refreshTimer_->start();
}

// 合并后刷新,并报告新出现的图片
void FolderList::onRefreshTimer()
{
    QSet<QString> oldPaths;
    for (int i = 0; i < all_.size(); ++i) oldPaths.insert(all_.at(i).path);

    QString err;
    if (!scan(&err)) {
        all_.clear();
    }
    rebuildView();
    emit changed();

    QStringList added;
    for (int i = 0; i < all_.size(); ++i) {
        const FileEntry& e = all_.at(i);
        if (!e.isDir && e.isImage && !oldPaths.contains(e.path)) added.append(e.path);
    }
    if (!added.isEmpty()) emit newFilesFound(added);
}

// 扫描磁盘填充 all_
bool FolderList::scan(QString* error)
{
    QDir dir(folder_);
    if (!dir.exists()) {
        if (error) *error = S("err.folderOpen").arg(folder_, S("err.folderNotFound"));
        return false;
    }
    if (!dir.isReadable()) {
        if (error) *error = S("err.folderOpen").arg(folder_, S("err.folderNotReadable"));
        return false;
    }
    QStringList extList = imageExtensions();
    QSet<QString> exts(extList.begin(), extList.end());

    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    QFileInfoList list = dir.entryInfoList();
    all_.clear();
    for (int i = 0; i < list.size(); ++i) {
        const QFileInfo& fi = list.at(i);
        FileEntry e;
        e.path = fi.absoluteFilePath();
        e.name = fi.fileName();
        e.isDir = fi.isDir();
        e.isHidden = fi.isHidden();
        e.isExtra = false;
        e.size = e.isDir ? 0 : fi.size();
        e.modified = fi.lastModified();
        e.isImage = !e.isDir && hasImageExt(e.name, exts);
        all_.append(e);
    }
    return true;
}

// 按选项过滤并排序,生成 entries_
void FolderList::rebuildView()
{
    const bool hideNonImages = Config::instance().getBool(QStringLiteral("FileList/HideNonImages"));
    const bool showHidden = Config::instance().getBool(QStringLiteral("FileList/ShowHidden"));

    entries_.clear();
    for (int i = 0; i < all_.size(); ++i) {
        const FileEntry& e = all_.at(i);
        if (e.isHidden && !showHidden) continue;
        if (!e.isDir && hideNonImages && !e.isImage) continue;
        entries_.append(e);
    }
    for (int i = 0; i < extras_.size(); ++i) entries_.append(extras_.at(i));

    const SortKey key = sortKey_;
    const bool desc = sortDesc_;
    std::stable_sort(entries_.begin(), entries_.end(),
        [key, desc](const FileEntry& a, const FileEntry& b) {
            if (a.isDir != b.isDir) return a.isDir;  // 文件夹永远在前
            int c = 0;
            if (key == SortKey::Size) {
                c = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
            } else if (key == SortKey::Date) {
                c = (a.modified < b.modified) ? -1 : (a.modified > b.modified ? 1 : 0);
            } else if (key == SortKey::Type) {
                c = QString::compare(QFileInfo(a.name).suffix(), QFileInfo(b.name).suffix(),
                                     Qt::CaseInsensitive);
            }
            if (c == 0) c = FolderList::naturalCompare(a.name, b.name);
            return desc ? (c > 0) : (c < 0);
        });

    fileCount_ = 0;
    totalBytes_ = 0;
    for (int i = 0; i < entries_.size(); ++i) {
        if (!entries_.at(i).isDir) {
            ++fileCount_;
            totalBytes_ += entries_.at(i).size;
        }
    }
}
