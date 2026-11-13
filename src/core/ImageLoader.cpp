#include "ImageLoader.h"
#include "Strings.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSvgRenderer>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QMetaObject>
#include <QMutexLocker>
#include <QtConcurrent/QtConcurrentRun>

// ---------------------------------------------------------------------------
// 公共小函数
// ---------------------------------------------------------------------------

// 色深文字:24/32 位 -> "16M",8 位 -> "256",其余 -> 位数
QString colorDepthText(int bits)
{
    if (bits == 24 || bits == 32) return QStringLiteral("16M");
    if (bits == 8) return QStringLiteral("256");
    return QString::number(bits);
}

// 一行信息文字:宽x高x色深(withFormat 为 true 时再加空格+格式名)
QString imageInfoLine(const ImageInfo& info, bool withFormat)
{
    QString s = QStringLiteral("%1x%2x%3")
                    .arg(info.size.width())
                    .arg(info.size.height())
                    .arg(colorDepthText(info.bits));
    if (withFormat && !info.format.isEmpty()) {
        s += QLatin1Char(' ') + info.format;
    }
    return s;
}

// 读整个文件;失败时在 error 写明原因
static bool readWholeFile(const QString& path, QByteArray* data, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = S("err.readFile").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }
    *data = f.readAll();
    return true;
}

// 把文件大小和修改时间填进 info
static void fillFileInfo(const QString& path, ImageInfo* info)
{
    QFileInfo fi(path);
    info->fileSize = fi.size();
    info->modified = fi.lastModified();
}

// ---------------------------------------------------------------------------
// QtImageDecoder
// ---------------------------------------------------------------------------

QString QtImageDecoder::name() const
{
    return QStringLiteral("Qt");
}

bool QtImageDecoder::canDecode(const QByteArray& head, const QString& path) const
{
    Q_UNUSED(head);
    QImageReader r(path);
    return r.canRead();
}

bool QtImageDecoder::decode(const QString& path, LoadResult& out) const
{
    QImageReader r(path);
    r.setAutoTransform(true);
    const QString fileName = QFileInfo(path).fileName();
    QByteArray fmt = r.format();
    bool animated = false;
    if (r.supportsAnimation()) {
        animated = r.imageCount() > 1;
    }
    QImage img = r.read();
    if (img.isNull()) {
        out.error = S("err.imageLoad").arg(fileName, r.errorString());
        return false;
    }
    out.image = img;
    out.isSvg = false;
    out.isAnimated = animated;
    out.info.size = img.size();
    out.info.bits = img.depth();
    out.info.format = QString::fromLatin1(fmt).toLower();
    fillFileInfo(path, &out.info);
    out.info.valid = true;
    return true;
}

bool QtImageDecoder::readInfo(const QString& path, ImageInfo* info, QString* error) const
{
    QImageReader r(path);
    r.setAutoTransform(true);
    QSize sz = r.size();
    if (!sz.isValid()) {
        if (error) *error = S("err.imageLoad").arg(QFileInfo(path).fileName(), r.errorString());
        return false;
    }
    info->size = sz;
    QPixelFormat pf = QImage::toPixelFormat(r.imageFormat());
    info->bits = pf.bitsPerPixel();
    info->format = QString::fromLatin1(r.format()).toLower();
    fillFileInfo(path, info);
    info->valid = true;
    return true;
}

