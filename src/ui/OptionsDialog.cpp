#include "OptionsDialog.h"
#include "core/Config.h"
#include "core/Strings.h"
#include "core/FolderList.h"
#include "core/Theme.h"
#include "platform/FileAssoc.h"
#include "core/Commands.h"

#include <QColorDialog>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QStringList>

namespace {

// 把一页内容(布局已填好)包进滚动区,返回可放入标签页的控件
QWidget* wrapScroll(QWidget* content)
{
    QScrollArea* area = new QScrollArea();
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}

// 给颜色按钮设置显示颜色:按钮本身的样式完全走主题,颜色用图标色块表示(边框色取主题 border)
void paintColorButton(QPushButton* b, const QColor& c)
{
    b->setProperty("color", c.name(QColor::HexRgb));
    b->setText(c.name(QColor::HexRgb).toUpper());
    const QSize sz(36, 16);
    QPixmap pm(sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(c);
    p.setPen(QPen(Theme::color(QStringLiteral("border")), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, sz.width() - 1, sz.height() - 1), 4, 4);
    p.end();
    b->setIcon(QIcon(pm));
    b->setIconSize(sz);
}

// "段/键" 对应的界面文字键
QString optText(const QString& key)
{
    QString k = key;
    k.replace(QLatin1Char('/'), QLatin1Char('.'));
    return S(QStringLiteral("opt.") + k);
}

} // namespace

// 构造:7 页 + 确定/取消/应用
OptionsDialog::OptionsDialog(QWidget* parent) : QDialog(parent), tabs_(nullptr)
{
    setWindowTitle(S("dlg.options.title"));
    resize(520, 520);
    QVBoxLayout* root = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildBrowsePage(), S("options.page.browse"));
    tabs_->addTab(buildFileListPage(), S("options.page.filelist"));
    tabs_->addTab(buildThumbnailsPage(), S("options.page.thumbnails"));
    tabs_->addTab(buildViewerPage(), S("options.page.viewer"));
    tabs_->addTab(buildDisplayPage(), S("options.page.display"));
    tabs_->addTab(buildArchivePage(), S("options.page.archive"));
    tabs_->addTab(buildMiscPage(), S("options.page.misc"));
    root->addWidget(tabs_, 1);

    QDialogButtonBox* box = new QDialogButtonBox(this);
    QPushButton* ok = box->addButton(S("btn.ok"), QDialogButtonBox::AcceptRole);
    QPushButton* cancel = box->addButton(S("btn.cancel"), QDialogButtonBox::RejectRole);
    QPushButton* apply = box->addButton(S("btn.apply"), QDialogButtonBox::ApplyRole);
    root->addWidget(box);
    connect(ok, &QPushButton::clicked, this, &OptionsDialog::onOk);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(apply, &QPushButton::clicked, this, &OptionsDialog::onApply);

    loadValues();
}

// 浏览页
QWidget* OptionsDialog::buildBrowsePage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addCheck(v, QStringLiteral("Browse/ShowToolbar"));
    addCheck(v, QStringLiteral("Browse/ShowStatusBar"));
    addCheck(v, QStringLiteral("Browse/ShowMenu"));
    addCheck(v, QStringLiteral("Browse/ShowPathBar"));
    addCheck(v, QStringLiteral("Browse/ShowTree"));
    addCheck(v, QStringLiteral("Browse/ShowShortcuts"));
    addCheck(v, QStringLiteral("Browse/ShowPreview"));
    addCheck(v, QStringLiteral("Browse/HidePreviewInThumbMode"));
    addCheck(v, QStringLiteral("Browse/FullPathInTitle"));
    addEnum(v, QStringLiteral("Browse/WindowGeometryMode"));
    addCheck(v, QStringLiteral("Browse/StartInLastFolder"));
    addCheck(v, QStringLiteral("Browse/SaveRecentFolders"));
    addSpin(v, QStringLiteral("Browse/RecentFolderCount"));
    addCheck(v, QStringLiteral("Browse/SingleClickRename"));
    addCheck(v, QStringLiteral("Browse/DoubleClickEditDescription"));
    v->addStretch(1);
    return wrapScroll(page);
}

