#include "overlays.h"

#include <LayerShellQt/Window>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QScreen>

#include "backend.h"
#include "hyprland.h"

Overlays::Overlays(QQmlEngine *engine, Backend *backend, Hyprland *hyprland, QObject *parent)
    : QObject(parent), m_engine(engine), m_backend(backend), m_hyprland(hyprland) {
    // Every surface reads the one reveal, as `haze.reveal`.
    engine->rootContext()->setContextProperty("haze", this);

    m_fade.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_reveal = value.toReal();
        emit revealChanged();
    });
    // Faded out, the surfaces go.
    connect(&m_fade, &QVariantAnimation::finished, this, [this] {
        if (qFuzzyIsNull(m_reveal))
            clear();
    });
    // A surface that never reports a frame must not hold the others back.
    m_shownTimeout.setSingleShot(true);
    m_shownTimeout.setInterval(250);
    connect(&m_shownTimeout, &QTimer::timeout, this, [this] { fadeTo(1); });

    connect(backend, &Backend::changed, this, &Overlays::sync);
    connect(hyprland, &Hyprland::changed, this, &Overlays::sync);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &Overlays::sync);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        delete m_views.take(screen);
        delete m_under.take(screen);
    });
    sync();
}

Overlays::~Overlays() {
    m_fade.disconnect(this);
    clear();
    m_engine->rootContext()->setContextProperty("haze", nullptr);
}

void Overlays::clear() {
    m_fade.stop();
    m_shownTimeout.stop();
    qDeleteAll(m_views);
    m_views.clear();
    qDeleteAll(m_under);
    m_under.clear();
    if (!qFuzzyIsNull(m_reveal)) {
        m_reveal = 0;
        emit revealChanged();
    }
}

void Overlays::fadeTo(qreal target) {
    if (m_fade.state() == QAbstractAnimation::Running && qFuzzyCompare(m_fade.endValue().toReal() + 1, target + 1))
        return;
    m_fade.stop();
    if (qFuzzyCompare(m_reveal + 1, target + 1)) {
        if (qFuzzyIsNull(target))
            clear();
        return;
    }
    m_fade.setStartValue(m_reveal);
    m_fade.setEndValue(target);
    // A fade turned around midway takes only the time of what is left.
    m_fade.setDuration(qMax(1, int(180 * qAbs(target - m_reveal))));
    m_fade.start();
}

// The surfaces reach the screen one after the other. Fading each in as it
// arrives shows as separate motions, so the fade waits for the last of them.
void Overlays::fadeInWhenShown() {
    const QList<QQuickView *> all = m_views.values() + m_under.values();
    for (QQuickView *view : all) {
        if (!view->property("hazeShown").toBool()) {
            m_shownTimeout.start();
            return;
        }
    }
    m_shownTimeout.stop();
    fadeTo(1);
}

void Overlays::sync() {
    // With no window in focus there is nothing to set apart, and a blurred
    // wallpaper would only look broken.
    const bool wanted = m_backend->enabled() && m_hyprland->hasActiveWindow();
    m_hyprland->setWatching(wanted);

    m_wanted = wanted;
    if (!wanted) {
        m_shownTimeout.stop();
        fadeTo(0);
        return;
    }

    // Hyprland's blur rule goes by the layer's name, and a layer can't be
    // renamed, so switching the blur on or off makes them anew.
    if (m_backend->blur() != m_blurring)
        clear();
    m_blurring = m_backend->blur();

    for (QScreen *screen : QGuiApplication::screens()) {
        if (!m_under.contains(screen))
            m_under.insert(screen, create(screen, true));
        if (!m_views.contains(screen))
            m_views.insert(screen, create(screen, false));
    }
    fadeInWhenShown();
}

QQuickView *Overlays::create(QScreen *screen, bool under) {
    auto *view = new QQuickView(m_engine, nullptr);
    view->setColor(Qt::transparent);
    // Clicks and keys go to whatever is under the haze.
    view->setFlags(view->flags() | Qt::FramelessWindowHint | Qt::WindowTransparentForInput
                   | Qt::WindowDoesNotAcceptFocus);
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->setScreen(screen);

    if (QGuiApplication::platformName().startsWith("wayland")) {
        using Layer = LayerShellQt::Window;
        Layer *layer = Layer::get(view);
        layer->setScope(m_blurring ? "haze" : "haze-plain");
        layer->setScreen(screen);
        layer->setAnchors(Layer::Anchors(Layer::AnchorTop | Layer::AnchorBottom | Layer::AnchorLeft
                                         | Layer::AnchorRight));
        // The haze over the windows keeps off the area the bar reserves (an
        // exclusive zone of zero), or it would blur the bar's clock along
        // with everything else. The one under the bar covers the whole
        // screen (minus one), and lies below the windows.
        layer->setLayer(under ? Layer::LayerBottom : Layer::LayerTop);
        layer->setExclusiveZone(under ? -1 : 0);
        layer->setKeyboardInteractivity(Layer::KeyboardInteractivityNone);
    }

    // Frames are announced from the render thread, so the news arrives later,
    // by which time the surface may be gone: the connection belongs to the
    // view, and goes with it, rather than being left to call into a deleted
    // one.
    connect(view, &QQuickWindow::frameSwapped, view, [this, view] {
        if (view->property("hazeShown").toBool())
            return;
        view->setProperty("hazeShown", true);
        if (m_wanted)
            fadeInWhenShown();
    });

    view->setInitialProperties({{"screenName", screen->name()}, {"under", under}});
    view->setSource(QUrl("qrc:/Overlay.qml"));
    view->show();
    return view;
}
