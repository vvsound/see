#pragma once
#include <QString>
#include <QMap>

// 读写文件夹里的 descript.ion(每行:文件名 空格 描述)
class DescriptIon {
public:
    // 构造:folder 为文件夹路径
    explicit DescriptIon(const QString& folder);
    // 读取(文件不存在不算错);失败写 error
    bool load(QString* error);
    // 取某文件的描述(没有返回空)
    QString get(const QString& fileName) const;
    // 设置某文件的描述(空表示删除)
    void set(const QString& fileName, const QString& text);
    // 写回文件(按 Archive/DescriptOem、DescriptHidden);失败写 error
    bool save(QString* error);
    // 文件是否被修改过
    bool isDirty() const;

private:
    // descript.ion 的完整路径
    QString filePath() const;
    QString folder_;
    QMap<QString, QString> map_;       // 小写文件名 -> 描述
    QMap<QString, QString> nameCase_;  // 小写文件名 -> 原始文件名
    bool dirty_;
};
