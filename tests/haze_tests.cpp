#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include "backend.h"
#include "control.h"
#include "hyprland.h"
#include "overlays.h"
#include "theme.h"
#include "trayicon.h"
#include "translator.h"

namespace {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                if ((word != "qsTr" && word != "tr") || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}


// A Hyprland that answers requests from a table, remembers what it was asked,
// and has an event socket to announce things on.
class FakeHyprland : public QObject {
public:
    QTemporaryDir dir;
    QLocalServer requests, events;
    QHash<QString, QByteArray> replies;
    QStringList received;
    QLocalSocket *listener = nullptr;

    FakeHyprland() {
        replies.insert("j/activewindow", "{}");
        replies.insert("j/monitors", "[]");
        requests.listen(dir.filePath("requests.sock"));
        events.listen(dir.filePath("events.sock"));
        connect(&requests, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket *client = requests.nextPendingConnection()) {
                connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
                auto answer = [this, client] {
                    if (!client->bytesAvailable())
                        return;
                    const QString command = QString::fromUtf8(client->readAll());
                    received << command;
                    client->write(replies.value(command, "ok"));
                    client->flush();
                    client->disconnectFromServer();
                };
                connect(client, &QLocalSocket::readyRead, this, answer);
                answer();
            }
        });
        connect(&events, &QLocalServer::newConnection, this, [this] { listener = events.nextPendingConnection(); });
    }

    QString requestSocket() const { return requests.fullServerName(); }
    QString eventSocket() const { return events.fullServerName(); }

    void setMonitors(const QByteArray &json) { replies.insert("j/monitors", json); }
    void focus(int x, int y, int width, int height, int monitor) {
        replies.insert("j/activewindow", QString(R"({"at": [%1, %2], "size": [%3, %4], "monitor": %5, "mapped": true, "hidden": false})")
                                             .arg(x).arg(y).arg(width).arg(height).arg(monitor).toUtf8());
    }
    void announce(const QByteArray &event) {
        listener->write(event + "\n");
        listener->flush();
    }
    int count(const QString &command) const { return received.count(command); }
};

} // namespace

class HazeTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        // A Hyprland made without sockets must find none, whatever desktop
        // the tests run on.
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    }

    void settingsStartWithDefaults() {
        QTemporaryDir dir;
        Backend backend(nullptr, dir.filePath("haze/settings.json"));
        QVERIFY(backend.enabled());
        QVERIFY(backend.blur());
        QVERIFY(backend.customBlur());
        QCOMPARE(backend.tint(), QString("theme"));
        QCOMPARE(backend.dim(), .25);
    }

    void settingsSurviveARestart() {
        QTemporaryDir dir;
        const QString file = dir.filePath("haze/settings.json");
        {
            Backend backend(nullptr, file);
            backend.setEnabled(false);
            backend.setDim(.5);
            backend.setTint("accent");
            backend.setBlur(false);
            backend.setCustomBlur(false);
            backend.setStrength(8);
            backend.setVibrancy(.7);
            backend.setGrain(.3);
        }
        Backend backend(nullptr, file);
        QVERIFY(!backend.enabled());
        QCOMPARE(backend.dim(), .5);
        QCOMPARE(backend.tint(), QString("accent"));
        QVERIFY(!backend.blur());
        QVERIFY(!backend.customBlur());
        QCOMPARE(backend.strength(), 8);
        QCOMPARE(backend.vibrancy(), .7);
        QCOMPARE(backend.grain(), .3);
    }

    void settingsStayInRange() {
        QTemporaryDir dir;
        const QString file = dir.filePath("settings.json");
        QVERIFY(write(file, R"({"dim": 4, "tint": "plaid", "strength": 99, "vibrancy": -1})"));
        Backend backend(nullptr, file);
        QCOMPARE(backend.dim(), .9);
        QCOMPARE(backend.tint(), QString("theme"));
        QCOMPARE(backend.strength(), 10);
        QCOMPARE(backend.vibrancy(), 0.);

        backend.setDim(-1);
        QCOMPARE(backend.dim(), 0.);
        backend.setStrength(0);
        QCOMPARE(backend.strength(), 1);
        backend.setTint("plaid");
        QCOMPARE(backend.tint(), QString("theme"));
    }

    void brokenSettingsFallBack() {
        QTemporaryDir dir;
        const QString file = dir.filePath("settings.json");
        QVERIFY(write(file, "not json"));
        Backend backend(nullptr, file);
        QVERIFY(backend.enabled());
        QCOMPARE(backend.dim(), .25);
    }

    void tintsCycle() {
        QTemporaryDir dir;
        Backend backend(nullptr, dir.filePath("settings.json"));
        QStringList seen;
        for (int i = 0; i < backend.tints().size(); ++i) {
            seen << backend.tint();
            backend.nextTint();
        }
        QCOMPARE(seen, backend.tints());
        QCOMPARE(backend.tint(), QString("theme"));
    }

    void strongerBlurReachesFurther() {
        QVERIFY(Backend::blurLua(true, 1, .35, 0).contains("size = 3, passes = 2, vibrancy = 0.350, noise = 0.0000"));
        QVERIFY(Backend::blurLua(true, 5, 1, 1).contains("size = 7, passes = 3, vibrancy = 1.000, noise = 0.1000"));
        QVERIFY(Backend::blurLua(true, 10, 0, .5).contains("size = 12, passes = 4, vibrancy = 0.000, noise = 0.0500"));
        // The desktop's own blur is remembered before it is replaced, and a
        // desktop with its blur turned off gets it switched on meanwhile.
        for (bool custom : {true, false}) {
            QVERIFY(Backend::blurLua(custom, 5, 0, 0).startsWith("haze_saved = haze_saved or"));
            QVERIFY(Backend::blurLua(custom, 5, 0, 0).contains("enabled = true"));
        }
        QVERIFY(Backend::blurLua(false, 5, 0, 0).contains("size = haze_saved.size"));
    }

    void hyprlandFindsTheFocusedWindow() {
        FakeHyprland fake;
        fake.setMonitors(R"([{"id": 0, "name": "DP-1", "x": 0, "y": 0, "reserved": [0, 44, 0, 0]},
                             {"id": 1, "name": "DP-2", "x": 1920, "y": 0, "reserved": [10, 0, 0, 0]}])");
        fake.focus(2000, 100, 800, 600, 1);

        Hyprland hyprland(fake.requestSocket(), fake.eventSocket());
        QVERIFY(hyprland.available());
        QTRY_VERIFY(hyprland.hasActiveWindow());
        // Measured from where the monitor's usable area starts.
        QTRY_COMPARE(hyprland.holes().value("DP-2").toRectF(), QRectF(70, 100, 800, 600));
        QVERIFY(!hyprland.holes().contains("DP-1"));
        // What each monitor's bars reserve.
        QCOMPARE(hyprland.reserved().value("DP-1").toMap().value("top").toDouble(), 44.);
        QCOMPARE(hyprland.reserved().value("DP-2").toMap().value("left").toDouble(), 10.);
        QCOMPARE(hyprland.reserved().value("DP-2").toMap().value("top").toDouble(), 0.);

        // Focus moves to the other monitor, under its bar.
        QTRY_VERIFY(fake.listener);
        fake.focus(100, 144, 500, 400, 0);
        fake.announce("activewindowv2>>55d4");
        QTRY_COMPARE(hyprland.holes().value("DP-1").toRectF(), QRectF(100, 100, 500, 400));
        QVERIFY(!hyprland.holes().contains("DP-2"));

        // An empty workspace has no focused window.
        fake.replies.insert("j/activewindow", "{}");
        fake.announce("workspacev2>>3,3");
        QTRY_VERIFY(!hyprland.hasActiveWindow());
        QVERIFY(hyprland.holes().isEmpty());
    }

    void hyprlandFollowsAWindowBeingDragged() {
        // Hyprland announces nothing while the mouse moves a window, so a
        // watched Hyprland asks again by itself.
        FakeHyprland fake;
        fake.setMonitors(R"([{"id": 0, "name": "DP-1", "x": 0, "y": 0, "reserved": [0, 0, 0, 0]}])");
        fake.focus(0, 0, 400, 300, 0);
        Hyprland hyprland(fake.requestSocket(), fake.eventSocket());
        QTRY_VERIFY(hyprland.hasActiveWindow());

        fake.focus(50, 60, 400, 300, 0);
        QTest::qWait(300);
        QCOMPARE(hyprland.holes().value("DP-1").toRectF(), QRectF(0, 0, 400, 300));
        hyprland.setWatching(true);
        QTRY_COMPARE(hyprland.holes().value("DP-1").toRectF(), QRectF(50, 60, 400, 300));

        hyprland.setWatching(false);
        const int asked = fake.count("j/activewindow");
        QTest::qWait(300);
        QCOMPARE(fake.count("j/activewindow"), asked);
    }

    void hyprlandReadsItsOptions() {
        FakeHyprland fake;
        fake.replies.insert("j/getoption general:border_size", R"({"option": "general:border_size", "int": 3, "set": true})");
        Hyprland hyprland(fake.requestSocket(), fake.eventSocket());
        QTRY_COMPARE(hyprland.borderSize(), 3);

        // A reload may have changed them.
        QTRY_VERIFY(fake.listener);
        fake.replies.insert("j/getoption general:border_size", R"({"option": "general:border_size", "int": 1, "set": true})");
        QSignalSpy reloaded(&hyprland, &Hyprland::configReloaded);
        fake.announce("configreloaded>>");
        QTRY_COMPARE(hyprland.borderSize(), 1);
        QCOMPARE(reloaded.count(), 1);
    }

    void withoutHyprlandNothingIsAsked() {
        Hyprland hyprland;
        QVERIFY(!hyprland.available());
        QVERIFY(!hyprland.hasActiveWindow());
        bool answered = false;
        hyprland.request("j/monitors", [&](const QByteArray &reply) { answered = reply.isEmpty(); });
        QVERIFY(answered);
        hyprland.evalNow("return 1");
    }

    void aHyprlandThatWentAwayAnswersNothing() {
        QTemporaryDir dir;
        Hyprland hyprland(dir.filePath("gone.sock"), dir.filePath("gone2.sock"));
        bool answered = false;
        hyprland.request("j/monitors", [&](const QByteArray &reply) { answered = reply.isEmpty(); });
        QTRY_VERIFY(answered);
        QVERIFY(!hyprland.hasActiveWindow());
    }

    void backendGivesHyprlandTheRuleAndTheBlur() {
        FakeHyprland fake;
        QTemporaryDir dir;
        Hyprland hyprland(fake.requestSocket(), fake.eventSocket());
        Backend backend(&hyprland, dir.filePath("settings.json"));
        const QString rule = "/eval " + Backend::ruleLua(), restore = "/eval " + Backend::restoreLua();
        QTRY_COMPARE(fake.count(rule), 1);
        QTRY_COMPARE(fake.count("/eval " + Backend::blurLua(true, 5, backend.vibrancy(), backend.grain())), 1);

        backend.setStrength(7);
        const QString blur = "/eval " + Backend::blurLua(true, 7, backend.vibrancy(), backend.grain());
        QTRY_COMPARE(fake.count(blur), 1);

        // The desktop's own look, still switched on.
        backend.setCustomBlur(false);
        QTRY_COMPARE(fake.count("/eval " + Backend::blurLua(false, 7, 0, 0)), 1);
        backend.setCustomBlur(true);
        QTRY_COMPARE(fake.count(blur), 2);

        // A reload forgets the rule and the blur, so both are given again.
        QTRY_VERIFY(fake.listener);
        const int rules = fake.count(rule);
        fake.announce("configreloaded>>");
        QTRY_COMPARE(fake.count(rule), rules + 1);
        QTRY_COMPARE(fake.count(blur), 3);

        // Off, the desktop gets its own blur back.
        const int restores = fake.count(restore);
        backend.setEnabled(false);
        QTRY_COMPARE(fake.count(restore), restores + 1);
        // And so does a haze that only dims.
        backend.setEnabled(true);
        QTRY_COMPARE(fake.count(blur), 4);
        backend.setBlur(false);
        QTRY_COMPARE(fake.count(restore), restores + 2);
    }

    void quittingReturnsTheDesktopsBlur() {
        FakeHyprland fake;
        QTemporaryDir dir;
        const QString restore = "/eval " + Backend::restoreLua();
        Hyprland hyprland(fake.requestSocket(), fake.eventSocket());
        Backend backend(&hyprland, dir.filePath("settings.json"));
        QTRY_COMPARE(fake.count("/eval " + Backend::blurLua(true, backend.strength(), backend.vibrancy(), backend.grain())), 1);
        const int restores = fake.count(restore);

        // The last request is sent without an event loop; the fake needs one
        // to take it, so shutdown runs on another thread here.
        QThread *thread = QThread::create([&] { hyprland.evalNow(Backend::restoreLua()); });
        thread->start();
        QTRY_COMPARE(fake.count(restore), restores + 1);
        QVERIFY(thread->wait(5000));
        delete thread;
    }

    void aSecondHazeHandsItsCommandToTheFirst() {
        QTemporaryDir dir;
        const QString path = dir.filePath("haze.sock");
        QString reply;
        QVERIFY(!Control::send(path, "toggle", &reply));

        Control control(path);
        QStringList commands;
        control.handler = [&](const QString &command) {
            commands << command;
            return QString("Haze is on");
        };
        QVERIFY(control.listen());

        bool sent = false;
        QThread *thread = QThread::create([&] { sent = Control::send(path, "toggle", &reply); });
        thread->start();
        QTRY_VERIFY(thread->isFinished());
        delete thread;
        QVERIFY(sent);
        QCOMPARE(commands, QStringList{"toggle"});
        QCOMPARE(reply, QString("Haze is on"));
    }

    void aDeadHazesSocketIsTakenOver() {
        QTemporaryDir dir;
        const QString path = dir.filePath("haze.sock");
        QVERIFY(write(path, "stale"));
        Control control(path);
        QVERIFY(control.listen());
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }


    void hazeCoversEveryScreenButTheFocusedWindow() {
        const QString screen = QGuiApplication::primaryScreen()->name();
        FakeHyprland fake;
        fake.setMonitors(QString(R"([{"id": 0, "name": "%1", "x": 0, "y": 0, "reserved": [0, 40, 0, 0]}])").arg(screen).toUtf8());
        fake.replies.insert("j/getoption general:border_size", R"({"option": "general:border_size", "int": 2, "set": true})");

        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        Hyprland hyprland(fake.requestSocket(), fake.eventSocket());
        Backend backend(&hyprland, dir.filePath("settings.json"));

        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("hyprland", &hyprland);
        engine.rootContext()->setContextProperty("backend", &backend);

        // Nothing has focus yet, so there is nothing to set apart.
        Overlays overlays(&engine, &backend, &hyprland);
        QTRY_COMPARE(hyprland.borderSize(), 2);
        QVERIFY(overlays.views().isEmpty());

        QTRY_VERIFY(fake.listener);
        fake.focus(100, 140, 800, 600, 0);
        fake.announce("activewindowv2>>55d4");
        QTRY_COMPARE(overlays.views().size(), QGuiApplication::screens().size());
        QQuickView *view = overlays.views().first();
        QVERIFY(view->flags().testFlag(Qt::WindowTransparentForInput));
        QCOMPARE(view->color(), QColor(Qt::transparent));

        // Every surface fades in by the one reveal, so they move as one.
        auto *reveal = engine.rootContext()->contextProperty("haze").value<QObject *>();
        QCOMPARE(reveal, &overlays);
        QTRY_COMPARE(overlays.reveal(), 1.);

        // The hole is the window and its border, under the bar's 40 pixels.
        QQuickItem *haze = view->rootObject();
        QVERIFY(haze);
        QCOMPARE(haze->property("holeX").toReal(), 98.);
        QCOMPARE(haze->property("holeY").toReal(), 98.);
        QCOMPARE(haze->property("holeWidth").toReal(), 804.);
        QCOMPARE(haze->property("holeHeight").toReal(), 604.);

        // Under the bar lies a second haze, the height of what the bar
        // reserves, in the same tint, so the bar's background matches.
        QCOMPARE(overlays.underViews().size(), QGuiApplication::screens().size());
        QQuickItem *below = overlays.underViews().first()->rootObject();
        QVERIFY(below->property("under").toBool());
        QVERIFY(!haze->property("under").toBool());
        QCOMPARE(below->findChild<QQuickItem *>("topEdge")->height(), 40.);
        QCOMPARE(below->findChild<QQuickItem *>("topEdge")->width(), below->width());
        QCOMPARE(below->property("fill").value<QColor>(), haze->property("fill").value<QColor>());

        // It follows the focus to the next window.
        fake.focus(300, 240, 400, 300, 0);
        fake.announce("activewindowv2>>66e5");
        QTRY_COMPARE(haze->property("holeX").toReal(), 298.);
        QTRY_COMPARE(haze->property("holeWidth").toReal(), 404.);

        // A haze that blurs keeps a trace of tint, or Hyprland would skip it.
        backend.setDim(0);
        QVERIFY(haze->property("strength").toReal() > 0);
        backend.setDim(.5);
        QCOMPARE(haze->property("strength").toReal(), .5);
        QCOMPARE(haze->property("tint").value<QColor>(), theme.background());
        backend.setTint("accent");
        QCOMPARE(haze->property("tint").value<QColor>(), theme.accent());

        // Turning the blur off makes plain layers in their place.
        backend.setBlur(false);
        QCOMPARE(overlays.views().size(), QGuiApplication::screens().size());
        QCOMPARE(overlays.views().first()->rootObject()->property("strength").toReal(), 0.);
        QTRY_COMPARE(overlays.reveal(), 1.);
        QCOMPARE(overlays.views().first()->rootObject()->property("strength").toReal(),
                 overlays.underViews().first()->rootObject()->property("strength").toReal());

        // Off, they fade out together and then go; turned back on midway,
        // the same ones fade back in.
        backend.setEnabled(false);
        QCOMPARE(overlays.views().size(), QGuiApplication::screens().size());
        QTRY_VERIFY(overlays.reveal() < 1);
        backend.setEnabled(true);
        QTRY_COMPARE(overlays.reveal(), 1.);
        QCOMPARE(overlays.views().size(), QGuiApplication::screens().size());
        backend.setEnabled(false);
        QTRY_VERIFY(overlays.views().isEmpty());
        QVERIFY(overlays.underViews().isEmpty());
        QCOMPARE(overlays.reveal(), 0.);
        backend.setEnabled(true);
        QCOMPARE(overlays.views().size(), QGuiApplication::screens().size());
        QTRY_COMPARE(overlays.reveal(), 1.);

        // Surfaces made and thrown away in quick succession, with frames of
        // the old ones still being announced, leave nothing dangling.
        for (int i = 0; i < 30; ++i) {
            backend.setBlur(i % 2);
            QTest::qWait(i % 7);
        }
        backend.setEnabled(false);
        backend.setEnabled(true);
        QTRY_COMPARE(overlays.reveal(), 1.);

        // No window in focus, no haze.
        fake.replies.insert("j/activewindow", "{}");
        fake.announce("workspacev2>>3,3");
        QTRY_VERIFY(overlays.views().isEmpty());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void statusBarIconIsDrawnInTheGivenColor() {
        TrayIconProvider provider;
        QSize size;
        const QImage on = provider.requestPixmap("on/#ff0000", &size, QSize(48, 48)).toImage();
        QCOMPARE(size, QSize(48, 48));
        // The window in focus is filled in, in the color asked for, and the
        // corners stay clear.
        QCOMPARE(on.pixelColor(28, 27), QColor("#ff0000"));
        QCOMPARE(on.pixelColor(0, 0).alpha(), 0);
        // Off, it is only an outline.
        const QImage off = provider.requestPixmap("off/#ff0000", &size, QSize(48, 48)).toImage();
        QCOMPARE(off.pixelColor(28, 27).alpha(), 0);
        QCOMPARE(off.pixelColor(28, 18), QColor("#ff0000"));
        // A color that makes no sense still gives an icon.
        QVERIFY(provider.requestPixmap("on/plaid", &size, QSize()).toImage().pixelColor(56, 54).alpha() == 255);
    }

    void statusBarIconTurnsTheHazeOnAndOff() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Hyprland hyprland;
        Backend backend(&hyprland, dir.filePath("settings.json"));

        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        Theme theme(dir.path());
        engine.addImageProvider("haze", new TrayIconProvider);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl("qrc:/Tray.qml"));
        QScopedPointer<QObject> tray(component.create());
        QVERIFY2(tray, qPrintable(component.errorString()));

        // The icon and its tooltip say which way the haze is.
        QCOMPARE(tray->property("tooltip").toString(), QString("Haze is on"));
        auto *toggle = tray->findChild<QObject *>("trayToggle");
        QCOMPARE(toggle->property("text").toString(), QString("Turn the haze off"));

        // A click on the icon is the main action.
        QQmlExpression click(qmlContext(tray.data()), tray.data(), "activated(SystemTrayIcon.Trigger)");
        click.evaluate();
        QVERIFY2(!click.hasError(), qPrintable(click.error().toString()));
        QVERIFY(!backend.enabled());
        QCOMPARE(tray->property("tooltip").toString(), QString("Haze is off"));
        QCOMPARE(QQmlExpression(qmlContext(tray.data()), tray.data(), "icon.source").evaluate().toString(),
                 QString("image://haze/off/#e6e6e9"));
        QCOMPARE(toggle->property("text").toString(), QString("Turn the haze on"));

        // The menu does the same, and has the blur and the window.
        QVERIFY(QMetaObject::invokeMethod(toggle, "triggered"));
        QVERIFY(backend.enabled());
        auto *blur = tray->findChild<QObject *>("trayBlur");
        QVERIFY(blur->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(blur, "triggered"));
        QVERIFY(!backend.blur());
        QVERIFY(!blur->property("checked").toBool());

        QSignalSpy settings(&backend, &Backend::settingsRequested);
        QVERIFY(QMetaObject::invokeMethod(tray->findChild<QObject *>("traySettings"), "triggered"));
        QCOMPARE(settings.count(), 1);
        QQmlExpression middle(qmlContext(tray.data()), tray.data(), "activated(SystemTrayIcon.MiddleClick)");
        middle.evaluate();
        QCOMPARE(settings.count(), 2);

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }

    void windowLoadsAndAnswersTheKeyboard() {
        // A temp language, theme, and settings file, so the test never
        // depends on the desktop's, and no Hyprland.
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        Hyprland hyprland;
        Backend backend(&hyprland, dir.filePath("settings.json"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("hyprland", &hyprland);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        // Space is the main action: the haze, on and off.
        auto *state = window->findChild<QObject *>("state");
        auto *enabled = window->findChild<QObject *>("enabled");
        QCOMPARE(state->property("text").toString(), QString("Haze is on"));
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(!backend.enabled());
        QCOMPARE(state->property("text").toString(), QString("Haze is off"));
        QVERIFY(!enabled->property("checked").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(backend.enabled());
        QVERIFY(enabled->property("checked").toBool());

        // The sliders show the settings and follow the keys.
        auto *dim = window->findChild<QObject *>("dim");
        QCOMPARE(dim->property("value").toDouble(), .25);
        QTest::keyClick(window, Qt::Key_BracketRight);
        QCOMPARE(backend.dim(), .3);
        QCOMPARE(dim->property("value").toDouble(), .3);
        QTest::keyClick(window, Qt::Key_BracketLeft);
        QCOMPARE(backend.dim(), .25);

        QTest::keyClick(window, Qt::Key_T);
        QCOMPARE(backend.tint(), QString("black"));
        QTest::keyClick(window, Qt::Key_B);
        QVERIFY(!backend.blur());
        QVERIFY(!window->findChild<QObject *>("blur")->property("checked").toBool());
        QTest::keyClick(window, Qt::Key_B);
        QTest::keyClick(window, Qt::Key_C);
        QVERIFY(!backend.customBlur());
        QVERIFY(!window->findChild<QObject *>("customBlur")->property("checked").toBool());
        QTest::keyClick(window, Qt::Key_C);
        QTest::keyClick(window, Qt::Key_Equal);
        QCOMPARE(backend.strength(), 6);
        QCOMPARE(window->findChild<QObject *>("strength")->property("value").toInt(), 6);
        QTest::keyClick(window, Qt::Key_Minus);
        QCOMPARE(backend.strength(), 5);

        // What is wrong is said: there is no Hyprland here.
        QVERIFY(window->findChild<QObject *>("noHyprland")->property("visible").toBool());

        auto *help = window->findChild<QObject *>("help");
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->property("opened").toBool());
        // A Material popup animates out: `opened` drops at once, `visible` after.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());
        QVERIFY(window->isVisible());

        // Escape closes the window, and the haze stays on.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->isVisible());
        QVERIFY(backend.enabled());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(HazeTests)
#include "haze_tests.moc"
