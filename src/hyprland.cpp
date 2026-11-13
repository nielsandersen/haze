#include "hyprland.h"

#include <QJsonArray>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// How often a watched window is looked at: at rest, and on the move.
const int restingInterval = 120;
const int movingInterval = 6;

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

    m_poll.setInterval(restingInterval);
    connect(&m_poll, &QTimer::timeout, this, &Hyprland::refresh);
    // Still for a moment, the window has come to rest.
    m_settle.setSingleShot(true);
    m_settle.setInterval(250);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        m_moving = false;
        if (m_poll.isActive())
            m_poll.start(restingInterval);
        emit changed();
    });

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
        m_poll.start(m_moving ? movingInterval : restingInterval);
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
    const QString address = window.value("address").toString();
    QStringList tags;
    for (const QJsonValue &tag : window.value("tags").toArray())
        tags << tag.toString();
    if (rect == m_window && monitor == m_windowMonitor && tags == m_windowTags && address == m_windowAddress)
        return;

    // The same window somewhere else is a window on the move: a drag or a
    // resize with the mouse, which Hyprland announces nothing about. Until it
    // rests it is looked at every few milliseconds rather than a few times a
    // second, so the hole keeps up with it.
    const bool moved = rect.isValid() && m_window.isValid() && address == m_windowAddress && rect != m_window;
    if (moved) {
        m_windowBefore = m_window;
        m_moving = true;
        m_settle.start();
        if (m_poll.isActive())
            m_poll.start(movingInterval);
    } else {
        m_moving = false;
        m_settle.stop();
    }
    m_window = rect;
    m_windowMonitor = monitor;
    m_windowTags = tags;
    m_windowAddress = address;
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

// The focused window with its title bar, when it has one. Hyprland reports a
// window without the bar the hyprbars plugin draws above it, so a hole the
// size of the window alone would leave the haze over the title bar.
QRectF Hyprland::framedWindow() const {
    QRectF window = m_window;
    // A tag set by a rule is listed with a star after it.
    const bool tagged = m_titlebarTag.isEmpty() || m_windowTags.contains(m_titlebarTag)
                        || m_windowTags.contains(m_titlebarTag + "*");
    if (window.isValid() && m_titlebars && m_titlebarHeight > 0 && tagged)
        window.setTop(window.top() - m_titlebarHeight);

    // The haze reaches the screen a frame or two after the window was looked
    // at, and a window on the move is further along by then. So the hole
    // also covers where the window is heading, each edge as far again as it
    // just went: a little sharp background behind a moving window is better
    // than haze over its leading edge.
    if (window.isValid() && m_moving && m_windowBefore.isValid()) {
        auto ahead = [](double now, double before) { return now + std::clamp(now - before, -160., 160.); };
        QRectF next;
        next.setLeft(ahead(m_window.left(), m_windowBefore.left()));
        next.setTop(ahead(m_window.top(), m_windowBefore.top()));
        next.setRight(ahead(m_window.right(), m_windowBefore.right()));
        next.setBottom(ahead(m_window.bottom(), m_windowBefore.bottom()));
        window = window.united(next.normalized());
    }
    return window;
}

void Hyprland::refreshOptions() {
    // Without the plugin Hyprland knows no such options, and the answers are
    // not JSON: then there are no title bars.
    request("j/getoption plugin:hyprbars:enabled", [this](const QByteArray &reply) {
        const QJsonObject object = QJsonDocument::fromJson(reply).object();
        const bool enabled = object.value("bool").toBool(object.value("int").toInt() != 0);
        if (enabled == m_titlebars)
            return;
        m_titlebars = enabled;
        emit changed();
    });
    request("j/getoption plugin:hyprbars:bar_height", [this](const QByteArray &reply) {
        const int height = QJsonDocument::fromJson(reply).object().value("int").toInt();
        if (height == m_titlebarHeight)
            return;
        m_titlebarHeight = height;
        emit changed();
    });
    request("j/getoption plugin:hyprbars:workspace_tag", [this](const QByteArray &reply) {
        const QString tag = QJsonDocument::fromJson(reply).object().value("str").toString();
        if (tag == m_titlebarTag)
            return;
        m_titlebarTag = tag;
        emit changed();
    });
    // A Hyprland that takes Lua answers "ok"; no answer at all says nothing
    // either way.
    request("/eval haze_asked = true", [this](const QByteArray &reply) {
        if (reply.trimmed().isEmpty())
            return;
        const bool scriptable = reply.trimmed() == "ok";
        if (scriptable == m_scriptable)
            return;
        m_scriptable = scriptable;
        emit changed();
    });
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
            holes.insert(monitor.name, framedWindow().translated(-monitor.origin));
    return holes;
}

QVariantMap Hyprland::reserved() const {
    QVariantMap reserved;
    for (const Monitor &monitor : m_monitors)
        reserved.insert(monitor.name, QVariantMap{{"left", monitor.left}, {"top", monitor.top},
                                                  {"right", monitor.right}, {"bottom", monitor.bottom}});
    return reserved;
}
