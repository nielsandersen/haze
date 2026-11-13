// haze — blurs and dims everything but the window you are working in.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTextStream>
#include <QUrl>

#include "backend.h"
#include "control.h"
#include "hyprland.h"
#include "overlays.h"
#include "theme.h"
#include "trayicon.h"
#include "translator.h"

namespace {

const char *version = "0.1.0";

const char *usage = "Usage: haze [command]\n"
                    "\n"
                    "  open      Show the settings window (the default)\n"
                    "  start     Start Haze without a window, as it was left (for login)\n"
                    "  on        Turn the haze on, starting Haze if needed\n"
                    "  off       Turn the haze off\n"
                    "  toggle    Turn the haze on or off\n"
                    "  quit      Turn the haze off and stop Haze\n"
                    "\n"
                    "  --help, --version\n";

} // namespace

int main(int argc, char *argv[]) {
    QTextStream out(stdout), err(stderr);
    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : "open";
    if (command == "--help" || command == "-h") {
        out << usage;
        return 0;
    }
    if (command == "--version") {
        out << "haze " << version << "\n";
        return 0;
    }
    if (!QStringList{"open", "start", "on", "off", "toggle", "quit"}.contains(command)) {
        err << "haze: unknown command '" << command << "'\n\n" << usage;
        return 1;
    }

    // A Haze already running takes the command, and this one is done. Asking
    // needs no display and no event loop, so it comes before the application
    // exists.
    {
        QString reply;
        if (Control::send(Control::defaultPath(), command, &reply)) {
            out << reply << "\n";
            return 0;
        }
        if (command == "off" || command == "quit") {
            out << "Haze is not running\n";
            return 0;
        }
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("haze");
    app.setApplicationVersion(version);
    // The haze outlives its settings window.
    app.setQuitOnLastWindowClosed(false);

    // Matches the window to haze.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("haze");
    app.setWindowIcon(QIcon::fromTheme("haze", QIcon(":/haze.svg")));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Hyprland hyprland;
    Backend backend(&hyprland);
    if (command == "on" || command == "toggle")
        backend.setEnabled(command == "on" || !backend.enabled());

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("hyprland", &hyprland);
    engine.rootContext()->setContextProperty("backend", &backend);

    Overlays overlays(&engine, &backend, &hyprland);

    // The icon in the status bar, on or off, for as long as Haze runs.
    engine.addImageProvider("haze", new TrayIconProvider);
    engine.load(QUrl("qrc:/Tray.qml"));

    // The window is made the first time it is asked for, so `haze on` at
    // login costs no window.
    auto showWindow = [&engine] {
        auto find = [&engine]() -> QQuickWindow * {
            for (QObject *root : engine.rootObjects())
                if (auto *window = qobject_cast<QQuickWindow *>(root))
                    return window;
            return nullptr;
        };
        QQuickWindow *window = find();
        if (!window) {
            engine.load(QUrl("qrc:/Main.qml"));
            window = find();
        }
        if (!window)
            return false;
        window->show();
        window->requestActivate();
        return true;
    };

    QObject::connect(&backend, &Backend::settingsRequested, &app, showWindow);

    Control control;
    control.handler = [&](const QString &request) {
        if (request == "open")
            showWindow();
        else if (request == "on")
            backend.setEnabled(true);
        else if (request == "off")
            backend.setEnabled(false);
        else if (request == "toggle")
            backend.toggle();
        else if (request == "quit") {
            // After the answer is on its way.
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            return QString("Haze stopped");
        }
        return QString(backend.enabled() ? "Haze is on" : "Haze is off");
    };
    if (!control.listen())
        err << "haze: can't listen on " << Control::defaultPath() << ", so other haze commands won't reach this one\n";

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &backend, &Backend::shutdown);

    if (command == "open" && !showWindow())
        return 1;
    return app.exec();
}
