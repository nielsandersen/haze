#include "control.h"

#include <QDir>
#include <QLocalSocket>
#include <QStandardPaths>

Control::Control(const QString &path, QObject *parent)
    : QObject(parent), m_path(path.isEmpty() ? defaultPath() : path) {
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *client = m_server.nextPendingConnection()) {
            connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            connect(client, &QLocalSocket::readyRead, client, [this, client] {
                if (!client->canReadLine())
                    return;
                const QString command = QString::fromUtf8(client->readLine()).trimmed();
                client->write((handler ? handler(command) : QString()).toUtf8() + '\n');
                client->flush();
                client->disconnectFromServer();
            });
        }
    });
}

QString Control::defaultPath() {
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    return runtime + "/haze.sock";
}

bool Control::listen() {
    if (m_server.listen(m_path))
        return true;
    // Only called once no running Haze answered, so a socket in the way is a
    // leftover from one that was killed.
    QLocalServer::removeServer(m_path);
    return m_server.listen(m_path);
}

bool Control::send(const QString &path, const QString &command, QString *reply) {
    QLocalSocket socket;
    socket.connectToServer(path);
    if (!socket.waitForConnected(500))
        return false;
    socket.write(command.toUtf8() + '\n');
    socket.flush();
    while (!socket.canReadLine() && socket.waitForReadyRead(2000)) {
    }
    if (reply)
        *reply = QString::fromUtf8(socket.readLine()).trimmed();
    return true;
}