// 文件列表页
QWidget* OptionsDialog::buildFileListPage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addCheck(v, QStringLiteral("FileList/HideNonImages"));
    addCheck(v, QStringLiteral("FileList/ShowHidden"));
    addCheck(v, QStringLiteral("FileList/ShowLocalDrives"));
    addCheck(v, QStringLiteral("FileList/UseSystemIcons"));
    addCheck(v, QStringLiteral("FileList/AutoColumnWidth"));
    addCheck(v, QStringLiteral("FileList/ShowGrid"));
    addCheck(v, QStringLiteral("FileList/FullRowSelect"));
    addCheck(v, QStringLiteral("FileList/SizeInKB"));
    addCheck(v, QStringLiteral("FileList/HighlightImages"));
    addColor(v, QStringLiteral("FileList/HighlightColor"));
    addEnum(v, QStringLiteral("FileList/ReadImageInfo"));
    addCheck(v, QStringLiteral("FileList/PreviewAuto"));
    addSpin(v, QStringLiteral("FileList/PreviewDelayMs"));
    addSpin(v, QStringLiteral("FileList/PreviewHeight"));
    addCheck(v, QStringLiteral("FileList/AutoRefresh"));
    addCheck(v, QStringLiteral("FileList/AutoSelectNewFiles"));
    v->addStretch(1);
    return wrapScroll(page);
}

// 缩略图页
QWidget* OptionsDialog::buildThumbnailsPage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addSpin(v, QStringLiteral("Thumbnails/Width"));
    addSpin(v, QStringLiteral("Thumbnails/Height"));
    addSpin(v, QStringLiteral("Thumbnails/Border"));
    addCheck(v, QStringLiteral("Thumbnails/CacheEnabled"));
    addText(v, QStringLiteral("Thumbnails/CacheDir"));
    addCheck(v, QStringLiteral("Thumbnails/CacheCompress"));
    addCheck(v, QStringLiteral("Thumbnails/ShowFileSize"));
    addCheck(v, QStringLiteral("Thumbnails/ShowImageArea"));
    addCheck(v, QStringLiteral("Thumbnails/ShowAllFiles"));
    addCheck(v, QStringLiteral("Thumbnails/ShowFolders"));
    v->addStretch(1);
    return wrapScroll(page);
}

// 视图页
QWidget* OptionsDialog::buildViewerPage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addCheck(v, QStringLiteral("Viewer/ShowToolbar"));
    addCheck(v, QStringLiteral("Viewer/ShowStatusBar"));
    addCheck(v, QStringLiteral("Viewer/ShowMenu"));
    addCheck(v, QStringLiteral("Viewer/ShowFullPath"));
    addCheck(v, QStringLiteral("Viewer/ShrinkToWindow"));
    addCheck(v, QStringLiteral("Viewer/ShrinkToScreen"));
    addCheck(v, QStringLiteral("Viewer/EnlargeToWindow"));
    addCheck(v, QStringLiteral("Viewer/EnlargeToScreen"));
    addCheck(v, QStringLiteral("Viewer/AlwaysOnTop"));
    addCheck(v, QStringLiteral("Viewer/Fullscreen"));
    addCheck(v, QStringLiteral("Viewer/HideMouseInFullscreen"));
    addCheck(v, QStringLiteral("Viewer/AutoResizeWindow"));
    addCheck(v, QStringLiteral("Viewer/CenterOnResize"));
    addCheck(v, QStringLiteral("Viewer/SaveWindowPos"));
    addCheck(v, QStringLiteral("Viewer/ViewAllInFolder"));
    addCheck(v, QStringLiteral("Viewer/ExitAfterDelete"));
    addSpin(v, QStringLiteral("Viewer/PreloadCount"));
    v->addStretch(1);
    return wrapScroll(page);
}

// 显示页
QWidget* OptionsDialog::buildDisplayPage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addCheck(v, QStringLiteral("Display/GammaEnabled"));
    addDoubleSpin(v, QStringLiteral("Display/Gamma"));
    addCheck(v, QStringLiteral("Display/DisableGifAnimation"));
    v->addStretch(1);
    return wrapScroll(page);
}

// 归档页
QWidget* OptionsDialog::buildArchivePage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addEnum(v, QStringLiteral("Archive/DeleteMode"));
    addEnum(v, QStringLiteral("Archive/DuplicateAction"));
    addCheck(v, QStringLiteral("Archive/SkipIfIdentical"));
    addText(v, QStringLiteral("Archive/RenameSeparator"));
    addCheck(v, QStringLiteral("Archive/DescriptHidden"));
    addCheck(v, QStringLiteral("Archive/DescriptOem"));
    v->addStretch(1);
    return wrapScroll(page);
}

