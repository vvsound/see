#include "PreviewPane.h"
#include "core/Config.h"
#include "core/Strings.h"
#include "core/ImageLoader.h"

#include <QPainter>
#include <QPaintEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <QMetaObject>

// 构造
PreviewPane::PreviewPane(QWidget* parent) : QWidget(parent)
{
    autoPreview_ = true;
    delayMs_ = 200;
    delayTimer_ = new QTimer(this);
    delayTimer_->setSingleShot(true);
    connect(delayTimer_, &QTimer::timeout, this, &PreviewPane::onDelayTimeout);
    connect(this, &PreviewPane::previewDone, this, &PreviewPane::onPreviewReady);
    setMinimumHeight(60);
    setAutoFillBackground(true);
    applyOptions();
}

// 显示某个文件的预览(空路径清空);关闭\"预览自动\"时不更新
void PreviewPane::showFile(const QString& path)
{
    if (path.isEmpty() || !autoPreview_) {
        clear();
        return;
    }
    if (path == shownPath_ && error_.isEmpty() && !image_.isNull()) return;
    pendingPath_ = path;
    delayTimer_->start(delayMs_);
}

// 立即清空
void PreviewPane::clear()
{
    delayTimer_->stop();
    pendingPath_.clear();
    shownPath_.clear();
    image_ = QImage();
    error_.clear();
    emit imageChanged(image_);
    update();
}

// 重新读取配置
void PreviewPane::applyOptions()
{
    Config& c = Config::instance();
    autoPreview_ = c.getBool(QStringLiteral("FileList/PreviewAuto"));
    delayMs_ = c.getInt(QStringLiteral("FileList/PreviewDelayMs"));
    if (!autoPreview_) clear();
}

// 绘制:居中等比缩放;失败时画错误文字
void PreviewPane::paintEvent(QPaintEvent* e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.fillRect(rect(), palette().base());
    if (!error_.isEmpty()) {
        p.setPen(QColor(180, 40, 40));
        p.drawText(rect().adjusted(6, 6, -6, -6), Qt::AlignCenter | Qt::TextWordWrap, error_);
        return;
    }
    if (image_.isNull()) return;
    const QSize avail = size() - QSize(8, 8);
    QSize target = image_.size();
    if (target.width() > avail.width() || target.height() > avail.height()) {
        target = target.scaled(avail, Qt::KeepAspectRatio);
    }
    QRect r(QPoint(0, 0), target);
    r.moveCenter(rect().center());
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(r, image_);
}

// 延迟到期:后台生成预览
void PreviewPane::onDelayTimeout()
{
    const QString path = pendingPath_;
    if (path.isEmpty()) return;
    const QSize want(qMax(64, width()), qMax(64, height()));
    QtConcurrent::run([this, path, want]() {
        QString err;
        const QImage img = ImageLoader::instance().loadScaledNow(path, want, &err);
        QMetaObject::invokeMethod(this, [this, path, img, err]() { emit previewDone(path, img, err); },
                                  Qt::QueuedConnection);
    });
}

// 后台预览完成(只接受最新请求的结果)
void PreviewPane::onPreviewReady(const QString& path, const QImage& image, const QString& error)
{
    if (path != pendingPath_) return;
    shownPath_ = path;
    image_ = image;
    error_ = error;
    if (image.isNull() && error.isEmpty()) {
        error_ = S("err.imageLoad").arg(path).arg(S("err.noDecoder").arg(path));
    }
    emit imageChanged(image_);
    update();
}
