#pragma once
#include <QWidget>
#include <QImage>
#include <QString>
#include <QTimer>

// 浏览窗口左下的预览窗格
class PreviewPane : public QWidget {
    Q_OBJECT
public:
    // 构造
    explicit PreviewPane(QWidget* parent);
    // 显示某个文件的预览(按 FileList/PreviewDelayMs 延迟);空路径清空
    void showFile(const QString& path);
    // 立即清空
    void clear();
    // 重新读取配置(是否自动预览、延迟)
    void applyOptions();

protected:
    // 绘制(居中等比缩放;失败时画错误文字)
    void paintEvent(QPaintEvent* e) override;

private slots:
    // 延迟到期,开始后台生成预览
    void onDelayTimeout();
    // 后台预览完成
    void onPreviewReady(const QString& path, const QImage& image, const QString& error);

signals:
    // 内部:后台线程把结果送回主线程
    void previewDone(const QString& path, const QImage& image, const QString& error);

private:
    QImage image_;
    QString error_;
    QString pendingPath_;
    QString shownPath_;
    QTimer* delayTimer_;
    bool autoPreview_;
    int delayMs_;
};
