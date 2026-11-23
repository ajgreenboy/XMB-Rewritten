#pragma once

#include <QHash>
#include <QRect>
#include <QVariantAnimation>
#include <QWidget>

#include "notification_toast.h"

namespace xmb {
class Assets;

// The toast shown over a running game: a small click-through, never-activating, always-on-top window that draws the
// same measured toast as the XMB's own pass (notification_toast.h: geometry, fill, glyph, text rows, fade times). The
// window is exactly the box, placed top-right on the monitor the foreground (game) window is on, and scaled by that
// monitor's design-space factor, so it matches the XMB toast at any resolution or DPI.
class NotifyOverlay : public QWidget {
public:
    NotifyOverlay(Assets& assets,const QString& fontFamily);
    void showNotification(const QString& title,const QString& detail,const QString& icon);
    void hideNotification();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QRect inkBounds(const QString& texture,const QImage& image);
    void fadeTo(double target,int milliseconds);

    Assets& assets_;
    QString fontFamily_,title_,detail_,icon_;
    xmb::toast::Layout layout_;
    QString drawnTitle_,drawnDetail_;
    double scale_=1.;
    QPointF designOrigin_;      // the design area's top-left, in this window's own pixels
    bool wanted_=false;         // shown (or fading in) as opposed to fading out
    QHash<QString,QRect> inkBounds_;
    QVariantAnimation fade_;
};
}
