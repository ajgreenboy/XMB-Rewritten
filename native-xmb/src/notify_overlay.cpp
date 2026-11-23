#include "notify_overlay.h"
#include "design_space.h"
#include "assets.h"
#include <QGuiApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace xmb {

NotifyOverlay::NotifyOverlay(Assets& assets,const QString& fontFamily):
    QWidget(nullptr,Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::Tool|Qt::WindowDoesNotAcceptFocus),
    assets_(assets),fontFamily_(fontFamily) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    fade_.setEasingCurve(QEasingCurve::Linear);
    connect(&fade_,&QVariantAnimation::valueChanged,this,[this](const QVariant& value){setWindowOpacity(value.toDouble());});
    connect(&fade_,&QVariantAnimation::finished,this,[this]{
        if(wanted_)return;
        hide();title_.clear();detail_.clear();icon_.clear();
    });
}

void NotifyOverlay::fadeTo(double target,int milliseconds) {
    fade_.stop();
    fade_.setDuration(milliseconds);
    fade_.setStartValue(windowOpacity());
    fade_.setEndValue(target);
    fade_.start();
}

void NotifyOverlay::showNotification(const QString& title,const QString& detail,const QString& icon) {
    if(wanted_ && isVisible() && title_==title && detail_==detail && icon_==icon)return;
    title_=title;detail_=detail;icon_=icon;
    QScreen* screen=QGuiApplication::primaryScreen();
#ifdef Q_OS_WIN
    // The monitor the game is on: the foreground window's centre.
    const HWND foreground=GetForegroundWindow();
    RECT bounds{};
    if(foreground && GetWindowRect(foreground,&bounds)) {
        const QPoint center((bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2);
        if(QScreen* found=QGuiApplication::screenAt(center))screen=found;
    }
#endif
    if(!screen)return;
    const QRect area=screen->geometry();
    // The same centred contain-fit design space as the XMB's own pass, so the toast sits where it does there.
    const DesignSpace design(area.size());
    const bool twoLines=!detail_.isEmpty();
    QFont titleFont(fontFamily_);titleFont.setPixelSize(int(toast::kTextSize));
    QFont detailFont(fontFamily_);detailFont.setPixelSize(int(toast::kDetailSize));
    const QFontMetrics titleMetrics(titleFont),detailMetrics(detailFont);
    const int budget=int(toast::maxTextWidth());
    drawnTitle_=titleMetrics.elidedText(title_,Qt::ElideRight,budget);
    drawnDetail_=twoLines?detailMetrics.elidedText(detail_,Qt::ElideRight,budget):QString();
    const double textWidth=std::max<double>(titleMetrics.horizontalAdvance(drawnTitle_),twoLines?detailMetrics.horizontalAdvance(drawnDetail_):0.);
    const QImage image=assets_.bestImage(QStringLiteral("system_plugin"),icon_);
    layout_=toast::layoutToast(textWidth,!image.isNull(),twoLines);
    scale_=design.x;
    const auto rect=toast::overlayWindow(layout_.box,design.x,design.left,design.top);
    designOrigin_=QPointF(design.left-rect.x,design.top-rect.y);
    setGeometry(area.left()+rect.x,area.top()+rect.y,rect.w,rect.h);
#ifdef Q_OS_WIN
    const HWND hwnd=reinterpret_cast<HWND>(winId());
    const LONG_PTR styles=GetWindowLongPtr(hwnd,GWL_EXSTYLE);
    SetWindowLongPtr(hwnd,GWL_EXSTYLE,styles|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_LAYERED);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_FRAMECHANGED);
#endif
    const bool fresh=!isVisible() || !wanted_;
    wanted_=true;
    if(!isVisible())setWindowOpacity(0.);
    show();
    update();
    if(fresh)fadeTo(1.,toast::kFadeInMs);
}

void NotifyOverlay::hideNotification() {
    if(!isVisible()){title_.clear();detail_.clear();icon_.clear();return;}
    if(!wanted_)return;   // already fading out
    wanted_=false;
    fadeTo(0.,toast::kFadeOutMs);
}

// The visible (alpha) bounds of a texture, as the XMB view fits them: RCO glyphs carry transparent padding.
QRect NotifyOverlay::inkBounds(const QString& texture,const QImage& image) {
    auto bounds=inkBounds_.value(texture);
    if(!bounds.isNull())return bounds;
    const QImage argb=image.convertToFormat(QImage::Format_ARGB32);
    for(int y=0;y<argb.height();++y){const auto* row=reinterpret_cast<const QRgb*>(argb.constScanLine(y));for(int x=0;x<argb.width();++x)if(qAlpha(row[x])>16)bounds=bounds.united(QRect(x,y,1,1));}
    if(bounds.isNull())bounds=image.rect();
    inkBounds_.insert(texture,bounds);
    return bounds;
}

void NotifyOverlay::paintEvent(QPaintEvent*) {
    if(title_.isEmpty())return;
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing|QPainter::SmoothPixmapTransform|QPainter::TextAntialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);p.fillRect(rect(),Qt::transparent);p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setTransform(QTransform(scale_,0,0,scale_,designOrigin_.x(),designOrigin_.y()));   // 1920x1080 design units, as the XMB view draws
    const auto& box=layout_.box;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(toast::kFillRed,toast::kFillGreen,toast::kFillBlue,int(std::lround(toast::kFillAlpha*255.))));
    p.drawRoundedRect(QRectF(box.x,box.y,box.w,box.h),toast::kBoxRadius,toast::kBoxRadius);
    if(layout_.hasIcon) {
        const QImage image=assets_.bestImage(QStringLiteral("system_plugin"),icon_);
        if(!image.isNull()) {
            const auto bounds=inkBounds(icon_,image);
            const QRectF slot(layout_.icon.x,layout_.icon.y,layout_.icon.side,layout_.icon.side);
            const QSizeF size=QSizeF(bounds.size()).scaled(slot.size(),Qt::KeepAspectRatio);
            p.drawImage(QRectF(slot.center()-QPointF(size.width()/2,size.height()/2),size),image,bounds);
        }
    }
    const auto label=[&](const toast::TextRect& r,const QString& text,int size,int grey) {
        QFont font(fontFamily_);font.setPixelSize(size);font.setStyleStrategy(QFont::PreferAntialias);
        font.setHintingPreference(QFont::PreferNoHinting);p.setFont(font);
        const QRectF at(r.x,r.y,r.w,r.h);
        // The measured soft shadow, the same four taps the XMB toast draws (notification_toast.h).
        for(int tap=0;tap<toast::kShadowTaps;++tap) {
            const auto shadow=toast::shadowTap(tap);
            p.setPen(QColor(0,0,0,int(std::lround(255.*toast::shadowTapCoverage()))));
            p.drawText(at.translated(shadow.dx,shadow.dy),Qt::AlignLeft|Qt::AlignVCenter,text);
        }
        p.setPen(QColor(grey,grey,grey));p.drawText(at,Qt::AlignLeft|Qt::AlignVCenter,text);
    };
    label(layout_.title,drawnTitle_,int(toast::kTextSize),toast::kTitleGrey);
    if(layout_.twoLines)label(layout_.detail,drawnDetail_,int(toast::kDetailSize),toast::kDetailGrey);
}
}
