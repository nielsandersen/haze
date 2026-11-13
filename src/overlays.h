#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantAnimation>

class Backend;
class Hyprland;
class QQmlEngine;
class QQuickView;
class QScreen;

// The haze itself: one see-through surface per screen, above the windows and
// below the panels' popups, that takes no input. Each draws Overlay.qml, the
// tint with a hole where the focused window is. A second one per screen lies
// under the windows and the bar, and hazes the wallpaper behind the bar, so
// the bar's own clock and icons stay sharp on top of it. They exist only
// while Haze is on and a window has focus, and fade in and out as one: every
// surface draws at the same `reveal`, which starts to rise only once all of
// them are on screen.
class Overlays : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal reveal READ reveal NOTIFY revealChanged)

public:
    Overlays(QQmlEngine *engine, Backend *backend, Hyprland *hyprland, QObject *parent = nullptr);
    ~Overlays() override;

    QList<QQuickView *> views() const { return m_views.values(); }
    QList<QQuickView *> underViews() const { return m_under.values(); }
    qreal reveal() const { return m_reveal; }

signals:
    void revealChanged();

private:
    void sync();
    void clear();
    QQuickView *create(QScreen *screen, bool under);
    void fadeTo(qreal target);
    void fadeInWhenShown();

    QQmlEngine *m_engine;
    Backend *m_backend;
    Hyprland *m_hyprland;
    QHash<QScreen *, QQuickView *> m_views;
    QHash<QScreen *, QQuickView *> m_under;
    bool m_blurring = false;
    qreal m_reveal = 0;
    QVariantAnimation m_fade;
    QTimer m_shownTimeout;
    bool m_wanted = false;
};
