#include "FilterBar.h"
#include "core/Config.h"
#include "core/Strings.h"

#include <QHBoxLayout>
#include <QComboBox>
#include <QToolButton>
#include <QButtonGroup>
#include <QPair>
#include <QLayoutItem>
#include <algorithm>

namespace {

// 排序依据名称 <-> SortKey
bool sortKeyFromName(const QString& n, SortKey* out)
{
    if (n == QLatin1String("Name")) { *out = SortKey::Name; return true; }
    if (n == QLatin1String("Size")) { *out = SortKey::Size; return true; }
    if (n == QLatin1String("Date")) { *out = SortKey::Date; return true; }
    if (n == QLatin1String("Type")) { *out = SortKey::Type; return true; }
    return false;
}

QString sortKeyName(SortKey k)
{
    switch (k) {
    case SortKey::Name: return QStringLiteral("Name");
    case SortKey::Size: return QStringLiteral("Size");
    case SortKey::Date: return QStringLiteral("Date");
    case SortKey::Type: return QStringLiteral("Type");
    }
    return QStringLiteral("Name");
}

} // namespace

// 构造
FilterBar::FilterBar(QWidget* parent)
    : QWidget(parent), tagLay_(nullptr), tagHolder_(nullptr), sortBox_(nullptr), dirBtn_(nullptr), gridBtn_(nullptr),
      listBtn_(nullptr), sortHolder_(nullptr), viewHolder_(nullptr), currentId_(QStringLiteral("all")),
      curKey_(SortKey::Name), curDesc_(false), grid_(false), enabled_(true), showCounts_(true), groupOthers_(true),
      upper_(true), resetOnFolder_(true), maxTags_(6)
{
    setObjectName(QStringLiteral("mmFilterBar"));
    setAttribute(Qt::WA_StyledBackground, true);  // 否则主题里 #mmFilterBar 的背景不会画
    QHBoxLayout* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 4, 8, 4);
    lay->setSpacing(6);

    tagHolder_ = new QWidget(this);
    tagLay_ = new QHBoxLayout(tagHolder_);
    tagLay_->setContentsMargins(0, 0, 0, 0);
    tagLay_->setSpacing(4);
    lay->addWidget(tagHolder_, 1);

    sortHolder_ = new QWidget(this);
    QHBoxLayout* sl = new QHBoxLayout(sortHolder_);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(2);
    sortBox_ = new QComboBox(sortHolder_);
    sortBox_->setFocusPolicy(Qt::NoFocus);
    sortBox_->setToolTip(S("filter.sort.tip"));
    sl->addWidget(sortBox_);
    dirBtn_ = new QToolButton(sortHolder_);
    dirBtn_->setFocusPolicy(Qt::NoFocus);
    dirBtn_->setAutoRaise(true);
    sl->addWidget(dirBtn_);
    lay->addWidget(sortHolder_);

    viewHolder_ = new QWidget(this);
    QHBoxLayout* vl = new QHBoxLayout(viewHolder_);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(2);
    gridBtn_ = new QToolButton(viewHolder_);
    listBtn_ = new QToolButton(viewHolder_);
    QToolButton* vb[2] = { gridBtn_, listBtn_ };
    for (int i = 0; i < 2; ++i) {
        vb[i]->setCheckable(true);
        vb[i]->setAutoRaise(true);
        vb[i]->setFocusPolicy(Qt::NoFocus);
        vl->addWidget(vb[i]);
    }
    gridBtn_->setText(S("filter.view.grid"));
    gridBtn_->setToolTip(S("filter.view.grid.tip"));
    listBtn_->setText(S("filter.view.list"));
    listBtn_->setToolTip(S("filter.view.list.tip"));
    QButtonGroup* grp = new QButtonGroup(this);
    grp->setExclusive(true);
    grp->addButton(gridBtn_);
    grp->addButton(listBtn_);
    lay->addWidget(viewHolder_);

    connect(sortBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (i < 0 || i >= sortKeys_.size()) return;
        curKey_ = sortKeys_.at(i);
        emit sortChanged(curKey_, curDesc_);
    });
    connect(dirBtn_, &QToolButton::clicked, this, [this]() { reverseSort(); });
    connect(gridBtn_, &QToolButton::clicked, this, [this]() { emit viewCommandRequested(gridCmd_); });
    connect(listBtn_, &QToolButton::clicked, this, [this]() { emit viewCommandRequested(listCmd_); });

    applyOptions();
}

