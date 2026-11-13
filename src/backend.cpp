#include "backend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

#include "hyprland.h"

Backend::Backend(Hyprland *hyprland, const QString &settingsFile, QObject *parent, const QString &autostartFile)
    : QObject(parent), m_hyprland(hyprland),
      m_settingsFile(settingsFile.isEmpty()
                         ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                               + "/haze/settings.json"
                         : settingsFile),
      m_autostartFile(!autostartFile.isEmpty() ? autostartFile
                      : settingsFile.isEmpty()
                          ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                + "/autostart/haze.desktop"
                          : QFileInfo(settingsFile).absolutePath() + "/autostart/haze.desktop") {
    // A slider being dragged changes a setting many times a second; it is
    // written, and passed to Hyprland, once it rests.
    m_saveLater.setSingleShot(true);
    m_saveLater.setInterval(400);
    connect(&m_saveLater, &QTimer::timeout, this, &Backend::save);
    m_syncLater.setSingleShot(true);
    m_syncLater.setInterval(40);
    connect(&m_syncLater, &QTimer::timeout, this, &Backend::syncHyprland);

    load();
    if (m_hyprland) {
        // A reload forgets the rule and returns the blur to the desktop's own.
        connect(m_hyprland, &Hyprland::configReloaded, this, &Backend::syncHyprland);
        syncHyprland();
    }
}

Backend::~Backend() {
    if (m_saveLater.isActive())
        save();
}

void Backend::load() {
    QFile file(m_settingsFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    m_enabled = o.value("enabled").toBool(m_enabled);
    m_dim = std::clamp(o.value("dim").toDouble(m_dim), 0., .9);
    if (tints().contains(o.value("tint").toString()))
        m_tint = o.value("tint").toString();
    m_blur = o.value("blur").toBool(m_blur);
    m_customBlur = o.value("customBlur").toBool(m_customBlur);
    m_strength = std::clamp(o.value("strength").toInt(m_strength), 1, 10);
    m_vibrancy = std::clamp(o.value("vibrancy").toDouble(m_vibrancy), 0., 1.);
    m_grain = std::clamp(o.value("grain").toDouble(m_grain), 0., 1.);
}

bool Backend::save() {
    m_saveLater.stop();
    if (!QDir().mkpath(QFileInfo(m_settingsFile).absolutePath()))
        return false;
    const QJsonObject o{
        {"enabled", m_enabled}, {"dim", m_dim},           {"tint", m_tint},         {"blur", m_blur},
        {"customBlur", m_customBlur}, {"strength", m_strength}, {"vibrancy", m_vibrancy}, {"grain", m_grain},
    };
    // Whole or not at all, so a crash mid-write leaves the old settings.
    QSaveFile file(m_settingsFile);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(o).toJson());
    return file.commit();
}

void Backend::touch() {
    emit changed();
    m_saveLater.start();
    m_syncLater.start();
}

void Backend::setEnabled(bool enabled) {
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    touch();
}

void Backend::setDim(double dim) {
    dim = std::clamp(dim, 0., .9);
    if (qFuzzyCompare(dim + 1, m_dim + 1))
        return;
    m_dim = dim;
    touch();
}

void Backend::setTint(const QString &tint) {
    if (tint == m_tint || !tints().contains(tint))
        return;
    m_tint = tint;
    touch();
}

void Backend::nextTint() {
    const QStringList all = tints();
    setTint(all.at((all.indexOf(m_tint) + 1) % all.size()));
}

void Backend::setBlur(bool blur) {
    if (blur == m_blur)
        return;
    m_blur = blur;
    touch();
}

void Backend::setCustomBlur(bool custom) {
    if (custom == m_customBlur)
        return;
    m_customBlur = custom;
    touch();
}

void Backend::setStrength(int strength) {
    strength = std::clamp(strength, 1, 10);
    if (strength == m_strength)
        return;
    m_strength = strength;
    touch();
}

void Backend::setVibrancy(double vibrancy) {
    vibrancy = std::clamp(vibrancy, 0., 1.);
    if (qFuzzyCompare(vibrancy + 1, m_vibrancy + 1))
        return;
    m_vibrancy = vibrancy;
    touch();
}

