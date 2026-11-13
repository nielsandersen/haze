#include "hyprland.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString runtimeSocket(const char *name) {
    const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature.isEmpty())
        return {};
    return qEnvironmentVariable("XDG_RUNTIME_DIR") + "/hypr/" + signature + "/" + name;
}

} // namespace

Hyprland::Hyprland(const QString &requests, const QString &events, QObject *parent)
    : QObject(parent), m_requests(requests.isEmpty() ? runtimeSocket(".socket.sock") : requests) {
    const QString eventSocket = events.isEmpty() ? runtimeSocket(".socket2.sock") : events;

    // Every event that can move the focused window (focus, a window opening
    // beside it, a workspace switch) is a reason to ask where it is now.
    connect(&m_events, &QLocalSocket::readyRead, this, [this] {
        bool reloaded = false, any = false;
        while (m_events.canReadLine()) {
            const QByteArray line = m_events.readLine();
            any = true;
            reloaded = reloaded || line.startsWith("configreloaded>>");
        }
        if (reloaded) {
            refreshOptions();
            emit configReloaded();
        }
        if (any) {
            refreshMonitors();
            refresh();
        }
    });
    // Asked again on every connection, since anything may have changed between.
    connect(&m_events, &QLocalSocket::connected, this, [this] {
        m_reconnect.setInterval(1000);
        refreshOptions();
        refreshMonitors();
        refresh();
    });
    // A socket that can't be reached or drops is tried again, less often each time.
    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(1000);
    connect(&m_events, &QLocalSocket::disconnected, &m_reconnect, qOverload<>(&QTimer::start));
    connect(&m_events, &QLocalSocket::errorOccurred, &m_reconnect, qOverload<>(&QTimer::start));
    connect(&m_reconnect, &QTimer::timeout, this, [this, eventSocket] {
        m_reconnect.setInterval(std::min(m_reconnect.interval() * 2, 60000));
        m_events.abort();
        m_events.connectToServer(eventSocket, QIODevice::ReadOnly);
    });

    m_poll.setInterval(120);
    connect(&m_poll, &QTimer::timeout, this, &Hyprland::refresh);

    if (!available())
        return;
    if (!eventSocket.isEmpty())
        m_events.connectToServer(eventSocket, QIODevice::ReadOnly);
    refreshOptions();
    refreshMonitors();
    refresh();
}

Hyprland::~Hyprland() {
    // Nothing answers or reconnects while the members go.
    m_events.disconnect();
    for (QLocalSocket *socket : findChildren<QLocalSocket *>())
        socket->disconnect();
}

void Hyprland::request(const QString &command, std::function<void(const QByteArray &)> done) {
    if (!available()) {
        if (done)
            done({});
        return;
    }
    auto *socket = new QLocalSocket(this);
    // The reply so far, kept with the socket and freed with it.
    struct Pending : QObject {
        using QObject::QObject;
        QByteArray reply;
        bool finished = false;
    };
    auto *pending = new Pending(socket);
    // Hyprland answers and hangs up, so the reply is whole once it disconnects.
    auto finish = [socket, pending, done] {
        if (pending->finished)
            return;
        pending->finished = true;
        if (socket->isOpen())
            pending->reply += socket->readAll();
        socket->deleteLater();
        if (done)
            done(pending->reply);
    };
    connect(socket, &QLocalSocket::connected, socket, [socket, command] {
        socket->write(command.toUtf8());
        socket->flush();
    });
    connect(socket, &QLocalSocket::readyRead, socket, [socket, pending] { pending->reply += socket->readAll(); });
    connect(socket, &QLocalSocket::disconnected, socket, finish);
    connect(socket, &QLocalSocket::errorOccurred, socket, finish);
    // A Hyprland that stopped answering must not leave a refresh waiting forever.
    QTimer::singleShot(2000, socket, finish);
    socket->connectToServer(m_requests);
}

void Hyprland::evalNow(const QString &lua) {
    if (!available())
        return;
    QLocalSocket socket;
    socket.connectToServer(m_requests);
    if (!socket.waitForConnected(300))
        return;
    socket.write(("/eval " + lua).toUtf8());
    socket.waitForBytesWritten(300);
    socket.waitForReadyRead(300);
}

void Hyprland::setWatching(bool watching) {
    if (watching == m_poll.isActive())
        return;
    if (watching && available())
        m_poll.start();
    else
        m_poll.stop();
}

void Hyprland::refresh() {
    // An answer on its way may be from before the latest change, so ask once
    // more after it rather than twice at once.
    if (m_refreshing) {
        m_again = true;
        return;
    }
    m_refreshing = true;
    request("j/activewindow", [this](const QByteArray &reply) {
        m_refreshing = false;
        applyWindow(reply);
        if (m_again) {
            m_again = false;
            refresh();
        }
    });
}

void Hyprland::applyWindow(const QByteArray &reply) {
    // With nothing focused Hyprland answers with an empty object.
    const QJsonObject window = QJsonDocument::fromJson(reply).object();
    const QJsonArray at = window.value("at").toArray(), size = window.value("size").toArray();
    QRectF rect;
    if (at.size() == 2 && size.size() == 2 && window.value("mapped").toBool(true)
        && !window.value("hidden").toBool(false))
        rect = QRectF(at[0].toDouble(), at[1].toDouble(), size[0].toDouble(), size[1].toDouble());
    const int monitor = rect.isValid() ? window.value("monitor").toInt(-1) : -1;
    if (rect == m_window && monitor == m_windowMonitor)
        return;
    m_window = rect;
    m_windowMonitor = monitor;
    emit changed();
}

void Hyprland::refreshMonitors() {
    request("j/monitors", [this](const QByteArray &reply) {
        QList<Monitor> monitors;
        for (const QJsonValue &value : QJsonDocument::fromJson(reply).array()) {
            const QJsonObject object = value.toObject();
            // The area bars reserve, as left, top, right, bottom.
            const QJsonArray reserved = object.value("reserved").toArray();
            Monitor monitor;
            monitor.id = object.value("id").toInt(-1);
            monitor.name = object.value("name").toString();
            monitor.left = reserved.at(0).toDouble();
            monitor.top = reserved.at(1).toDouble();
            monitor.right = reserved.at(2).toDouble();
            monitor.bottom = reserved.at(3).toDouble();
            monitor.origin = QPointF(object.value("x").toDouble() + monitor.left,
                                     object.value("y").toDouble() + monitor.top);
            monitors << monitor;
        }
        // No answer keeps what was known rather than forgetting every monitor.
        if (monitors.isEmpty() || monitors == m_monitors)
            return;
        m_monitors = monitors;
        emit changed();
    });
}

void Hyprland::refreshOptions() {
    request("j/getoption general:border_size", [this](const QByteArray &reply) {
        const QJsonValue value = QJsonDocument::fromJson(reply).object().value("int");
        if (!value.isDouble() || value.toInt() == m_borderSize)
            return;
        m_borderSize = value.toInt();
        emit changed();
    });
}

QVariantMap Hyprland::holes() const {
    QVariantMap holes;
    if (!m_window.isValid())
        return holes;
    for (const Monitor &monitor : m_monitors)
        if (monitor.id == m_windowMonitor)
            holes.insert(monitor.name, m_window.translated(-monitor.origin));
    return holes;
}

QVariantMap Hyprland::reserved() const {
    QVariantMap reserved;
    for (const Monitor &monitor : m_monitors)
        reserved.insert(monitor.name, QVariantMap{{"left", monitor.left}, {"top", monitor.top},
                                                  {"right", monitor.right}, {"bottom", monitor.bottom}});
    return reserved;
}
