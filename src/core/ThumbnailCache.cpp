#include "ThumbnailCache.h"
#include "Config.h"
#include "Strings.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QMutexLocker>
#include <QMetaObject>

// 全局唯一实例
ThumbnailCache& ThumbnailCache::instance()
{
    static ThumbnailCache inst;
    return inst;
}

// 构造
ThumbnailCache::ThumbnailCache() : QObject(nullptr)
{
    running_ = 0;
    generation_ = 0;
    size_ = QSize(100, 100);
    diskEnabled_ = false;
    compress_ = true;
}

// 从配置读取缩略图尺寸、缓存开关等
void ThumbnailCache::configure()
{
    Config& c = Config::instance();
    const QSize ns(c.getInt(QStringLiteral("Thumbnails/Width")), c.getInt(QStringLiteral("Thumbnails/Height")));
    QString dir = c.getString(QStringLiteral("Thumbnails/CacheDir"));
    if (QDir::isRelativePath(dir)) dir = Config::appDir() + QLatin1Char('/') + dir;
    bool sizeChanged = false;
    {
        QMutexLocker lock(&mutex_);
        sizeChanged = (ns != size_);
        size_ = ns;
        diskEnabled_ = c.getBool(QStringLiteral("Thumbnails/CacheEnabled"));
        compress_ = c.getBool(QStringLiteral("Thumbnails/CacheCompress"));
        cacheDir_ = QDir::cleanPath(dir);
    }
    if (sizeChanged) clearMemory();
}

// 缩略图框尺寸
QSize ThumbnailCache::thumbSize() const
{
    QMutexLocker lock(&mutex_);
    return size_;
}

// 内存中已有的缩略图
QImage ThumbnailCache::cached(const QString& path) const
{
    return images_.value(path);
}

// 内存中的图片信息
ImageInfo ThumbnailCache::cachedInfo(const QString& path) const
{
    return infos_.value(path);
}

// 请求异步生成
void ThumbnailCache::request(const QString& path)
{
    if (images_.contains(path) || requested_.contains(path)) return;
    requested_.insert(path);
    queue_.append(path);
    startNext();
}

// 取消还没开始的请求(已在运行的结果会被丢弃)
void ThumbnailCache::cancelAll()
{
    {
        QMutexLocker lock(&mutex_);
        ++generation_;
    }
    queue_.clear();
    requested_.clear();
}

// 清空内存缓存
void ThumbnailCache::clearMemory()
{
    cancelAll();
    images_.clear();
    infos_.clear();
    order_.clear();
}

// 启动等待中的后台任务:最多 3 个同时进行,后请求的先做(滚动时先显示当前可见的)
void ThumbnailCache::startNext()
{
    while (running_ < 3 && !queue_.isEmpty()) {
        const QString p = queue_.takeLast();
        ++running_;
        QtConcurrent::run([this, p]() { runTask(p); });
    }
}

// 后台任务完成(主线程)
void ThumbnailCache::onTaskDone(const QString& path, const QImage& thumb, const ImageInfo& info, const QString& error)
{
    --running_;
    requested_.remove(path);
    if (info.valid) infos_.insert(path, info);
    if (!thumb.isNull()) {
        images_.insert(path, thumb);
        order_.append(path);
        if (order_.size() > 2500) {
            for (int i = 0; i < 500; ++i) {
                const QString old = order_.takeFirst();
                images_.remove(old);
                infos_.remove(old);
            }
        }
        emit ready(path);
    }
    if (!error.isEmpty()) emit failed(path, error);
    startNext();
}

// 后台线程里执行:读信息、取缩略图(磁盘缓存优先)
void ThumbnailCache::runTask(const QString& path)
{
    QSize sz;
    bool disk = false;
    bool comp = false;
    QString dir;
    int gen = 0;
    {
        QMutexLocker lock(&mutex_);
        sz = size_;
        disk = diskEnabled_;
        comp = compress_;
        dir = cacheDir_;
        gen = generation_;
    }
    ImageLoader& loader = ImageLoader::instance();
    ImageInfo info;
    QImage thumb;
    QString err;
    if (loader.readInfo(path, &info, &err)) {
        QString cachePath;
        if (disk) cachePath = diskCachePath(path);
        if (disk && QFile::exists(cachePath)) thumb.load(cachePath);
        if (thumb.isNull()) {
            err.clear();
            thumb = loader.loadScaledNow(path, sz, &err);
            if (thumb.isNull()) {
                if (err.isEmpty()) err = S("err.imageLoad").arg(QFileInfo(path).fileName()).arg(S("err.noDecoder").arg(path));
            } else if (disk) {
                QDir().mkpath(dir);
                if (!thumb.save(cachePath, "PNG", comp ? 20 : 100)) {
                    err = S("err.writeFile").arg(cachePath).arg(QStringLiteral("PNG"));
                }
            }
        }
    }
    QMetaObject::invokeMethod(this, [this, path, thumb, info, err, gen]() {
        int cur = 0;
        {
            QMutexLocker lock(&mutex_);
            cur = generation_;
        }
        if (gen != cur) {  // 已被取消:丢弃结果
            --running_;
            startNext();
            return;
        }
        onTaskDone(path, thumb, info, err);
    }, Qt::QueuedConnection);
}

// 磁盘缓存文件路径
QString ThumbnailCache::diskCachePath(const QString& path) const
{
    QSize sz;
    QString dir;
    {
        QMutexLocker lock(&mutex_);
        sz = size_;
        dir = cacheDir_;
    }
    const QFileInfo fi(path);
    const QString key = fi.absoluteFilePath() + QLatin1Char('|') + QString::number(fi.size()) + QLatin1Char('|')
                        + QString::number(fi.lastModified().toMSecsSinceEpoch()) + QLatin1Char('|')
                        + QString::number(sz.width()) + QLatin1Char('x') + QString::number(sz.height());
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex();
    return dir + QLatin1Char('/') + QString::fromLatin1(h) + QStringLiteral(".png");
}
