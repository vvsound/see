#pragma once
#include <QObject>
#include <QStringList>
#include <QLocalServer>

// 单实例:已有程序在运行时,把要打开的路径转交给它
class SingleInstance : public QObject {
    Q_OBJECT
public:
    // 构造
    explicit SingleInstance(QObject* parent);
    // 尝试把路径发给已运行的实例;成功返回 true(本进程应退出)
    bool sendToRunning(const QStringList& paths);
    // 成为服务端,开始监听;失败写 error
    bool listen(QString* error);

signals:
    // 收到其他进程转交的路径
    void pathsReceived(const QStringList& paths);

private slots:
    // 有新连接
    void onNewConnection();

private:
    // 本次登录用户专属的服务名
    static QString serverName();
    QLocalServer* server_;
};
