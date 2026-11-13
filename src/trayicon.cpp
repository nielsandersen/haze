#include "trayicon.h"

#include <QIcon>
#include <QPainter>

QPixmap TrayIconProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) {
    const bool on = id.section('/', 0, 0) == "on";
    QColor color(id.section('/', 1));
    if (!color.isValid())
        color = QColor("#e6e6e9");

    // Large by default, so the bar scales it down rather than up.
    const int side = requestedSize.isValid() ? qMax(requestedSize.width(), requestedSize.height()) : 96;
    QPixmap pixmap = QIcon(on ? ":/haze-symbolic.svg" : ":/haze-off-symbolic.svg").pixmap(QSize(side, side));
    if (!pixmap.isNull()) {
        // The drawing is only a shape: keep its alpha, replace its color.
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
    }
    if (size)
        *size = pixmap.size();
    return pixmap;
}