// 显示方式名 -> 命令 ID
QString FilterBar::commandForMode(const QString& modeName)
{
    return QStringLiteral("View") + modeName;
}

// 重新读取设置
void FilterBar::applyOptions()
{
    Config& c = Config::instance();
    enabled_ = c.getBool(QStringLiteral("Filter/ShowBar"));
    showCounts_ = c.getBool(QStringLiteral("Filter/ShowCounts"));
    groupOthers_ = c.getBool(QStringLiteral("Filter/GroupOthers"));
    maxTags_ = c.getInt(QStringLiteral("Filter/MaxTags"));
    upper_ = (c.getEnum(QStringLiteral("Filter/TagCase")) == QLatin1String("Upper"));
    resetOnFolder_ = c.getBool(QStringLiteral("Filter/ResetOnFolderChange"));
    gridCmd_ = commandForMode(c.getEnum(QStringLiteral("Filter/GridMode")));
    listCmd_ = commandForMode(c.getEnum(QStringLiteral("Filter/ListMode")));

    // 排序依据列表
    sortKeys_.clear();
    const QStringList names = c.getList(QStringLiteral("Filter/SortKeys"));
    for (int i = 0; i < names.size(); ++i) {
        SortKey k = SortKey::Name;
        if (!sortKeyFromName(names.at(i), &k)) {
            Config::fatal(S("err.filterSortKey").arg(Config::iniPath()).arg(c.lineNumber(QStringLiteral("Filter/SortKeys"))).arg(names.at(i)));
        }
        if (!sortKeys_.contains(k)) sortKeys_.append(k);
    }
    sortBox_->blockSignals(true);
    sortBox_->clear();
    for (int i = 0; i < sortKeys_.size(); ++i) {
        sortBox_->addItem(S(QStringLiteral("filter.sort.") + sortKeyName(sortKeys_.at(i))));
    }
    sortBox_->blockSignals(false);
    setSort(curKey_, curDesc_);

    sortHolder_->setVisible(c.getBool(QStringLiteral("Filter/ShowSort")) && !sortKeys_.isEmpty());
    viewHolder_->setVisible(c.getBool(QStringLiteral("Filter/ShowViewToggle")));
    syncDirButton();
    if (!enabled_) currentId_ = QStringLiteral("all");
    rebuildTags();
    setVisible(enabled_);
}

// 条是否显示
bool FilterBar::barEnabled() const
{
    return enabled_;
}

// 重建标签
void FilterBar::setExtensions(const QMap<QString, int>& counts, bool folderChanged)
{
    counts_ = counts;
    if (folderChanged && resetOnFolder_) currentId_ = QStringLiteral("all");
    rebuildTags();
}

// 按 counts_ 生成标签按钮
bool FilterBar::rebuildTags()
{
    // 清除旧按钮
    // (连同末尾的弹性空白一起清掉,否则每次重建都会多一个)
    while (tagLay_->count() > 0) {
        QLayoutItem* it = tagLay_->takeAt(0);
        if (it->widget() != nullptr) {
            it->widget()->hide();
            it->widget()->deleteLater();
        }
        delete it;
    }
    tagButtons_.clear();
    tags_.clear();

    // 按文件数从多到少、同数量按扩展名排序
    QList<QPair<int, QString> > order;
    int total = 0;
    for (QMap<QString, int>::const_iterator it = counts_.constBegin(); it != counts_.constEnd(); ++it) {
        order.append(qMakePair(it.value(), it.key()));
        total += it.value();
    }
    std::sort(order.begin(), order.end(), [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });

    Tag all;
    all.id = QStringLiteral("all");
    all.count = total;
    all.text = showCounts_ ? S("filter.tagCount").arg(S("filter.all")).arg(total) : S("filter.all");
    tags_.append(all);

    QStringList restExts;
    int restCount = 0;
    for (int i = 0; i < order.size(); ++i) {
        const QString ext = order.at(i).second;
        const int n = order.at(i).first;
        if (i < maxTags_) {
            Tag t;
            t.id = QStringLiteral("ext:") + ext;
            t.exts << ext;
            t.count = n;
            QString label = ext.isEmpty() ? S("filter.noExt") : (upper_ ? ext.toUpper() : ext.toLower());
            t.text = showCounts_ ? S("filter.tagCount").arg(label).arg(n) : label;
            tags_.append(t);
        } else {
            restExts << ext;
            restCount += n;
        }
    }
    if (groupOthers_ && !restExts.isEmpty()) {
        Tag o;
        o.id = QStringLiteral("other");
        o.exts = restExts;
        o.count = restCount;
        o.text = showCounts_ ? S("filter.tagCount").arg(S("filter.other")).arg(restCount) : S("filter.other");
        tags_.append(o);
    }

    // 当前标签不存在了:回到"全部"
    bool reset = false;
    bool found = false;
    for (int i = 0; i < tags_.size(); ++i) {
        if (tags_.at(i).id == currentId_) found = true;
    }
    if (!found) {
        reset = (currentId_ != QLatin1String("all"));
        currentId_ = QStringLiteral("all");
    }

    for (int i = 0; i < tags_.size(); ++i) {
        QToolButton* b = new QToolButton(tagHolder_);
        b->setObjectName(QStringLiteral("mmFilterTag"));
        b->setText(tags_.at(i).text);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(S("filter.tag.tip").arg(tags_.at(i).count));
        const QString id = tags_.at(i).id;
        connect(b, &QToolButton::clicked, this, [this, id]() { selectTag(id); });
        tagLay_->addWidget(b);
        tagButtons_.append(b);
    }
    tagLay_->addStretch(1);
    syncTagButtons();
    return reset;
}

