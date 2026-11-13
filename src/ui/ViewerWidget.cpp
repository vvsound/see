#include "ViewerWidget.h"
#include "core/Config.h"

#include <QPainter>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <cmath>

// 对位图做 γ 校正(gamma>1 变亮);TODO-CONFIRM:ACDSee 里 γ 数值的确切含义
static QImage applyGammaToImage(const QImage& src, double gamma)
{
    if (src.isNull() || gamma <= 0.0) return src;
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    uchar lut[256];
    for (int i = 0; i < 256; ++i) {
        int v = qRound(255.0 * std::pow(i / 255.0, 1.0 / gamma));
        lut[i] = static_cast<uchar>(qBound(0, v, 255));
    }
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            QRgb c = line[x];
            line[x] = qRgba(lut[qRed(c)], lut[qGreen(c)], lut[qBlue(c)], qAlpha(c));
        }
    }
    return img;
}

// 构造
ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent)
{
    svg_ = nullptr;
    movie_ = nullptr;
    zoom_ = 1.0;
    rotation_ = 0;
    offset_ = QPointF(0, 0);
    dragging_ = false;
    gammaEnabled_ = false;
    gamma_ = 1.0;
    disableAnimation_ = false;
    shrinkToWindow_ = false;
    enlargeToWindow_ = false;
    shrinkToScreen_ = false;
    enlargeToScreen_ = false;
    cursorHidden_ = false;
    zoomLock_ = false;
    fitAuto_ = false;
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setContextMenuPolicy(Qt::NoContextMenu);
    applyOptions();
}

// 显示载入结果:成功显示图;失败显示含文件名和原因的错误文字
void ViewerWidget::showResult(const LoadResult& result)
{
    const double keepZoom = zoom_;
    clear();
    if (zoomLock_) zoom_ = keepZoom;
    path_ = result.path;
    if (!result.ok) {
        errorText_ = result.error;
        update();
        return;
    }
    baseSize_ = result.info.size;
    if (result.isSvg) {
        svgData_ = result.svgData;
        svg_ = new QSvgRenderer(svgData_, this);
    } else {
        image_ = result.image;
        baseSize_ = image_.size();
        gammaImage_ = gammaEnabled_ ? applyGammaToImage(image_, gamma_) : QImage();
        if (result.isAnimated && !disableAnimation_) {
            movie_ = new QMovie(result.path, QByteArray(), this);
            if (movie_->isValid()) {
                connect(movie_, &QMovie::frameChanged, this, &ViewerWidget::onMovieFrame);
                movie_->start();
            } else {
                delete movie_;  // 动画读不了时仍显示已载入的第一帧
                movie_ = nullptr;
            }
        }
    }
    rotation_ = 0;
    offset_ = QPointF(0, 0);
    applyInitialZoom();
    update();
}

// 清空显示
void ViewerWidget::clear()
{
    if (movie_) { movie_->stop(); delete movie_; movie_ = nullptr; }
    if (svg_) { delete svg_; svg_ = nullptr; }
    image_ = QImage();
    gammaImage_ = QImage();
    svgData_.clear();
    errorText_.clear();
    baseSize_ = QSize();
    path_.clear();
    dragging_ = false;
    update();
}

// 是否有图片在显示
bool ViewerWidget::hasImage() const
{
    return svg_ != nullptr || !image_.isNull();
}

// 当前缩放比例
double ViewerWidget::zoom() const
{
    return zoom_;
}

// 设置缩放;anchor 为视图坐标中的缩放中心
void ViewerWidget::setZoom(double z, const QPointF& anchor, bool useAnchor)
{
    if (!hasImage()) return;
    if (z < 0.01) z = 0.01;
    if (z > 64.0) z = 64.0;
    const QPointF c(width() / 2.0, height() / 2.0);
    const QPointF a = useAnchor ? anchor : c;
    const QPointF p = (a - (c + offset_)) / zoom_;  // 锚点下的图片内位置(相对图片中心)
    offset_ = a - c - p * z;
    zoom_ = z;
    fitAuto_ = false;
    clampOffset();
    zoomChangedInternal();
    update();
}

// 放大一档(以窗口中心为中心);TODO-CONFIRM:每档倍率
void ViewerWidget::zoomIn()
{
    setZoom(zoom_ * 1.25, QPointF(), false);
}

// 缩小一档
void ViewerWidget::zoomOut()
{
    setZoom(zoom_ / 1.25, QPointF(), false);
}

// 适应窗口(大图缩小、小图不放大,除非选项要求放大)
void ViewerWidget::fitToWindow()
{
    if (!hasImage()) return;
    QSize d = displaySizeAt100();
    if (d.width() <= 0 || d.height() <= 0) return;
    double z = qMin(width() / double(d.width()), height() / double(d.height()));
    if (z > 1.0 && !enlargeToWindow_) z = 1.0;
    zoom_ = z;
    fitAuto_ = false;
    offset_ = QPointF(0, 0);
    zoomChangedInternal();
    update();
}

