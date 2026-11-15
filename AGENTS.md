# Working on Haze

Haze blurs and dims everything on a Hyprland desktop except the focused
window. `README.md` describes what it does for the person using it; this file
is for whoever changes the code.

## Build and test

```bash
bin/build                                 # builds build/haze
bin/test                                  # builds, then runs the tests offscreen
cmake --install build --prefix ~/.local   # installs for this user
```

Run `bin/test` before every commit. The tests load the real `Main.qml` and
`Overlay.qml` offscreen, so a QML error fails them.

Dependencies: Qt 6 (base, declarative, svg, wayland), `layer-shell-qt`, and a
Hyprland configured in Lua.

## Layout

| Path | What it holds |
| --- | --- |
| `src/main.cpp` | Command line (`haze on`, `off`, `toggle`, ...) and the version string |
| `src/backend.*` | Settings: whether the haze is on and how it looks; saved to `~/.config/haze/settings.json` |
| `src/control.*` | Single instance: a second `haze` hands its command to the running one |
| `src/hyprland.*` | What Haze reads from Hyprland (focused window, screens) and the blur it asks for |
| `src/overlays.*` | The haze surfaces: one layer per screen above the windows, one under the bar |
| `src/theme.*` | Follows the Omarchy theme live |
| `src/translator.*`, `src/i18n/` | Follows Omarchy's language; one JSON catalog per language |
| `src/trayicon.*`, `src/Tray.qml` | The status bar icon |
| `src/Main.qml`, `src/Overlay.qml` | The settings window and the haze layer |
| `share/` | Desktop entry and icons |
| `tests/haze_tests.cpp` | Qt tests |
| `packaging/aur/` | `PKGBUILD` and `.SRCINFO` for the `hyprhaze` package |

A new file in `src/` needs no CMake change; the sources are globbed.

## Rules

- Haze never writes to the Hyprland or Omarchy config. It changes the blur at
  run time and gives the desktop its settings back when turned off or quit.
- Every string shown to a person goes through the translator. Add it to both
  `src/i18n/en.json` and `src/i18n/da.json`.
- Colours come from the Omarchy theme, never from literals.
- Keep `README.md` current when a key, command or behaviour changes.

## Releasing

1. Change `version` in `src/main.cpp` and commit as `Bump to X.Y.Z`.
2. Tag `vX.Y.Z` and push the tag.
3. In `packaging/aur/`, set `pkgver`, update `sha256sums` for the new tag's
   archive, and regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.
