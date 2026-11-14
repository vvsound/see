#pragma once
#include <QWidget>
#include <QLabel>
#include <QImage>
#include <QString>
#include "ui/PreviewPane.h"

// 直方图:对预览图统计 R/G/B 和亮度,画成叠加的曲线
class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent);
    // 用这张图重新统计;空图清空
    void setImage(const QImage& image);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    int bins_[4][256];  // 0=R 1=G 2=B 3=亮度
    int peak_;
    bool has_;
};

// 浏览窗口右侧的信息面板:预览、基本信息、EXIF、直方图(各部分和字段由 [Info] 设置决定)
class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(QWidget* parent);
    // 显示某个文件的信息;空路径清空
    void showFile(const QString& path);
    // 清空
    void clear();
    // 重新读取 [Info] 设置
    void applyOptions();

private:
    // 生成基本信息 / EXIF 的 HTML 文字
    QString basicHtml(const QString& path) const;
    QString exifHtml(const QString& path) const;

    PreviewPane* preview_;
    QLabel* basic_;
    QLabel* exif_;
    QLabel* histTitle_;
    HistogramWidget* hist_;
    QString path_;
};
