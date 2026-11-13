#pragma once

#include <QLocalServer>
#include <QObject>
#include <functional>

// One Haze runs per session. A second `haze` hands its command (open, on,
// off, toggle, quit) to the first over a local socket and prints the answer.
class Control : public QObject {
    Q_OBJECT

public:
    explicit Control(const QString &path = {}, QObject *parent = nullptr);

    static QString defaultPath();

    // Takes the socket over from a Haze that died without cleaning up.
    bool listen();
    // Answers one command; set before listening.
    std::function<QString(const QString &)> handler;

    // Sends a command to a running Haze and waits briefly for its answer.
    // False when none is running.
    static bool send(const QString &path, const QString &command, QString *reply);

private:
    QString m_path;
    QLocalServer m_server;
};