QImage QtImageDecoder::decodeScaled(const QString& path, const QSize& maxSize, QString* error) const
{
    QImageReader r(path);
    r.setAutoTransform(true);
    QSize sz = r.size();
    if (sz.isValid() && (sz.width() > maxSize.width() || sz.height() > maxSize.height())) {
        r.setScaledSize(sz.scaled(maxSize, Qt::KeepAspectRatio));
    }
    QImage img = r.read();
    if (img.isNull()) {
        if (error) *error = S("err.imageLoad").arg(QFileInfo(path).fileName(), r.errorString());
        return QImage();
    }
    if (img.width() > maxSize.width() || img.height() > maxSize.height()) {
        img = img.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return img;
}

// ---------------------------------------------------------------------------
// SvgImageDecoder
// ---------------------------------------------------------------------------

// 取 SVG 默认尺寸:先用 defaultSize,没有就用 viewBox;都没有返回无效尺寸
static QSize svgDefaultSize(const QSvgRenderer& r)
{
    QSize s = r.defaultSize();
    if (s.isValid() && s.width() > 0 && s.height() > 0) return s;
    QSizeF vb = r.viewBoxF().size();
    if (vb.width() > 0 && vb.height() > 0) return vb.toSize();
    return QSize();
}

QString SvgImageDecoder::name() const
{
    return QStringLiteral("SVG");
}

bool SvgImageDecoder::canDecode(const QByteArray& head, const QString& path) const
{
    if (head.contains("<svg")) return true;
    // gzip 压缩的 svgz:魔数 1F 8B,并且确实能解析成 SVG
    if (head.size() >= 2 && static_cast<unsigned char>(head[0]) == 0x1F
        && static_cast<unsigned char>(head[1]) == 0x8B) {
        QByteArray data;
        QString err;
        if (!readWholeFile(path, &data, &err)) return false;
        QSvgRenderer r(data);
        return r.isValid();
    }
    return false;
}

bool SvgImageDecoder::decode(const QString& path, LoadResult& out) const
{
    QByteArray data;
    QString err;
    if (!readWholeFile(path, &data, &err)) {
        out.error = err;
        return false;
    }
    QSvgRenderer r(data);
    const QString fileName = QFileInfo(path).fileName();
    if (!r.isValid()) {
        out.error = S("err.svgInvalid").arg(fileName);
        return false;
    }
    QSize sz = svgDefaultSize(r);
    if (!sz.isValid()) {
        out.error = S("err.svgInvalid").arg(fileName);
        return false;
    }
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    r.render(&p);
    p.end();
    out.image = img;
    out.isSvg = true;
    out.svgData = data;
    out.isAnimated = false;
    out.info.size = sz;
    out.info.bits = 32;
    out.info.format = QStringLiteral("svg");
    fillFileInfo(path, &out.info);
    out.info.valid = true;
    return true;
}

bool SvgImageDecoder::readInfo(const QString& path, ImageInfo* info, QString* error) const
{
    QByteArray data;
    if (!readWholeFile(path, &data, error)) return false;
    QSvgRenderer r(data);
    QSize sz = r.isValid() ? svgDefaultSize(r) : QSize();
    if (!sz.isValid()) {
        if (error) *error = S("err.svgInvalid").arg(QFileInfo(path).fileName());
        return false;
    }
    info->size = sz;
    info->bits = 32;
    info->format = QStringLiteral("svg");
    fillFileInfo(path, info);
    info->valid = true;
    return true;
}

QImage SvgImageDecoder::decodeScaled(const QString& path, const QSize& maxSize, QString* error) const
{
    QByteArray data;
    if (!readWholeFile(path, &data, error)) return QImage();
    QSvgRenderer r(data);
    QSize sz = r.isValid() ? svgDefaultSize(r) : QSize();
    if (!sz.isValid()) {
        if (error) *error = S("err.svgInvalid").arg(QFileInfo(path).fileName());
        return QImage();
    }
    QSize target = sz.scaled(maxSize, Qt::KeepAspectRatio);
    if (target.width() < 1 || target.height() < 1) target = QSize(1, 1);
    QImage img(target, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    r.render(&p);
    p.end();
    return img;
}

// ---------------------------------------------------------------------------
// ImageLoader
// ---------------------------------------------------------------------------

ImageLoader& ImageLoader::instance()
{
    static ImageLoader inst;
    return inst;
}

ImageLoader::ImageLoader() : QObject(nullptr), cacheLimit_(5), maxRunning_(2)
{
    qRegisterMetaType<LoadResult>("LoadResult");
}

ImageLoader::~ImageLoader()
{
    QThreadPool::globalInstance()->waitForDone();
    qDeleteAll(decoders_);
    decoders_.clear();
}

// 注册解码器(所有权归 ImageLoader);先注册的先尝试
void ImageLoader::registerDecoder(IImageDecoder* decoder)
{
    QMutexLocker lock(&mutex_);
    decoders_.append(decoder);
}

// 注册内置的两个解码器:SVG 先于 Qt 通用
void ImageLoader::registerDefaults()
{
    {
        QMutexLocker lock(&mutex_);
        if (!decoders_.isEmpty()) return;
    }
    registerDecoder(new SvgImageDecoder());
    registerDecoder(new QtImageDecoder());
}

// 找到能解码此文件的解码器:按文件开头内容判断,不看扩展名
IImageDecoder* ImageLoader::findDecoder(const QString& path) const
{
    QByteArray head;
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return nullptr;
        head = f.read(16384);
    }
    for (int i = 0; i < decoders_.size(); ++i) {
        if (decoders_.at(i)->canDecode(head, path)) return decoders_.at(i);
    }
    return nullptr;
}

// 同步载入(在调用线程中解码),不查缓存
LoadResult ImageLoader::loadNow(const QString& path) const
{
    QElapsedTimer timer;
    timer.start();
    LoadResult r;
    r.path = path;
    const QString fileName = QFileInfo(path).fileName();

    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        r.error = S("err.readFile").arg(fileName, probe.errorString());
        r.seconds = timer.elapsed() / 1000.0;
        return r;
    }
    probe.close();

    IImageDecoder* dec = findDecoder(path);
    if (!dec) {
        r.error = S("err.noDecoder").arg(fileName);
        r.seconds = timer.elapsed() / 1000.0;
        return r;
    }
    r.ok = dec->decode(path, r);
    if (!r.ok && r.error.isEmpty()) {
        r.error = S("err.imageLoad").arg(fileName, dec->name());
    }
    r.seconds = timer.elapsed() / 1000.0;
    return r;
}

