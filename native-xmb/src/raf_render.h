#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

namespace xmb {
struct RafStillStats {
    int actors=0,models=0,textures=0,triangles=0;
    bool cameraFallback=false,lightFallback=false;
};
// A dynamic theme's scene drawn as ONE STILL FRAME: the actors at the initial pose the RAF tree declares, the VSMX
// script not run (so no motion, no timers, no time of day), approximate GLSL lighting in place of the firmware's raf.qrc
// effect programs. Null with an error on any failure; callers then keep the theme's still bgimage.
QImage renderRafStill(const QByteArray& raf,const QString& devFlash,int width,int height,QString* error=nullptr,RafStillStats* stats=nullptr);
// Kill switch: XMB_RAF_THEMES=0 skips the scene and shows the theme's still bgimage, as before scenes were drawn.
bool rafThemesEnabled();
}
