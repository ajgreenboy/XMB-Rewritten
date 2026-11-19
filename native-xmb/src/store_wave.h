#pragma once
#include <array>
#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include "rco_objects.h"   // Materials::Mnu

namespace xmb::storewave {

// The legacy PlayStation Store background "wave" material, read at run time from the firmware's
// `vsh/resource/qgl/store.qrc` entry `/datacom/store.mnu` through Materials (rco_objects.h). The MNU
// carries five `bk_curvp_N_{x,y}` control points (the ribbon's profile curve), `bk_color_{h,s,v}` (the
// wave's HSV tint) and `glow_col_{r,g,b}` / `glow_intensity` (the crest's glow). This header is the pure,
// testable model: it extracts those values and turns them into the 2D QPainter crest the store surface
// draws. The 2D path is an INTERPRETATION of the firmware's 3D ribbon (a swept profile), not a
// shader-equivalent reconstruction, and the orientation is a fit to the 2008 period reference — see
// native-xmb/audit/LEGACY-STORE.md.

struct ControlPoint { double u = 0, v = 0; };

struct Params {
    std::array<ControlPoint, 5> curve{};   // bk_curvp_0..4, normalized [0,1] (v: 0 crest, 1 trough)
    double colorH = 0, colorS = 0, colorV = 0;   // bk_color (HSV, 0..1)
    double glowR = 0, glowG = 0, glowB = 0;      // glow_col (RGB, 0..1)
    double glowIntensity = 0;                    // glow_intensity (0..1)
    bool present = false;                        // false when store.mnu cannot be read / is incomplete
};

// Extract the wave material from a parsed store MNU. Returns !present when the MNU is missing or any of
// the five control points / colour keys is absent, so a caller can tell "no firmware material" apart from
// "material with these values".
Params paramsFromMnu(const Materials::Mnu& mnu);

// HSV (h, s, v each 0..1) -> an opaque RGB colour.
QColor hsvToRgb(double h, double s, double v);

// The crest band in the 1920x1080 canvas: the 2008 reference's bright cyan crest (x300..716 @ 1024x576,
// peak y~43) scaled x1.875, with the crest feet near y270. A fit to the reference; the firmware stores no
// screen-space rectangle.
inline constexpr double kCrestLeft = 562.0;
inline constexpr double kCrestRight = 1342.0;
inline constexpr double kCrestTop = 81.0;
inline constexpr double kCrestBottom = 270.0;
inline QRectF crestBox() { return QRectF(kCrestLeft, kCrestTop, kCrestRight - kCrestLeft, kCrestBottom - kCrestTop); }

// The curved crest as a smooth (Catmull-Rom -> cubic Bezier) path through the five control points, mapped
// into `box` and folded about the band's horizontal centre so the monotonic firmware profile (a rise then
// a level crest) reads as a symmetric arch that peaks in the upper middle. The fold is an interpretive fit.
QPainterPath crestPath(const Params& params, const QRectF& box);

} // namespace xmb::storewave
