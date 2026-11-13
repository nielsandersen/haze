#pragma once

#include <QQuickImageProvider>

// Draws the status bar icon in a given color: "image://haze/on/#e6e6e9".
// A bar gives an icon its own color only when it can find it by a symbolic
// name in the icon theme, and a bar already running doesn't see icons
// installed after it started. So Haze hands over the picture itself, in the
// theme's foreground, and draws it again when the theme changes.
class TrayIconProvider : public QQuickImageProvider {
public:
    TrayIconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

    QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) override;
};
