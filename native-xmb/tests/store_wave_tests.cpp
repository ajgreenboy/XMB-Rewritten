// Deterministic tests for the legacy Store wave model (src/store_wave.h/.cpp): the MNU extraction, the
// HSV -> RGB conversion and the curved crest geometry. Everything is pure and firmware-free: the MNU is
// built from synthetic key/value pairs (invented numbers with the same "monotonic rise then level crest"
// structure as the firmware's bk_curvp, but not the firmware's own values), so no firmware data is
// reproduced and no dev_flash is read.

#include "store_wave.h"

#include <cstdio>
#include <cmath>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

using xmb::storewave::Params;
using xmb::storewave::paramsFromMnu;
using xmb::storewave::hsvToRgb;
using xmb::storewave::crestPath;
using xmb::storewave::crestBox;
using xmb::storewave::ControlPoint;
using Mnu = xmb::Materials::Mnu;

Mnu mnuWith(std::initializer_list<std::pair<const char*, double>> pairs) {
    Mnu m;
    m.ok = true;
    for (const auto& [k, v] : pairs) {
        xmb::Materials::Item item;
        item.kind = xmb::Materials::Item::Kind::Number;
        item.number = v;
        m.byName.insert(QString::fromLatin1(k), item);
    }
    return m;
}

const std::initializer_list<std::pair<const char*, double>> kFull = {
    {"bk_curvp_0_x", 0.10}, {"bk_curvp_0_y", 0.90},
    {"bk_curvp_1_x", 0.12}, {"bk_curvp_1_y", 0.40},
    {"bk_curvp_2_x", 0.15}, {"bk_curvp_2_y", 0.10},
    {"bk_curvp_3_x", 0.50}, {"bk_curvp_3_y", 0.05},
    {"bk_curvp_4_x", 0.90}, {"bk_curvp_4_y", 0.05},
    {"bk_color_h", 0.60}, {"bk_color_s", 0.80}, {"bk_color_v", 0.50},
    {"glow_col_r", 0.10}, {"glow_col_g", 0.94}, {"glow_col_b", 1.00},
    {"glow_intensity", 0.21},
};
} // namespace

int main() {
    // ---- MNU extraction ---------------------------------------------------------
    {
        const Params p = paramsFromMnu(mnuWith(kFull));
        check(p.present, "full mnu -> present");
        check(near(p.curve[0].u, 0.10) && near(p.curve[0].v, 0.90), "curve[0] = (0.10, 0.90)");
        check(near(p.curve[4].u, 0.90) && near(p.curve[4].v, 0.05), "curve[4] = (0.90, 0.05)");
        check(near(p.colorH, 0.60) && near(p.colorS, 0.80) && near(p.colorV, 0.50), "bk_color HSV");
        check(near(p.glowR, 0.10) && near(p.glowG, 0.94) && near(p.glowB, 1.00), "glow_col RGB");
        check(near(p.glowIntensity, 0.21), "glow_intensity");
    }
    {
        Mnu empty;
        check(!paramsFromMnu(empty).present, "unparsed (ok=false) mnu -> not present");
    }
    {
        // A full curve but no colour keys must still fall back.
        Mnu m = mnuWith({
            {"bk_curvp_0_x", 0.10}, {"bk_curvp_0_y", 0.90},
            {"bk_curvp_1_x", 0.12}, {"bk_curvp_1_y", 0.40},
            {"bk_curvp_2_x", 0.15}, {"bk_curvp_2_y", 0.10},
            {"bk_curvp_3_x", 0.50}, {"bk_curvp_3_y", 0.05},
            {"bk_curvp_4_x", 0.90}, {"bk_curvp_4_y", 0.05},
        });
        check(!paramsFromMnu(m).present, "curve-only mnu -> not present");
    }

    // ---- HSV -> RGB -------------------------------------------------------------
    {
        const QColor red = hsvToRgb(0.0, 1.0, 1.0);
        check(red.red() == 255 && red.green() == 0 && red.blue() == 0, "hsv(0,1,1) -> pure red");
        const QColor green = hsvToRgb(1.0 / 3.0, 1.0, 1.0);
        check(green.red() == 0 && green.green() == 255 && green.blue() == 0, "hsv(1/3,1,1) -> pure green");
        const QColor blue = hsvToRgb(2.0 / 3.0, 1.0, 1.0);
        check(blue.red() == 0 && blue.green() == 0 && blue.blue() == 255, "hsv(2/3,1,1) -> pure blue");
        const QColor grey = hsvToRgb(0.5, 0.0, 0.5);
        check(grey.red() == 128 && grey.green() == 128 && grey.blue() == 128, "hsv(any,0,0.5) -> mid grey");
        // A blue sector value stays blue-dominant (the firmware's bk_color is a blue hue ~0.60).
        const QColor navy = hsvToRgb(0.60, 0.80, 0.50);
        check(navy.blue() > navy.red() && navy.blue() > navy.green(), "hsv(0.60,0.80,0.50) -> blue-dominant");
    }

    // ---- Crest geometry ---------------------------------------------------------
    {
        Params p;
        p.curve[0] = ControlPoint{0.10, 0.90};
        p.curve[1] = ControlPoint{0.12, 0.40};
        p.curve[2] = ControlPoint{0.15, 0.10};
        p.curve[3] = ControlPoint{0.50, 0.05};
        p.curve[4] = ControlPoint{0.90, 0.05};
        const QRectF box = crestBox();

        const QPainterPath a = crestPath(p, box);
        const QPainterPath b = crestPath(p, box);
        check(a.elementCount() > 2, "crest path is non-trivial");
        check(a.boundingRect() == b.boundingRect(), "crest path is deterministic");

        const QRectF r = a.boundingRect();
        // The arch spans the crest band horizontally (both feet are inside it) ...
        check(r.left() >= box.left() - 1e-6 && r.right() <= box.right() + 1e-6, "crest x within band");
        // ... rises to the upper middle (its peak is near the band's top) ...
        check(r.top() < box.top() + 0.2 * (box.bottom() - box.top()), "crest peaks in the upper middle");
        // ... and is a genuine arch, not a straight horizontal line: the feet sit far below the peak.
        check(r.height() > 0.5 * (box.bottom() - box.top()), "crest is an arch (large vertical extent)");
        // The apex (highest point) is near the horizontal centre of the band.
        check(std::fabs(r.center().x() - (box.left() + box.right()) / 2.0) < 0.2 * box.width(),
              "crest apex near the band centre");
    }

    if (failures) {
        std::fprintf(stderr, "store-wave: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("store-wave: %d checks passed\n", checks);
    return 0;
}
