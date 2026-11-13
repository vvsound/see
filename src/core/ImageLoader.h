#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QImage>
#include <QSize>
#include <QDateTime>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QSet>

// 图片的基本信息(不需要完整解码即可得到)
struct ImageInfo {
    QSize size;          // 宽高(SVG 为其默认尺寸)
    int bits;            // 色深(位)
    QString format;      // 小写格式名,如 jpeg png svg
    qint64 fileSize;     // 文件大小(字节)
    QDateTime modified;  // 修改时间
    bool valid;          // 信息是否有效
    ImageInfo() : bits(0), fileSize(0), valid(false) {}
};

// 一次载入的结果
struct LoadResult {
    QString path;        // 文件完整路径
    bool ok;             // 是否成功
    QString error;       // 失败原因(含文件名),ok==false 时有效
    QImage image;        // 位图(非 SVG 时有效;动画 GIF 仅第一帧)
    bool isSvg;          // 是否矢量图
    QByteArray svgData;  // SVG 原始数据(isSvg 时有效,查看窗口据此重新矢量渲染)
    bool isAnimated;     // 是否为动画(GIF/WebP 动画)
    ImageInfo info;      // 基本信息
    double seconds;      // 载入耗时(秒)
    LoadResult() : ok(false), isSvg(false), isAnimated(false), seconds(0.0) {}
};

// 色深文字:24/32 位 -> "16M",8 位 -> "256",其余 -> 位数
QString colorDepthText(int bits);
// 一行信息文字:宽x高x色深(withFormat 为 true 时再加空格+格式名)
QString imageInfoLine(const ImageInfo& info, bool withFormat);

// 图片解码器接口:以后加新格式 = 加一个解码器类并注册
class IImageDecoder {
public:
    virtual ~IImageDecoder() {}
    // 解码器名称
    virtual QString name() const = 0;
    // 根据文件开头内容判断能否解码(不依赖扩展名)
    virtual bool canDecode(const QByteArray& head, const QString& path) const = 0;
    // 完整解码;失败时在 out.error 写明原因并返回 false
    virtual bool decode(const QString& path, LoadResult& out) const = 0;
    // 只读基本信息;失败时在 error 写明原因
    virtual bool readInfo(const QString& path, ImageInfo* info, QString* error) const = 0;
    // 解码为不超过 maxSize 的缩略图(用于缩略图和预览);失败时写 error
    virtual QImage decodeScaled(const QString& path, const QSize& maxSize, QString* error) const = 0;
};

// Qt 通用解码器(jpeg/png/gif/bmp/webp/tiff/ico 等 Qt 插件支持的格式)
class QtImageDecoder : public IImageDecoder {
public:
    // 解码器名称
    QString name() const override;
    // 交给 QImageReader 按内容判断
    bool canDecode(const QByteArray& head, const QString& path) const override;
    // 完整解码
    bool decode(const QString& path, LoadResult& out) const override;
    // 读基本信息
    bool readInfo(const QString& path, ImageInfo* info, QString* error) const override;
    // 缩略解码
    QImage decodeScaled(const QString& path, const QSize& maxSize, QString* error) const override;
};

// SVG 解码器(svg/svgz,按内容识别)
class SvgImageDecoder : public IImageDecoder {
public:
    // 解码器名称
    QString name() const override;
    // 开头含 <svg 或 gzip 且能解析为 SVG
    bool canDecode(const QByteArray& head, const QString& path) const override;
    // 读入 SVG 数据并渲染一张默认尺寸的位图
    bool decode(const QString& path, LoadResult& out) const override;
    // 读默认尺寸
    bool readInfo(const QString& path, ImageInfo* info, QString* error) const override;
    // 渲染缩略图
    QImage decodeScaled(const QString& path, const QSize& maxSize, QString* error) const override;
};

// 图片载入器:管理解码器、后台载入、预加载、缓存
class ImageLoader : public QObject {
    Q_OBJECT
public:
    // 全局唯一实例
    static ImageLoader& instance();
    // 注册解码器(所有权归 ImageLoader);先注册的先尝试,SVG 要先于 Qt 通用
    void registerDecoder(IImageDecoder* decoder);
    // 注册内置的两个解码器(SVG、Qt 通用)
    void registerDefaults();
    // 同步载入(在调用线程中解码),不查缓存
    LoadResult loadNow(const QString& path) const;
    // 异步载入;priority 越大越先(当前图用 10,预加载用 1);完成后发 loaded
    void request(const QString& path, int priority);
    // 从缓存取(文件大小和修改时间必须没变);命中返回 true
    bool getCached(const QString& path, LoadResult* out) const;
    // 预加载一批文件(低优先级)
    void prefetch(const QStringList& paths);
    // 取消不在 keep 中的等待中的任务
    void cancelPending(const QStringList& keep);
    // 清空缓存
    void clearCache();
    // 只读基本信息(同步)
    bool readInfo(const QString& path, ImageInfo* info, QString* error) const;
    // 同步解码缩略图(线程安全,可在后台线程调用)
    QImage loadScaledNow(const QString& path, const QSize& maxSize, QString* error) const;
    // 按内容判断文件是否为已注册解码器能识别的图片
    bool canDecodeFile(const QString& path) const;
    // 缓存的最大张数
    void setCacheLimit(int count);

signals:
    // 一张图载入完成(成功或失败都发,失败时 result.ok==false)
    void loaded(const LoadResult& result);

private slots:
    // 后台任务完成,回到主线程处理
    void onTaskFinished(const LoadResult& result);

private:
    ImageLoader();
    ~ImageLoader();
    // 找到能解码此文件的解码器(没有返回 nullptr)
    IImageDecoder* findDecoder(const QString& path) const;
    // 后台线程里执行的载入
    void runTask(const QString& path);
    // 启动下一个等待中的任务
    void startNext();
    // 放入缓存
    void putCache(const LoadResult& r);

    QList<IImageDecoder*> decoders_;
    mutable QMutex mutex_;
    QList<LoadResult> cache_;                 // 最近载入的结果(新的在后)
    int cacheLimit_;
    QMap<QString, int> pending_;              // 等待中: 路径 -> 优先级
    QSet<QString> running_;                   // 正在载入的路径
    int maxRunning_;                          // 最大并发后台任务数
};
