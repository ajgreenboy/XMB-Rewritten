#include "store_wave.h"
#include <QtMath>
#include <algorithm>

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
    // The wave needs every control point and every colour key; a partially-populated material would draw a
    // broken wave, so an incomplete MNU is reported as "not present" and the caller keeps its fallback.
    static const char* const required[] = {
        "bk_curvp_0_x", "bk_curvp_0_y", "bk_curvp_1_x", "bk_curvp_1_y",
        "bk_curvp_2_x", "bk_curvp_2_y", "bk_curvp_3_x", "bk_curvp_3_y",
        "bk_curvp_4_x", "bk_curvp_4_y", "bk_color_h", "bk_color_s", "bk_color_v",
        "glow_col_r", "glow_col_g", "glow_col_b", "glow_intensity",
    };
    for (const char* key : required) if (!hasNumber(mnu, QString::fromLatin1(key))) return p;
    for (int i = 0; i < 5; ++i)
        p.curve[i] = ControlPoint{
            number(mnu, QStringLiteral("bk_curvp_%1_x").arg(i)),
            number(mnu, QStringLiteral("bk_curvp_%1_y").arg(i)) };
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

namespace {
QPainterPath catmullRom(const QVector<QPointF>& pts) {
    QPainterPath path;
    const int n = pts.size();
    if (n < 2) return path;
    path.moveTo(pts[0]);
    for (int i = 0; i < n - 1; ++i) {
        const QPointF p0 = pts[std::max(0, i - 1)];
        const QPointF p1 = pts[i];
        const QPointF p2 = pts[i + 1];
        const QPointF p3 = pts[std::min(n - 1, i + 2)];
        const QPointF c1 = p1 + (p2 - p0) / 6.0;
        const QPointF c2 = p2 - (p3 - p1) / 6.0;
        path.cubicTo(c1, c2, p2);
    }
    return path;
}
} // namespace

QPainterPath crestPath(const Params& params, const QRectF& box) {
    const double apexX = 0.5 * (box.left() + box.right());
    const auto mapPoint = [&](const ControlPoint& p, bool right) {
        const double x = right ? (box.right() - p.u * (box.right() - apexX))
                               : (box.left() + p.u * (apexX - box.left()));
        const double y = box.top() + p.v * (box.bottom() - box.top());
        return QPointF(x, y);
    };
    QVector<QPointF> anchors;
    anchors.reserve(10);
    for (int i = 0; i < 5; ++i) anchors.push_back(mapPoint(params.curve[i], false));
    for (int i = 4; i >= 0; --i) anchors.push_back(mapPoint(params.curve[i], true));
    return catmullRom(anchors);
}

} // namespace xmb::storewave
