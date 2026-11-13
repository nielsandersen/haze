#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class Hyprland;

// Haze's settings: whether it is on, and how the haze looks. They are kept in
// ~/.config/haze/settings.json, and the parts Hyprland draws (the blur behind
// the haze) are passed on to it.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(double dim READ dim WRITE setDim NOTIFY changed)
    Q_PROPERTY(QString tint READ tint WRITE setTint NOTIFY changed)
    Q_PROPERTY(bool blur READ blur WRITE setBlur NOTIFY changed)
    Q_PROPERTY(bool customBlur READ customBlur WRITE setCustomBlur NOTIFY changed)
    Q_PROPERTY(int strength READ strength WRITE setStrength NOTIFY changed)
    Q_PROPERTY(double vibrancy READ vibrancy WRITE setVibrancy NOTIFY changed)
    Q_PROPERTY(double grain READ grain WRITE setGrain NOTIFY changed)
    Q_PROPERTY(QStringList tints READ tints CONSTANT)
    Q_PROPERTY(bool startAtLogin READ startAtLogin WRITE setStartAtLogin NOTIFY changed)
    Q_PROPERTY(bool startAtLoginFailed READ startAtLoginFailed NOTIFY changed)

public:
    // Tests pass a temp settings file; without Hyprland only the settings work.
    // Given a settings file and no autostart file, the autostart file is kept
    // beside it, never in the real ~/.config/autostart.
    explicit Backend(Hyprland *hyprland = nullptr, const QString &settingsFile = {}, QObject *parent = nullptr,
                     const QString &autostartFile = {});
    ~Backend() override;

    bool enabled() const { return m_enabled; }
    double dim() const { return m_dim; }
    QString tint() const { return m_tint; }
    bool blur() const { return m_blur; }
    bool customBlur() const { return m_customBlur; }
    int strength() const { return m_strength; }
    double vibrancy() const { return m_vibrancy; }
    double grain() const { return m_grain; }
    QStringList tints() const { return {"theme", "black", "accent", "white"}; }

    void setEnabled(bool enabled);
    void setDim(double dim);
    void setTint(const QString &tint);
    void setBlur(bool blur);
    void setCustomBlur(bool custom);
    void setStrength(int strength);
    void setVibrancy(double vibrancy);
    void setGrain(double grain);

    // Starting at login is an entry in ~/.config/autostart, which the session
    // runs: there or not is the whole setting.
    bool startAtLogin() const;
    bool startAtLoginFailed() const { return m_startAtLoginFailed; }
    void setStartAtLogin(bool start);

    Q_INVOKABLE void toggle() { setEnabled(!m_enabled); }
    Q_INVOKABLE void nextTint();
    // Asks for the settings window, from wherever there is none to ask in.
    Q_INVOKABLE void openSettings() { emit settingsRequested(); }

    // Writes the settings now, and gives the desktop its own blur back.
    void shutdown();

    // The Lua handed to Hyprland. The layer names are what the rule matches:
    // a haze that blurs is named "haze", one that only dims "haze-plain".
    static QString ruleLua();
    static QString blurLua(bool custom, int strength, double vibrancy, double grain);
    static QString restoreLua();

signals:
    void changed();
    void settingsRequested();

private:
    void load();
    bool save();
    void touch();
    void syncHyprland();

    Hyprland *m_hyprland;
    QString m_settingsFile;
    QString m_autostartFile;
    bool m_startAtLoginFailed = false;
    QTimer m_saveLater;
    QTimer m_syncLater;

    bool m_enabled = true;
    double m_dim = .25;
    QString m_tint = "theme";
    bool m_blur = true;
    bool m_customBlur = true;
    int m_strength = 5;
    double m_vibrancy = .35;
    double m_grain = .1;
};
