#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QTimer>

// 文件夹里的一项
struct FileEntry {
    QString path;        // 完整路径
    QString name;        // 文件名
    bool isDir;          // 是否文件夹
    bool isImage;        // 扩展名在图片扩展名列表里
    bool isHidden;       // 隐藏属性
    bool isExtra;        // 是否"添加到列表"加进来的外部文件
    qint64 size;         // 字节数
    QDateTime modified;  // 修改时间
    FileEntry() : isDir(false), isImage(false), isHidden(false), isExtra(false), size(0) {}
};

// 排序依据
enum class SortKey { Name, Size, Date, Type };

// 当前文件夹的内容列表(浏览窗口和查看窗口共用)
class FolderList : public QObject {
    Q_OBJECT
public:
    // 构造
    explicit FolderList(QObject* parent);
    // 进入文件夹并扫描;失败返回 false 并写 error(含文件夹名和原因)
    bool setFolder(const QString& path, QString* error);
    // 当前文件夹路径(无则空)
    QString folder() const;
    // 重新扫描(扩展名或选项变化、手动刷新)
    void refresh();
    // 浏览窗口要显示的全部项(已按选项过滤和排序,文件夹在前)
    const QList<FileEntry>& entries() const;
    // 查看窗口要翻页的图片(只含图片,含 extra 文件)
    QList<FileEntry> imageEntries() const;
    // 在 imageEntries 中查找某路径的下标,没有返回 -1
    int imageIndexOf(const QString& path) const;
    // 图片总数
    int imageCount() const;
    // 总文件数(不含文件夹)和总字节数
    int fileCount() const;
    qint64 totalBytes() const;
    // 设置排序
    void setSort(SortKey key, bool descending);
    SortKey sortKey() const;
    bool sortDescending() const;
    // 添加外部文件到列表(不在当前文件夹里的图片);成功返回 true
    bool addExtraFile(const QString& path, QString* error);
    // 清除所有外部文件
    void clearExtraFiles();
    // 某个路径是否为扩展名列表里的图片
    static bool isImagePath(const QString& path);
    // 当前图片扩展名列表(小写,不带点)
    static QStringList imageExtensions();
    // 名称自然排序比较(file2 < file10,忽略大小写)
    static int naturalCompare(const QString& a, const QString& b);
    // 启用/停用文件夹变化自动刷新(读 FileList/AutoRefresh)
    void applyOptions();
    // 是否把所有子文件夹里的图片也并进当前列表(平铺显示,文件名显示为 子文件夹/文件名);开关存在 QuickView.state
    void setIncludeSubfolders(bool on);
    bool includeSubfolders() const;
    // 平铺时最多收多少个文件(防止在盘符根目录一次扫太多卡死)
    static int subfolderLimit();

public:
    // 上一次扫描因为超过上限而没收完子文件夹里的图片
    bool subfoldersTruncated() const { return subTruncated_; }

signals:
    // 列表内容已改变(扫描完成、刷新)
    void changed();
    // 自动刷新发现了新文件
    void newFilesFound(const QStringList& paths);

private slots:
    // 文件夹被系统通知有变化
    void onDirectoryChanged(const QString& path);
    // 合并短时间内的多次变化后刷新
    void onRefreshTimer();

private:
    // 扫描磁盘填充 all_
    bool scan(QString* error);
    // 按选项过滤并排序,生成 entries_
    void rebuildView();

    QString folder_;
    bool includeSub_ = false;
    bool subTruncated_ = false;
    QList<FileEntry> all_;       // 扫描到的全部项
    QList<FileEntry> entries_;   // 过滤排序后
    QList<FileEntry> extras_;    // 外部添加的文件
    SortKey sortKey_;
    bool sortDesc_;
    QFileSystemWatcher* watcher_;
    QTimer* refreshTimer_;
    int fileCount_;
    qint64 totalBytes_;
};
