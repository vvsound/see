#pragma once
#include <QWidget>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include "core/FolderList.h"

class QHBoxLayout;
class QComboBox;
class QToolButton;
class QButtonGroup;

// 中栏顶部的筛选条:扩展名标签(按当前文件夹里实际有的扩展名自动生成)、排序、网格/列表切换。
// 所有开关、数量、排序项见 QuickView.ini 的 [Filter]。
class FilterBar : public QWidget {
    Q_OBJECT
public:
    // 构造
    explicit FilterBar(QWidget* parent);
    // 重新读取 [Filter] 设置并重建界面
    void applyOptions();
    // 条是否显示(Filter/ShowBar)
    bool barEnabled() const;
    // 用当前文件夹里每种扩展名的文件数重建标签(扩展名小写,空串=没有扩展名)。
    // folderChanged 为真且设置了 Filter/ResetOnFolderChange 时回到"全部"。不发 filterChanged 信号。
    void setExtensions(const QMap<QString, int>& counts, bool folderChanged);
    // 当前是否在按扩展名筛选
    bool filterActive() const;
    // 当前允许显示的扩展名(filterActive 为真时有效;空串元素=没有扩展名的文件)
    QStringList filterExts() const;
    // 在标签里向后(+1)/向前(-1)切换;回到"全部"
    void cycleTag(int delta);
    void selectAllTag();
    // 同步排序状态(不发信号)
    void setSort(SortKey key, bool desc);
    // 下一种排序依据 / 反转排序方向(发 sortChanged)
    void cycleSort();
    void reverseSort();
    // 同步显示方式(true=网格类,false=列表类;不发信号)
    void setGrid(bool grid);
    // 切换网格/列表时要执行的命令 ID(例如 ViewThumbInfo / ViewDetails)
    QString toggleCommand() const;

signals:
    // 筛选标签变了
    void filterChanged();
    // 用户在条上改了排序
    void sortChanged(SortKey key, bool desc);
    // 用户点了网格/列表按钮,请求执行该命令
    void viewCommandRequested(const QString& commandId);

private:
    struct Tag {
        QString id;          // "all" / "other" / "ext:jpg"
        QString text;        // 按钮文字(已含数量)
        QStringList exts;    // 该标签包含的扩展名
        int count;
    };
    // 重建标签按钮;返回当前标签是否因消失而被重置
    bool rebuildTags();
    // 选中某标签并发信号
    void selectTag(const QString& id);
    // 刷新各按钮的选中状态
    void syncTagButtons();
    // 刷新排序方向按钮文字
    void syncDirButton();
    // 读命令 ID 设置
    static QString commandForMode(const QString& modeName);

    QHBoxLayout* tagLay_;
    QWidget* tagHolder_;
    QComboBox* sortBox_;
    QToolButton* dirBtn_;
    QToolButton* gridBtn_;
    QToolButton* listBtn_;
    QWidget* sortHolder_;
    QWidget* viewHolder_;
    QList<Tag> tags_;
    QList<QToolButton*> tagButtons_;
    QString currentId_;
    QMap<QString, int> counts_;
    QList<SortKey> sortKeys_;
    SortKey curKey_;
    bool curDesc_;
    bool grid_;
    bool enabled_;
    bool showCounts_;
    bool groupOthers_;
    bool upper_;
    bool resetOnFolder_;
    int maxTags_;
    QString gridCmd_;
    QString listCmd_;
};
