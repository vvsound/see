#pragma once
#include <QString>
#include <QMap>

// 读取结果的状态
enum class ExifStatus {
    Ok,           // 读到了 EXIF
    NoExif,       // 文件是 JPEG,但里面没有 EXIF 段
    Unsupported,  // 不是 JPEG(目前只读 JPEG 的 EXIF)
    Error         // 文件读不了或 EXIF 结构损坏,message 里写明原因
};

// EXIF 读取结果;values 的键:Make Model LensModel ExposureTime FNumber ISO FocalLength
// ExposureBias DateTimeOriginal Software Orientation(值已是可直接显示的文字;文件里没有的键不出现)
struct ExifResult {
    ExifStatus status = ExifStatus::Error;
    QString message;
    QMap<QString, QString> values;
};

// 读取 JPEG 的 EXIF。只读文件开头的标记段,不解码图像。
ExifResult readExif(const QString& path);