// 杂项页(含"设置文件关联"按钮)
QWidget* OptionsDialog::buildMiscPage()
{
    QWidget* page = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(page);
    addEnum(v, QStringLiteral("Misc/OnStartFile"));
    addEnum(v, QStringLiteral("Misc/OnDropFile"));
    addCheck(v, QStringLiteral("Misc/PrintShowSetup"));
    addCheck(v, QStringLiteral("Misc/TitleShowVersion"));
    addCheck(v, QStringLiteral("Misc/ShowToolTips"));
    addCheck(v, QStringLiteral("Misc/ShowInfoTips"));
    QPushButton* assoc = new QPushButton(S("options.btn.assoc"), page);
    connect(assoc, &QPushButton::clicked, this, &OptionsDialog::onFileAssoc);
    v->addWidget(assoc, 0, Qt::AlignLeft);
    v->addStretch(1);
    return wrapScroll(page);
}

// 复选框
QCheckBox* OptionsDialog::addCheck(QVBoxLayout* layout, const QString& key)
{
    QCheckBox* w = new QCheckBox(optText(key));
    layout->addWidget(w);
    Binding b;
    b.key = key;
    b.type = QStringLiteral("bool");
    b.widget = w;
    bindings_.append(b);
    return w;
}

// 整数
QSpinBox* OptionsDialog::addSpin(QVBoxLayout* layout, const QString& key)
{
    const ConfigKeyMeta* m = Config::instance().meta(key);
    if (m == nullptr) Config::fatal(S("err.unknownKey").arg(key));
    QSpinBox* w = new QSpinBox();
    if (m->hasRange) w->setRange(int(m->minValue), int(m->maxValue));
    else w->setRange(0, 99999);
    QHBoxLayout* row = new QHBoxLayout();
    row->addWidget(new QLabel(optText(key)));
    row->addStretch(1);
    row->addWidget(w);
    layout->addLayout(row);
    Binding b;
    b.key = key;
    b.type = QStringLiteral("int");
    b.widget = w;
    bindings_.append(b);
    return w;
}

// 小数
QDoubleSpinBox* OptionsDialog::addDoubleSpin(QVBoxLayout* layout, const QString& key)
{
    const ConfigKeyMeta* m = Config::instance().meta(key);
    if (m == nullptr) Config::fatal(S("err.unknownKey").arg(key));
    QDoubleSpinBox* w = new QDoubleSpinBox();
    w->setDecimals(2);
    w->setSingleStep(0.05);
    if (m->hasRange) w->setRange(m->minValue, m->maxValue);
    else w->setRange(0.0, 99999.0);
    QHBoxLayout* row = new QHBoxLayout();
    row->addWidget(new QLabel(optText(key)));
    row->addStretch(1);
    row->addWidget(w);
    layout->addLayout(row);
    Binding b;
    b.key = key;
    b.type = QStringLiteral("double");
    b.widget = w;
    bindings_.append(b);
    return w;
}

// 枚举:下拉框,每项的显示文字取 enum.段.键.值,itemData 存原始值
QComboBox* OptionsDialog::addEnum(QVBoxLayout* layout, const QString& key)
{
    const ConfigKeyMeta* m = Config::instance().meta(key);
    if (m == nullptr) Config::fatal(S("err.unknownKey").arg(key));
    QComboBox* w = new QComboBox();
    QString dotted = key;
    dotted.replace(QLatin1Char('/'), QLatin1Char('.'));
    for (int i = 0; i < m->enumValues.size(); ++i) {
        const QString v = m->enumValues.at(i);
        w->addItem(S(QStringLiteral("enum.") + dotted + QLatin1Char('.') + v), v);
    }
    QHBoxLayout* row = new QHBoxLayout();
    row->addWidget(new QLabel(optText(key)));
    row->addStretch(1);
    row->addWidget(w);
    layout->addLayout(row);
    Binding b;
    b.key = key;
    b.type = QStringLiteral("enum");
    b.widget = w;
    bindings_.append(b);
    return w;
}

// 文字
QLineEdit* OptionsDialog::addText(QVBoxLayout* layout, const QString& key)
{
    QLineEdit* w = new QLineEdit();
    QHBoxLayout* row = new QHBoxLayout();
    row->addWidget(new QLabel(optText(key)));
    row->addWidget(w, 1);
    layout->addLayout(row);
    Binding b;
    b.key = key;
    b.type = QStringLiteral("string");
    b.widget = w;
    bindings_.append(b);
    return w;
}

