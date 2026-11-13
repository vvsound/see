#pragma once
#include <QString>
#include <QMap>

// 界面文字:从 lang/zh-CN.ini 读取;找不到键名直接报错退出
class Strings {
public:
    // 全局唯一实例
    static Strings& instance();
    // 载入文字表:先找 exe 旁的 lang/zh-CN.ini,没有则用内置的;失败返回 false
    bool load(QString* error);
    // 取文字(找不到键名 fatal)
    QString get(const QString& key) const;
    // 键名是否存在
    bool has(const QString& key) const;

private:
    Strings();
    bool parse(const QString& text, const QString& sourceName, QString* error);
    QMap<QString, QString> map_;
};

// 取界面文字的简写
inline QString S(const QString& key) { return Strings::instance().get(key); }
inline QString S(const char* key) { return Strings::instance().get(QString::fromLatin1(key)); }
