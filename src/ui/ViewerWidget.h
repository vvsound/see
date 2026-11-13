#pragma once
#include <QWidget>
#include <QImage>
#include <QPointF>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QMovie>
#include <QSvgRenderer>
#include "core/ImageLoader.h"

// 查看窗口的图片显示区:缩放、拖动、旋转、SVG 重新矢量渲染、错误文字
class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    // 构造
    explicit ViewerWidget(QWidget* parent);
    // 显示载入结果(成功显示图;失败显示含文件名和原因的错误文字)
    void showResult(const LoadResult& result);
    // 清空显示
    void clear();
    // 是否有图片在显示
    bool hasImage() const;
    // 当前缩放比例(1.0 = 100%)
    double zoom() const;
    // 设置缩放;anchor 为视图坐标中的缩放中心(0,0 表示用窗口中心)
    void setZoom(double z, const QPointF& anchor, bool useAnchor);
    // 放大一档 / 缩小一档(以窗口中心为中心)
    void zoomIn();
    void zoomOut();
    // 适应窗口(大图缩小、小图不放大,除非选项要求放大)
    void fitToWindow();
    // 原始大小 100%
    void actualSize();
    // 向左/向右转 90 度(只改显示,不改文件)
    void rotateLeft();
    void rotateRight();
    // 当前旋转角度 0/90/180/270
    int rotation() const;
    // 旋转后图片在 100% 时的像素尺寸(窗口自动调整用),无图时为空
    QSize displaySizeAt100() const;
    // 重新读取配置(缩小/放大适应、γ、GIF 动画)
    void applyOptions();
    // 按选项(缩小/放大适应窗口或屏幕)计算并应用初始缩放
    void applyInitialZoom();
    // 缩放锁定:开启后换图不重算缩放,保持当前比例
    void setZoomLock(bool on);
    // 隐藏/显示鼠标指针(全屏用)
    void setCursorHidden(bool hidden);

signals:
    // 缩放比例变了
    void zoomChanged(double zoom);
    // 鼠标动作映射到命令,请求执行(命令 ID 来自 [Mouse] 配置)
    void commandRequested(const QString& id);
    // 请求弹出右键菜单
    void contextMenuRequested(const QPoint& globalPos);
    // 鼠标在视图内移动(全屏时用于重新显示光标)
    void mouseMoved();

protected:
    // 绘制
    void paintEvent(QPaintEvent* e) override;
    // 滚轮
    void wheelEvent(QWheelEvent* e) override;
    // 鼠标按下
    void mousePressEvent(QMouseEvent* e) override;
    // 鼠标移动
    void mouseMoveEvent(QMouseEvent* e) override;
    // 鼠标松开
    void mouseReleaseEvent(QMouseEvent* e) override;
    // 双击
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    // 窗口大小变化
    void resizeEvent(QResizeEvent* e) override;

private slots:
    // GIF/WebP 动画换帧
    void onMovieFrame();

private:
    // 图片(旋转后)当前缩放下的显示矩形
    QRectF imageRect() const;
    // 限制偏移量,避免图片被拖出视野
    void clampOffset();
    // 把配置里的鼠标映射值(命令 ID/Pan/ContextMenu/None)发出去
    void dispatchMouse(const QString& configKey);
    // 当前显示用的位图(应用 γ 校正)
    QImage currentFrame() const;
    // 保存当前缩放并通知
    void zoomChangedInternal();

    QImage image_;            // 位图(非 SVG)
    QImage gammaImage_;       // 应用 γ 后的位图缓存
    QSvgRenderer* svg_;       // SVG 渲染器(isSvg 时使用)
    QMovie* movie_;           // 动画(isAnimated 且未禁用时)
    QString errorText_;       // 错误文字
    QSize baseSize_;          // 未旋转的图片尺寸
    double zoom_;
    int rotation_;
    QPointF offset_;          // 图片中心相对视图中心的偏移
    bool dragging_;
    QPoint lastMouse_;
    bool gammaEnabled_;
    double gamma_;
    bool disableAnimation_;
    bool shrinkToWindow_;
    bool enlargeToWindow_;
    bool shrinkToScreen_;
    bool enlargeToScreen_;
    QByteArray svgData_;
    QString path_;
    bool zoomLock_;           // 缩放锁定
    bool fitAuto_;            // 当前缩放是自动算出的(窗口大小变化时重新适应)
    bool cursorHidden_;       // 光标是否被隐藏(全屏用)
};