// 颜色:按钮显示当前颜色,点击弹出颜色选择
QPushButton* OptionsDialog::addColor(QVBoxLayout* layout, const QString& key)
{
    QPushButton* w = new QPushButton();
    w->setMinimumWidth(90);
    connect(w, &QPushButton::clicked, this, &OptionsDialog::onPickColor);
    QHBoxLayout* row = new QHBoxLayout();
    row->addWidget(new QLabel(optText(key)));
    row->addStretch(1);
    row->addWidget(w);
    layout->addLayout(row);
    Binding b;
    b.key = key;
    b.type = QStringLiteral("color");
    b.widget = w;
    bindings_.append(b);
    return w;
}

// 把配置值装入控件
void OptionsDialog::loadValues()
{
    Config& cfg = Config::instance();
    for (int i = 0; i < bindings_.size(); ++i) {
        const Binding& b = bindings_.at(i);
        if (b.type == QLatin1String("bool")) {
            static_cast<QCheckBox*>(b.widget)->setChecked(cfg.getBool(b.key));
        } else if (b.type == QLatin1String("int")) {
            static_cast<QSpinBox*>(b.widget)->setValue(cfg.getInt(b.key));
        } else if (b.type == QLatin1String("double")) {
            static_cast<QDoubleSpinBox*>(b.widget)->setValue(cfg.getDouble(b.key));
        } else if (b.type == QLatin1String("enum")) {
            QComboBox* c = static_cast<QComboBox*>(b.widget);
            const int idx = c->findData(cfg.getEnum(b.key));
            if (idx < 0) Config::fatal(S("err.configType").arg(b.key).arg(cfg.getString(b.key)).arg(S("err.unknownKey").arg(b.key)));
            c->setCurrentIndex(idx);
        } else if (b.type == QLatin1String("string")) {
            static_cast<QLineEdit*>(b.widget)->setText(cfg.getString(b.key));
        } else if (b.type == QLatin1String("color")) {
            paintColorButton(static_cast<QPushButton*>(b.widget), cfg.getColor(b.key));
        }
    }
}

// 把控件值写入配置
bool OptionsDialog::storeValues(QString* error)
{
    Config& cfg = Config::instance();
    // 先全部检查,通过后再写,避免写一半
    for (int i = 0; i < bindings_.size(); ++i) {
        const Binding& b = bindings_.at(i);
        if (b.type == QLatin1String("string")) {
            const QString t = static_cast<QLineEdit*>(b.widget)->text();
            if (t.isEmpty()) {
                if (error != nullptr) *error = S("err.configType").arg(b.key).arg(t).arg(S("err.valueEmpty"));
                return false;
            }
        }
    }
    for (int i = 0; i < bindings_.size(); ++i) {
        const Binding& b = bindings_.at(i);
        if (b.type == QLatin1String("bool")) {
            cfg.setBool(b.key, static_cast<QCheckBox*>(b.widget)->isChecked());
        } else if (b.type == QLatin1String("int")) {
            cfg.setInt(b.key, static_cast<QSpinBox*>(b.widget)->value());
        } else if (b.type == QLatin1String("double")) {
            cfg.setDouble(b.key, static_cast<QDoubleSpinBox*>(b.widget)->value());
        } else if (b.type == QLatin1String("enum")) {
            cfg.setString(b.key, static_cast<QComboBox*>(b.widget)->currentData().toString());
        } else if (b.type == QLatin1String("string")) {
            cfg.setString(b.key, static_cast<QLineEdit*>(b.widget)->text());
        } else if (b.type == QLatin1String("color")) {
            cfg.setString(b.key, b.widget->property("color").toString().toUpper());
        }
    }
    return true;
}

// 确定
void OptionsDialog::onOk()
{
    QString err;
    if (!storeValues(&err)) {
        QMessageBox::warning(this, S("err.title"), err);
        return;
    }
    emit optionsApplied();
    accept();
}

// 应用
void OptionsDialog::onApply()
{
    QString err;
    if (!storeValues(&err)) {
        QMessageBox::warning(this, S("err.title"), err);
        return;
    }
    emit optionsApplied();
}

// 设置文件关联:和菜单里的"设置文件关联"是同一个命令(只维护一份流程)
void OptionsDialog::onFileAssoc()
{
    Commands::instance().trigger(QStringLiteral("FileAssoc"));
}

// 颜色按钮被点击
void OptionsDialog::onPickColor()
{
    QPushButton* btn = qobject_cast<QPushButton*>(sender());
    if (btn == nullptr) return;
    const QColor cur(btn->property("color").toString());
    const QColor c = QColorDialog::getColor(cur, this, S("options.btn.pickColor"));
    if (c.isValid()) paintColorButton(btn, c);
}