// 原始大小 100%
void ViewerWidget::actualSize()
{
    if (!hasImage()) return;
    zoom_ = 1.0;
    fitAuto_ = false;
    offset_ = QPointF(0, 0);
    zoomChangedInternal();
    update();
}

// 向左转 90 度(只改显示)
void ViewerWidget::rotateLeft()
{
    if (!hasImage()) return;
    rotation_ = (rotation_ + 270) % 360;
    offset_ = QPointF(0, 0);
    applyInitialZoom();
    update();
}

// 向右转 90 度
void ViewerWidget::rotateRight()
{
    if (!hasImage()) return;
    rotation_ = (rotation_ + 90) % 360;
    offset_ = QPointF(0, 0);
    applyInitialZoom();
    update();
}

// 当前旋转角度
int ViewerWidget::rotation() const
{
    return rotation_;
}

// 旋转后图片在 100% 时的像素尺寸
QSize ViewerWidget::displaySizeAt100() const
{
    if (!hasImage() || !baseSize_.isValid()) return QSize();
    if (rotation_ == 90 || rotation_ == 270) return QSize(baseSize_.height(), baseSize_.width());
    return baseSize_;
}

// 重新读取配置
void ViewerWidget::applyOptions()
{
    Config& cfg = Config::instance();
    shrinkToWindow_ = cfg.getBool(QStringLiteral("Viewer/ShrinkToWindow"));
    shrinkToScreen_ = cfg.getBool(QStringLiteral("Viewer/ShrinkToScreen"));
    enlargeToWindow_ = cfg.getBool(QStringLiteral("Viewer/EnlargeToWindow"));
    enlargeToScreen_ = cfg.getBool(QStringLiteral("Viewer/EnlargeToScreen"));
    gammaEnabled_ = cfg.getBool(QStringLiteral("Display/GammaEnabled"));
    gamma_ = cfg.getDouble(QStringLiteral("Display/Gamma"));
    disableAnimation_ = cfg.getBool(QStringLiteral("Display/DisableGifAnimation"));
    gammaImage_ = (gammaEnabled_ && !image_.isNull()) ? applyGammaToImage(image_, gamma_) : QImage();
    // 动画开关:已显示的动画图按新选项立即生效
    if (disableAnimation_ && movie_) {
        movie_->stop();
        delete movie_;
        movie_ = nullptr;
    }
    update();
}

// 按选项计算并应用初始缩放;TODO-CONFIRM:Screen 与 Window 两组选项的区分
void ViewerWidget::applyInitialZoom()
{
    if (!hasImage()) return;
    if (zoomLock_) {  // 缩放锁定:保持当前比例,只把图片放回中心
        offset_ = QPointF(0, 0);
        zoomChangedInternal();
        update();
        return;
    }
    QSize d = displaySizeAt100();
    if (d.width() <= 0 || d.height() <= 0) return;
    double z = 1.0;
    const double zw = qMin(width() / double(d.width()), height() / double(d.height()));
    double zs = zw;
    QScreen* scr = screen();
    if (scr) {
        QSize ss = scr->availableGeometry().size();
        zs = qMin(ss.width() / double(d.width()), ss.height() / double(d.height()));
    }
    if (shrinkToWindow_ && zw < 1.0) z = qMin(z, zw);
    if (shrinkToScreen_ && zs < 1.0) z = qMin(z, zs);
    if (enlargeToWindow_ && zw > 1.0) z = qMax(z, zw);
    if (enlargeToScreen_ && zs > 1.0) z = qMax(z, zs);
    zoom_ = z;
    fitAuto_ = true;
    offset_ = QPointF(0, 0);
    zoomChangedInternal();
    update();
}

// 缩放锁定开关
void ViewerWidget::setZoomLock(bool on)
{
    zoomLock_ = on;
}

// 隐藏/显示鼠标指针(全屏用)
void ViewerWidget::setCursorHidden(bool hidden)
{
    cursorHidden_ = hidden;
    if (hidden) setCursor(Qt::BlankCursor);
    else unsetCursor();
}

// 绘制
void ViewerWidget::paintEvent(QPaintEvent* e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.fillRect(rect(), QColor(0x20, 0x20, 0x20));  // TODO-CONFIRM:背景色
    if (!errorText_.isEmpty()) {
        p.setPen(QColor(0xFF, 0xA0, 0xA0));
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, errorText_);
        return;
    }
    if (!hasImage()) return;

    const QPointF c = imageRect().center();
    const QSizeF s(baseSize_.width() * zoom_, baseSize_.height() * zoom_);
    p.translate(c);
    p.rotate(rotation_);
    const QRectF target(-s.width() / 2.0, -s.height() / 2.0, s.width(), s.height());
    if (svg_) {
        p.setRenderHint(QPainter::Antialiasing, true);
        svg_->render(&p, target);  // 按当前缩放重新矢量渲染,放大不模糊
    } else {
        p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
        p.drawImage(target, currentFrame());
    }
}