// 异步载入;priority 越大越先
void ImageLoader::request(const QString& path, int priority)
{
    LoadResult cached;
    if (getCached(path, &cached)) {
        QMetaObject::invokeMethod(this, [this, cached]() { emit loaded(cached); },
                                  Qt::QueuedConnection);
        return;
    }
    {
        QMutexLocker lock(&mutex_);
        if (running_.contains(path)) return;  // 正在载入,完成后会发 loaded
        if (pending_.contains(path)) {
            if (pending_.value(path) < priority) pending_[path] = priority;
        } else {
            pending_.insert(path, priority);
        }
    }
    startNext();
}

// 从缓存取:文件大小和修改时间必须没变
bool ImageLoader::getCached(const QString& path, LoadResult* out) const
{
    QMutexLocker lock(&mutex_);
    for (int i = cache_.size() - 1; i >= 0; --i) {
        const LoadResult& r = cache_.at(i);
        if (r.path != path) continue;
        QFileInfo fi(path);
        if (fi.size() == r.info.fileSize && fi.lastModified() == r.info.modified) {
            if (out) *out = r;
            return true;
        }
        return false;
    }
    return false;
}

// 预加载一批文件(低优先级)
void ImageLoader::prefetch(const QStringList& paths)
{
    for (int i = 0; i < paths.size(); ++i) {
        const QString& p = paths.at(i);
        if (getCached(p, nullptr)) continue;
        QMutexLocker lock(&mutex_);
        if (running_.contains(p) || pending_.contains(p)) continue;
        pending_.insert(p, 1);
    }
    startNext();
}

// 取消不在 keep 中的等待中的任务
void ImageLoader::cancelPending(const QStringList& keep)
{
    QMutexLocker lock(&mutex_);
    QMap<QString, int>::iterator it = pending_.begin();
    while (it != pending_.end()) {
        if (!keep.contains(it.key())) it = pending_.erase(it);
        else ++it;
    }
}

// 清空缓存
void ImageLoader::clearCache()
{
    QMutexLocker lock(&mutex_);
    cache_.clear();
}

// 只读基本信息(同步)
bool ImageLoader::readInfo(const QString& path, ImageInfo* info, QString* error) const
{
    IImageDecoder* dec = findDecoder(path);
    if (!dec) {
        if (error) *error = S("err.noDecoder").arg(QFileInfo(path).fileName());
        return false;
    }
    return dec->readInfo(path, info, error);
}

// 同步解码缩略图(线程安全)
QImage ImageLoader::loadScaledNow(const QString& path, const QSize& maxSize, QString* error) const
{
    IImageDecoder* dec = findDecoder(path);
    if (!dec) {
        if (error) *error = S("err.noDecoder").arg(QFileInfo(path).fileName());
        return QImage();
    }
    return dec->decodeScaled(path, maxSize, error);
}

// 按内容判断文件是否为已注册解码器能识别的图片
bool ImageLoader::canDecodeFile(const QString& path) const
{
    return findDecoder(path) != nullptr;
}

// 缓存的最大张数
void ImageLoader::setCacheLimit(int count)
{
    QMutexLocker lock(&mutex_);
    cacheLimit_ = count < 1 ? 1 : count;
    while (cache_.size() > cacheLimit_) cache_.removeFirst();
}

// 启动等待中的任务(最多 maxRunning_ 个同时进行,优先级高的先)
void ImageLoader::startNext()
{
    QStringList toStart;
    {
        QMutexLocker lock(&mutex_);
        while (running_.size() + toStart.size() < maxRunning_ && !pending_.isEmpty()) {
            QString best;
            int bestPri = -1;
            QMap<QString, int>::const_iterator it = pending_.constBegin();
            for (; it != pending_.constEnd(); ++it) {
                if (it.value() > bestPri) { bestPri = it.value(); best = it.key(); }
            }
            pending_.remove(best);
            running_.insert(best);
            toStart.append(best);
        }
    }
    for (int i = 0; i < toStart.size(); ++i) {
        const QString p = toStart.at(i);
        QtConcurrent::run([this, p]() { runTask(p); });
    }
}

// 后台线程里执行的载入,完成后回到主线程
void ImageLoader::runTask(const QString& path)
{
    LoadResult r = loadNow(path);
    QMetaObject::invokeMethod(this, [this, r]() { onTaskFinished(r); }, Qt::QueuedConnection);
}

// 后台任务完成(主线程)
void ImageLoader::onTaskFinished(const LoadResult& result)
{
    {
        QMutexLocker lock(&mutex_);
        running_.remove(result.path);
    }
    if (result.ok) putCache(result);
    emit loaded(result);
    startNext();
}

// 放入缓存(同一路径只留最新的一份)
void ImageLoader::putCache(const LoadResult& r)
{
    QMutexLocker lock(&mutex_);
    for (int i = cache_.size() - 1; i >= 0; --i) {
        if (cache_.at(i).path == r.path) cache_.removeAt(i);
    }
    cache_.append(r);
    while (cache_.size() > cacheLimit_) cache_.removeFirst();
}
