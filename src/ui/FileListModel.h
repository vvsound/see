#pragma once
#include <QAbstractTableModel>
#include <QString>
#include <QIcon>
#include <QFileIconProvider>
#include <QHash>
#include <QMap>
#include <QColor>
#include <QStringList>
#include "core/Types.h"
#include "core/FolderList.h"
#include "core/DescriptIon.h"
#include <memory>
#include "core/ImageLoader.h"

// 浏览窗口文件列表的数据模型(详细模式 4 列;缩略图/图标/列表模式用第 0 列)
class FileListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    // 自定义数据角色
    enum Role {
        PathRole = Qt::UserRole + 1,   // 完整路径
        IsDirRole,                     // 是否文件夹
        IsImageRole,                   // 是否图片
        ThumbRole,                     // 缩略图 QImage(可能为空)
        InfoLineRole,                  // 缩略图下方信息行 宽x高x色深
        IsParentRole,                  // 是否 ".." 行
        ErrorRole                      // 缩略图/信息读取失败的原因(无错误为空)
    };
    // 构造
    explicit FileListModel(QObject* parent);
    // 绑定文件夹列表
    void setFolderList(FolderList* list);
    // 从 FolderList 重新装载数据
    void reload();
    // 重新读取配置(是否含 ".." 行、显示哪些项、高亮色、KB 显示)
    void applyOptions();
    // 行数
    int rowCount(const QModelIndex& parent) const override;
    // 列数(名称、大小、图片属性、描述)
    int columnCount(const QModelIndex& parent) const override;
    // 数据
    QVariant data(const QModelIndex& index, int role) const override;
    // 表头
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    // 标志(可编辑:名称列、描述列)
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    // 编辑(重命名/描述)
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    // 某行对应的文件项(".." 行返回空项)
    FileEntry entryAt(int row) const;
    // 某路径所在的行,没有返回 -1
    int rowOfPath(const QString& path) const;
    // 是否有 ".." 行(总在第 0 行)
    bool hasParentRow() const;
    // 设置是否显示 ".." 行(详细/列表模式显示)
    void setParentRowVisible(bool visible);
    // 设置当前显示方式(缩略图方式会隐藏非图片/文件夹,按缩略图页选项)
    void setViewMode(ViewMode mode);
    // 按扩展名筛选:on 为假时全部显示;exts 为小写扩展名(空串=没有扩展名)。文件夹不受影响。不自动重新装载,调用后请 reload()
    void setExtFilter(bool on, const QStringList& exts);
    // 当前文件夹里(按当前显示方式本来会显示的)文件,每种扩展名各有几个;不受扩展名筛选影响。供筛选条生成标签
    QMap<QString, int> extensionCounts() const;
    // 清除"读取失败"记录(手动刷新时调用,失败的文件会重新尝试)
    void clearFailed();

signals:
    // 在列表里改名成功(刷新完成后发出,用于选中新文件)
    void renamed(const QString& oldPath, const QString& newPath);

private slots:
    // 缩略图生成完毕,刷新对应行
    void onThumbReady(const QString& path);
    // 图片信息读取完毕,刷新对应行
    void onInfoReady(const QString& path);

private:
    // 行号转 FolderList 下标(减去 ".." 行)
    int entryIndex(int row) const;
    // 取一行的图片属性文字
    QString infoText(const FileEntry& e) const;
    // 文件大小文字
    QString sizeText(const FileEntry& e) const;
    // 当前模式下是否显示此项
    bool visible(const FileEntry& e) const;
    // 同上,但不考虑扩展名筛选
    bool visibleIgnoringFilter(const FileEntry& e) const;

    FolderList* list_;
    QList<FileEntry> rows_;
    bool parentRow_;
    ViewMode mode_;
    bool sizeInKB_;
    bool highlightImages_;
    QColor highlightColor_;
    bool useSystemIcons_;
    int readInfoPolicy_;       // 0=仅固定磁盘 1=总是 2=从不
    QFileIconProvider iconProvider_;
    QHash<QString, int> rowIndex_;          // 路径 -> 行号
    QMap<QString, QString> failed_;         // 读取失败的路径 -> 原因
    bool hasParentNow_;                     // 当前文件夹是否有上一级
    bool readInfoNow_;                      // 当前文件夹是否读取图片信息列
    bool infoTips_;                         // 是否显示信息提示
    bool thumbShowFolders_;                 // 缩略图方式是否显示文件夹
    bool thumbShowAll_;                     // 缩略图方式是否显示非图片
    mutable QHash<QString, QIcon> iconCache_;
    std::unique_ptr<DescriptIon> descr_;    // 当前文件夹的 descript.ion
    QString descrErrFolder_;                // 已经报过读取错误的文件夹(避免自动刷新时重复弹窗)
    bool showShortcuts_;                    // Browse/ShowShortcuts
    bool descEditable_;                     // Browse/DoubleClickEditDescription
    bool extFilterOn_;                      // 是否按扩展名筛选
    QStringList extFilter_;                 // 允许显示的扩展名
};