void Backend::setGrain(double grain) {
    grain = std::clamp(grain, 0., 1.);
    if (qFuzzyCompare(grain + 1, m_grain + 1))
        return;
    m_grain = grain;
    touch();
}

// Hyprland blurs what is behind a layer only where the layer is not fully
// see-through, which is what leaves the focused window sharp: the haze is
// drawn everywhere but over it.
QString Backend::ruleLua() {
    // Haze fades its surfaces itself, all as one, so Hyprland adds no motion
    // of its own to each.
    return "hl.layer_rule({ match = { namespace = \"^haze$\" }, blur = true, ignore_alpha = 0.005, "
           "no_anim = true, animation = \"none\" }) "
           "hl.layer_rule({ match = { namespace = \"^haze-plain$\" }, no_anim = true, animation = \"none\" })";
}

// Hyprland has one blur for the whole desktop, and a desktop may have it
// turned off, so while the haze is on it is switched on there, with Haze's
// own look or the desktop's, remembering the desktop's settings the first
// time. A config reload starts a fresh Lua state with the desktop's values,
// so nothing stale is kept.
QString Backend::blurLua(bool custom, int strength, double vibrancy, double grain) {
    const QString save = "haze_saved = haze_saved or { enabled = hl.get_config(\"decoration.blur.enabled\"), "
                         "size = hl.get_config(\"decoration.blur.size\"), "
                         "passes = hl.get_config(\"decoration.blur.passes\"), "
                         "vibrancy = hl.get_config(\"decoration.blur.vibrancy\"), "
                         "noise = hl.get_config(\"decoration.blur.noise\") } ";
    if (!custom)
        return save + "hl.config({ decoration = { blur = { enabled = true, size = haze_saved.size, "
                      "passes = haze_saved.passes, vibrancy = haze_saved.vibrancy, noise = haze_saved.noise } } })";

    strength = std::clamp(strength, 1, 10);
    // Each pass doubles the reach of the blur, so passes set the range and
    // the size fills in the steps between.
    const int passes = std::min(4, 2 + (strength - 1) / 3);
    const int size = 2 + strength;
    return save
           + QString("hl.config({ decoration = { blur = { enabled = true, size = %1, passes = %2, vibrancy = %3, "
                     "noise = %4 } } })")
                 .arg(size)
                 .arg(passes)
                 .arg(QString::number(std::clamp(vibrancy, 0., 1.), 'f', 3),
                      QString::number(std::clamp(grain, 0., 1.) * .1, 'f', 4));
}

QString Backend::restoreLua() {
    return "if haze_saved then hl.config({ decoration = { blur = haze_saved } }) haze_saved = nil end";
}

void Backend::syncHyprland() {
    m_syncLater.stop();
    if (!m_hyprland)
        return;
    m_hyprland->eval(ruleLua());
    m_hyprland->eval(m_enabled && m_blur ? blurLua(m_customBlur, m_strength, m_vibrancy, m_grain) : restoreLua());
}

void Backend::shutdown() {
    save();
    if (m_hyprland)
        m_hyprland->evalNow(restoreLua());
}

bool Backend::startAtLogin() const {
    return QFileInfo::exists(m_autostartFile);
}

void Backend::setStartAtLogin(bool start) {
    if (start == startAtLogin() && !m_startAtLoginFailed)
        return;
    bool done;
    if (start) {
        // `haze start` brings the haze back as it was left, on or off, with
        // no window.
        QSaveFile file(m_autostartFile);
        file.setDirectWriteFallback(false);
        done = QDir().mkpath(QFileInfo(m_autostartFile).absolutePath()) && file.open(QIODevice::WriteOnly)
               && file.write("[Desktop Entry]\nType=Application\nName=Haze\nExec=haze start\nIcon=haze\n") > 0
               && file.commit();
    } else {
        done = !QFileInfo::exists(m_autostartFile) || QFile::remove(m_autostartFile);
    }
    m_startAtLoginFailed = !done;
    emit changed();
}
