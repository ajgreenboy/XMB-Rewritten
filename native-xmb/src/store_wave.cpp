#include "store_wave.h"
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace xmb::storewave {

namespace {
// A key is "present" only when it is an actual number; number() alone cannot distinguish "0" from
// "absent", and the presence test matters for the fallback path.
bool hasNumber(const Materials::Mnu& mnu, const QString& key) {
    const auto it = mnu.byName.constFind(key);
    return it != mnu.byName.cend() && it->kind == Materials::Item::Kind::Number;
}
double number(const Materials::Mnu& mnu, const QString& key, double fallback = 0) {
    const auto it = mnu.byName.constFind(key);
    if (it == mnu.byName.cend() || it->kind != Materials::Item::Kind::Number) return fallback;
    return it->number;
}
} // namespace

Params paramsFromMnu(const Materials::Mnu& mnu) {
    Params p;
    if (!mnu.ok) return p;
    // The wave needs every colour key; a partially-populated material would draw with the wrong tint, so
    // an incomplete MNU is reported as "not present" and the caller keeps its fallback colours.
    static const char* const required[] = {
        "bk_color_h", "bk_color_s", "bk_color_v",
        "glow_col_r", "glow_col_g", "glow_col_b", "glow_intensity",
    };
    for (const char* key : required) if (!hasNumber(mnu, QString::fromLatin1(key))) return p;
    p.colorH = number(mnu, QStringLiteral("bk_color_h"));
    p.colorS = number(mnu, QStringLiteral("bk_color_s"));
    p.colorV = number(mnu, QStringLiteral("bk_color_v"));
    p.glowR = number(mnu, QStringLiteral("glow_col_r"));
    p.glowG = number(mnu, QStringLiteral("glow_col_g"));
    p.glowB = number(mnu, QStringLiteral("glow_col_b"));
    p.glowIntensity = number(mnu, QStringLiteral("glow_intensity"));
    p.present = true;
    return p;
}

QColor hsvToRgb(double h, double s, double v) {
    double hue = std::fmod(h, 1.0);
    if (hue < 0) hue += 1.0;
    const double sat = std::clamp(s, 0.0, 1.0);
    const double val = std::clamp(v, 0.0, 1.0);
    const int sector = int(hue * 6.0) % 6;
    const double f = hue * 6.0 - std::floor(hue * 6.0);
    const double p = val * (1.0 - sat);
    const double q = val * (1.0 - sat * f);
    const double t = val * (1.0 - sat * (1.0 - f));
    double r, g, b;
    switch (sector) {
    case 0: r = val; g = t;   b = p;   break;
    case 1: r = q;   g = val; b = p;   break;
    case 2: r = p;   g = val; b = t;   break;
    case 3: r = p;   g = q;   b = val; break;
    case 4: r = t;   g = p;   b = val; break;
    default: r = val; g = p;   b = q;  break;
    }
    const auto toByte = [](double x) { return int(std::clamp(x, 0.0, 1.0) * 255.0 + 0.5); };
    return QColor(toByte(r), toByte(g), toByte(b));
}

QPainterPath crestPath() {
    // Fitted to the 2008 reference's soft ribbon: ref (320,100) -> (570,40) -> (715,115) at 1024x576,
    // scaled x1.875 to the 1920x1080 canvas. A single open cubic: it rises from the lower-left, peaks in
    // the upper middle, then tapers gently down-right. Monotonic in x, so no vertical stems.
    QPainterPath path;
    path.moveTo(600.0, 187.0);
    path.cubicTo(840.0, 74.0, 1160.0, 74.0, 1341.0, 216.0);
    return path;
}

} // namespace xmb::storewave
