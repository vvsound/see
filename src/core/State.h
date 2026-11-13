#pragma once
#include <QString>
#include <QStringList>
#include <QMap>

// 程序自己记的状态(窗口位置、最近文件夹、显示方式等),存到 QuickView.state;不动用户的 ini
class State {
public:
    // 全局唯一实例
    static State& instance();
    // 读取 QuickView.state(文件不存在不算错)
    void load();
    // 读字符串,没有则返回 def
    QString getString(const QString& key, const QString& def) const;
    int getInt(const QString& key, int def) const;
    bool getBool(const QString& key, bool def) const;
    QStringList getList(const QString& key) const;
    // 写入并立即保存
    void setString(const QString& key, const QString& value);
    void setInt(const QString& key, int value);
    void setBool(const QString& key, bool value);
    void setList(const QString& key, const QStringList& value);
    // 保存到磁盘
    void save();
    // QuickView.state 的完整路径
    static QString statePath();

private:
    State();
    QMap<QString, QString> values_;
};