// 刷新标签按钮的选中状态
void FilterBar::syncTagButtons()
{
    for (int i = 0; i < tagButtons_.size() && i < tags_.size(); ++i) {
        tagButtons_.at(i)->setChecked(tags_.at(i).id == currentId_);
    }
}

// 选中标签
void FilterBar::selectTag(const QString& id)
{
    if (id == currentId_) {
        syncTagButtons();
        return;
    }
    currentId_ = id;
    syncTagButtons();
    emit filterChanged();
}

// 是否在筛选
bool FilterBar::filterActive() const
{
    return enabled_ && currentId_ != QLatin1String("all");
}

// 当前允许的扩展名
QStringList FilterBar::filterExts() const
{
    if (!filterActive()) return QStringList();
    for (int i = 0; i < tags_.size(); ++i) {
        if (tags_.at(i).id == currentId_) return tags_.at(i).exts;
    }
    return QStringList();
}

// 切换标签
void FilterBar::cycleTag(int delta)
{
    if (!enabled_ || tags_.size() < 2) return;
    int idx = 0;
    for (int i = 0; i < tags_.size(); ++i) {
        if (tags_.at(i).id == currentId_) idx = i;
    }
    const int n = tags_.size();
    idx = ((idx + delta) % n + n) % n;
    selectTag(tags_.at(idx).id);
}

// 回到"全部"
void FilterBar::selectAllTag()
{
    if (!enabled_) return;
    selectTag(QStringLiteral("all"));
}

// 同步排序状态
void FilterBar::setSort(SortKey key, bool desc)
{
    curKey_ = key;
    curDesc_ = desc;
    sortBox_->blockSignals(true);
    sortBox_->setCurrentIndex(sortKeys_.indexOf(key));
    sortBox_->blockSignals(false);
    syncDirButton();
}

// 排序方向按钮
void FilterBar::syncDirButton()
{
    dirBtn_->setText(curDesc_ ? S("filter.desc") : S("filter.asc"));
    dirBtn_->setToolTip(curDesc_ ? S("filter.desc.tip") : S("filter.asc.tip"));
}

// 下一种排序依据
void FilterBar::cycleSort()
{
    if (sortKeys_.isEmpty()) return;
    const int idx = sortKeys_.indexOf(curKey_);
    const SortKey next = sortKeys_.at((idx + 1) % sortKeys_.size());
    setSort(next, curDesc_);
    emit sortChanged(curKey_, curDesc_);
}

// 反转排序方向
void FilterBar::reverseSort()
{
    setSort(curKey_, !curDesc_);
    emit sortChanged(curKey_, curDesc_);
}

// 同步显示方式
void FilterBar::setGrid(bool grid)
{
    grid_ = grid;
    gridBtn_->blockSignals(true);
    listBtn_->blockSignals(true);
    gridBtn_->setChecked(grid);
    listBtn_->setChecked(!grid);
    gridBtn_->blockSignals(false);
    listBtn_->blockSignals(false);
}

// 切换要执行的命令
QString FilterBar::toggleCommand() const
{
    return grid_ ? listCmd_ : gridCmd_;
}
