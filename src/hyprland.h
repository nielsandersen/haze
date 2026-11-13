#pragma once

#include <QList>
#include <QLocalSocket>
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <QVariantMap>
#include <functional>

// What Haze needs to know from Hyprland, kept current: where the focused
// window is, where each monitor's usable area starts, and a few options.
// It also carries Haze's requests the other way (a layer rule, blur options).
class Hyprland : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool hasActiveWindow READ hasActiveWindow NOTIFY changed)
    Q_PROPERTY(QVariantMap holes READ holes NOTIFY changed)
    Q_PROPERTY(QVariantMap reserved READ reserved NOTIFY changed)
    Q_PROPERTY(int borderSize READ borderSize NOTIFY changed)
    Q_PROPERTY(bool scriptable READ scriptable NOTIFY changed)

public:
    // Without arguments it finds the running Hyprland's sockets. Tests pass
    // their own request and event sockets.
    explicit Hyprland(const QString &requests = {}, const QString &events = {}, QObject *parent = nullptr);
    ~Hyprland() override;

    bool available() const { return !m_requests.isEmpty(); }
    bool hasActiveWindow() const { return m_window.isValid(); }
    int borderSize() const { return m_borderSize; }
    // Whether this Hyprland takes Lua, which is how Haze gives it the blur
    // rule. One configured the older way answers that it doesn't know how.
    bool scriptable() const { return m_scriptable; }

    // For each monitor showing the focused window, by name: the window's
    // rectangle measured from the corner of that monitor's usable area, which
    // is where a layer surface that respects the bar begins.
    QVariantMap holes() const;

    // For each monitor, by name: how much of each edge its bars reserve, as
    // { left, top, right, bottom }.
    QVariantMap reserved() const;

    // One request on Hyprland's request socket; `done` gets the reply, or
    // nothing when Hyprland could not be reached.
    void request(const QString &command, std::function<void(const QByteArray &)> done = {});
    void eval(const QString &lua) { request("/eval " + lua); }
    // Waits for the request to be sent, briefly. Only for the last words on
    // the way out, when no event loop is left to send them.
    void evalNow(const QString &lua);

    // While a haze is on screen the focused window is also asked for on a
    // timer: Hyprland announces focus changes, but not a window being
    // dragged or resized with the mouse.
    void setWatching(bool watching);

public slots:
    void refresh();

signals:
    void changed();
    void configReloaded();

private:
    struct Monitor {
        int id = -1;
        QString name;
        QPointF origin;
        double left = 0, top = 0, right = 0, bottom = 0;
        bool operator==(const Monitor &other) const {
            return id == other.id && name == other.name && origin == other.origin && left == other.left
                   && top == other.top && right == other.right && bottom == other.bottom;
        }
    };

    void refreshMonitors();
    void refreshOptions();
    void applyWindow(const QByteArray &reply);

    QString m_requests;
    QLocalSocket m_events;
    QTimer m_reconnect;
    QTimer m_poll;
    bool m_refreshing = false;
    bool m_again = false;

    QList<Monitor> m_monitors;
    QRectF m_window;
    int m_windowMonitor = -1;
    int m_borderSize = 0;
    bool m_scriptable = true;
};