// 滚轮:按 [Mouse] 配置执行;缩放命令以光标位置为中心
void ViewerWidget::wheelEvent(QWheelEvent* e)
{
    const int dy = e->angleDelta().y();
    if (dy == 0) { e->ignore(); return; }
    const bool ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
    const QString key = QString::fromLatin1(ctrl ? "Mouse/CtrlWheel." : "Mouse/Wheel.")
                        + QString::fromLatin1(dy > 0 ? "Up" : "Down");
    const QString v = Config::instance().getString(key);
    if (v == QLatin1String("ZoomIn")) {
        setZoom(zoom_ * 1.25, e->position(), true);
    } else if (v == QLatin1String("ZoomOut")) {
        setZoom(zoom_ / 1.25, e->position(), true);
    } else {
        dispatchMouse(key);
    }
    e->accept();
}

// 鼠标按下
void ViewerWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    const QString v = Config::instance().getString(QStringLiteral("Mouse/LeftDrag"));
    if (v == QLatin1String("Pan")) {
        dragging_ = true;
        lastMouse_ = e->pos();
        setCursor(Qt::ClosedHandCursor);
    } else {
        dispatchMouse(QStringLiteral("Mouse/LeftDrag"));
    }
    e->accept();
}

// 鼠标移动
void ViewerWidget::mouseMoveEvent(QMouseEvent* e)
{
    emit mouseMoved();
    if (dragging_) {
        const QPoint d = e->pos() - lastMouse_;
        lastMouse_ = e->pos();
        offset_ += QPointF(d);
        clampOffset();
        update();
    }
}

// 鼠标松开:左键结束拖动;右键触发 RightClick 映射
void ViewerWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        if (cursorHidden_) setCursor(Qt::BlankCursor);
        else unsetCursor();
    } else if (e->button() == Qt::RightButton) {
        dispatchMouse(QStringLiteral("Mouse/RightClick"));
    }
    e->accept();
}

// 双击
void ViewerWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        dispatchMouse(QStringLiteral("Mouse/DoubleClick"));
    }
    e->accept();
}

// 窗口大小变化
void ViewerWidget::resizeEvent(QResizeEvent* e)
{
    Q_UNUSED(e);
    if (fitAuto_ && hasImage() && !zoomLock_) {
        applyInitialZoom();  // 窗口大小变了:按选项重新适应
        return;
    }
    clampOffset();
    update();
}

// 动画换帧
void ViewerWidget::onMovieFrame()
{
    update();
}

// 图片(旋转后)当前缩放下的显示矩形
QRectF ViewerWidget::imageRect() const
{
    const QSize d = displaySizeAt100();
    const QSizeF s(d.width() * zoom_, d.height() * zoom_);
    const QPointF c = QPointF(width() / 2.0, height() / 2.0) + offset_;
    return QRectF(c.x() - s.width() / 2.0, c.y() - s.height() / 2.0, s.width(), s.height());
}

// 限制偏移量:图片比视图小的方向居中,大的方向不让边缘进入视野
void ViewerWidget::clampOffset()
{
    const QSize d = displaySizeAt100();
    const double iw = d.width() * zoom_;
    const double ih = d.height() * zoom_;
    double maxX = (iw - width()) / 2.0;
    double maxY = (ih - height()) / 2.0;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    offset_.setX(qBound(-maxX, offset_.x(), maxX));
    offset_.setY(qBound(-maxY, offset_.y(), maxY));
}

// 读取 [Mouse] 里的映射并发出:命令 ID -> commandRequested;ContextMenu -> contextMenuRequested;
// Pan 和 None 在这里不做事(Pan 由按下事件处理)
void ViewerWidget::dispatchMouse(const QString& configKey)
{
    const QString v = Config::instance().getString(configKey);
    if (v == QLatin1String("None") || v == QLatin1String("Pan")) return;
    if (v == QLatin1String("ContextMenu")) {
        emit contextMenuRequested(QCursor::pos());
        return;
    }
    emit commandRequested(v);
}

// 当前显示用的位图(已应用 γ 校正)
QImage ViewerWidget::currentFrame() const
{
    if (movie_) {
        QImage f = movie_->currentImage();
        return gammaEnabled_ ? applyGammaToImage(f, gamma_) : f;
    }
    return gammaEnabled_ ? gammaImage_ : image_;
}

// 保存当前缩放并通知
void ViewerWidget::zoomChangedInternal()
{
    emit zoomChanged(zoom_);
}
