#pragma once
#include <QObject>
#include <QString>
#include <QImage>
#include <QSize>
#include <QMap>
#include <QList>
#include <QSet>
#include <QMutex>
#include "ImageLoader.h"

// 缩略图异步生成与缓存(内存 + 可选磁盘缓存)
class ThumbnailCache : public QObject {
    Q_OBJECT
public:
    // 全局唯一实例
    static ThumbnailCache& instance();
    // 从配置读取缩略图尺寸、缓存开关等
    void configure();
    // 缩略图框尺寸(Thumbnails/Width x Height)
    QSize thumbSize() const;
    // 内存中已有则返回(null 图表示还没有)
    QImage cached(const QString& path) const;
    // 内存中的图片信息(宽高色深)
    ImageInfo cachedInfo(const QString& path) const;
    // 请求异步生成缩略图和信息;完成后发 ready 或 failed
    void request(const QString& path);
    // 取消还没开始的请求
    void cancelAll();
    // 清空内存缓存
    void clearMemory();

signals:
    // 缩略图和信息已生成
    void ready(const QString& path);
    // 生成失败(error 含文件名和原因)
    void failed(const QString& path, const QString& error);

private slots:
    // 后台任务完成
    void onTaskDone(const QString& path, const QImage& thumb, const ImageInfo& info, const QString& error);

private:
    ThumbnailCache();
    // 后台线程里执行
    void runTask(const QString& path);
    // 磁盘缓存文件路径(由路径、大小、修改时间生成)
    QString diskCachePath(const QString& path) const;
    // 从等待队列启动后台任务(最多同时 3 个,后请求的先做)
    void startNext();

    QMap<QString, QImage> images_;
    QMap<QString, ImageInfo> infos_;
    QSet<QString> requested_;
    QList<QString> queue_;
    QList<QString> order_;     // 已缓存路径的先后顺序(内存超限时淘汰最早的)
    int running_;
    int generation_;
    QSize size_;
    bool diskEnabled_;
    bool compress_;
    QString cacheDir_;
    mutable QMutex mutex_;
};
