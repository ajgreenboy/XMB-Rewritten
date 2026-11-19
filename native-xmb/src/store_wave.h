#pragma once
#include <QColor>
#include <QPainterPath>
#include "rco_objects.h"   // Materials::Mnu

namespace xmb::storewave {

// The legacy PlayStation Store background "wave". Only the COLOURS come from the firmware — the
// `vsh/resource/qgl/store.qrc` entry `/datacom/store.mnu` carries `bk_color_{h,s,v}` (the wave's HSV
// tint) and `glow_col_{r,g,b}` / `glow_intensity` (the crest's cyan glow) — read through Materials
// (rco_objects.h). The SHAPE is a soft open curve FITTED to the 2008 period reference
// (`2008-04-03_us-store-detail-preview_flickr-2386461216.jpg`), not a firmware shape: the firmware's
// `bk_curvp` profile is a ribbon cross-section whose 3D sweep cannot be reconstructed in a QPainter
// path, so it is deliberately not used for geometry. See native-xmb/audit/LEGACY-STORE.md.

struct Params {
    double colorH = 0, colorS = 0, colorV = 0;   // bk_color (HSV, 0..1)
    double glowR = 0, glowG = 0, glowB = 0;      // glow_col (RGB, 0..1)
    double glowIntensity = 0;                    // glow_intensity (0..1)
    bool present = false;                        // false when store.mnu cannot be read / is incomplete
};

// Extract the wave COLOURS from a parsed store MNU. Returns !present when the MNU is missing or any of
// the colour keys is absent, so a caller can tell "no firmware material" apart from "material with these
// values".
Params paramsFromMnu(const Materials::Mnu& mnu);

// HSV (h, s, v each 0..1) -> an opaque RGB colour.
QColor hsvToRgb(double h, double s, double v);

// The crest as a single soft open cubic, fitted to the 2008 reference's ribbon (ref x320 y100 rising to
// x570 y40 then tapering right to x715 y115 @ 1024x576, scaled x1.875 to this 1920x1080 canvas). It is
// a gentle diagonal swoosh — monotonic in x, no vertical stems, not a closed or symmetric rectangle.
// This is a FIT, not a firmware shape.
QPainterPath crestPath();

} // namespace xmb::storewave
