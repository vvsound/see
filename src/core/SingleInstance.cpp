#include "SingleInstance.h"
#include "Strings.h"

#include <QLocalSocket>
#include <QDataStream>
#include <QByteArray>
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>

SingleInstance::SingleInstance(QObject* parent) : QObject(parent), server_(nullptr)
{
}

// 尝试把路径发给已运行的实例;连不上说明没有已运行实例,返回 false
bool SingleInstance::sendToRunning(const QStringList& paths)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(300)) {
        return false;
    }
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << paths;
    socket.write(block);
    if (!socket.waitForBytesWritten(2000)) {
        return false;
    }
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(1000);
    }
    return true;
}

// 成为服务端
bool SingleInstance::listen(QString* error)
{
    if (server_ == nullptr) {
        server_ = new QLocalServer(this);
        connect(server_, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    }
    // 上次异常退出可能残留同名服务,先清掉
    QLocalServer::removeServer(serverName());
    if (!server_->listen(serverName())) {
        if (error != nullptr) {
            *error = server_->errorString();
        }
        return false;
    }
    return true;
}

// 有新连接:读完全部数据后发出 pathsReceived
void SingleInstance::onNewConnection()
{
    while (server_->hasPendingConnections()) {
        QLocalSocket* sock = server_->nextPendingConnection();
        connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
        connect(sock, &QLocalSocket::readyRead, this, [this, sock]() {
            QDataStream in(sock);
            in.setVersion(QDataStream::Qt_6_0);
            in.startTransaction();
            QStringList paths;
            in >> paths;
            if (!in.commitTransaction()) {
                return; // 数据还没收全,等下一次 readyRead
            }
            emit pathsReceived(paths);
        });
    }
}

// 本次登录用户专属的服务名:用 exe 所在文件夹的哈希区分不同安装
QString SingleInstance::serverName()
{
    const QByteArray h = QCryptographicHash::hash(
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toUtf8(),
        QCryptographicHash::Sha1).toHex();
    return QStringLiteral("QuickView-") + QString::fromLatin1(h.left(16));
}
