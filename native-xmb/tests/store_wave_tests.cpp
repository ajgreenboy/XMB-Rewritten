// Deterministic tests for the legacy Store wave model (src/store_wave.h/.cpp): the MNU colour extraction,
// the HSV -> RGB conversion and the fitted crest geometry. Everything is pure and firmware-free: the MNU
// is built from synthetic key/value pairs (invented numbers), so no firmware data is reproduced and no
// dev_flash is read.

#include "store_wave.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

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
    {"bk_color_h", 0.60}, {"bk_color_s", 0.80}, {"bk_color_v", 0.50},
    {"glow_col_r", 0.10}, {"glow_col_g", 0.94}, {"glow_col_b", 1.00},
    {"glow_intensity", 0.21},
};
} // namespace

int main() {
    // ---- MNU colour extraction --------------------------------------------------
    {
        const Params p = paramsFromMnu(mnuWith(kFull));
        check(p.present, "full mnu -> present");
        check(near(p.colorH, 0.60) && near(p.colorS, 0.80) && near(p.colorV, 0.50), "bk_color HSV");
        check(near(p.glowR, 0.10) && near(p.glowG, 0.94) && near(p.glowB, 1.00), "glow_col RGB");
        check(near(p.glowIntensity, 0.21), "glow_intensity");
    }
    {
        Mnu empty;
        check(!paramsFromMnu(empty).present, "unparsed (ok=false) mnu -> not present");
    }
    {
        Mnu m = mnuWith({{"bk_color_h", 0.60}, {"bk_color_s", 0.80}, {"bk_color_v", 0.50}});
        check(!paramsFromMnu(m).present, "colour-only mnu missing glow keys -> not present");
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
        const QColor navy = hsvToRgb(0.60, 0.80, 0.50);
        check(navy.blue() > navy.red() && navy.blue() > navy.green(), "hsv(0.60,0.80,0.50) -> blue-dominant");
    }

    // ---- Crest geometry (a fitted open diagonal, NOT a rectangle) ----------------
    {
        const QPainterPath a = crestPath();
        const QPainterPath b = crestPath();
        // crestPath() only ever calls moveTo() and cubicTo(), so it is a single open curve by
        // construction; the element count just confirms it is one move + one cubic.
        check(a.elementCount() >= 2, "crest is a moveTo + cubic");
        check(a.boundingRect() == b.boundingRect(), "crest is deterministic");

        // No vertical stems: the crest is monotonic in x with a gentle, bounded slope. A folded/vertical
        // rectangle would backtrack or show a near-vertical edge (|dx/dy| ~ 0).
        QPointF prev = a.pointAtPercent(0.0);
        bool monotonic = true;
        double maxSlope = 0;
        for (int i = 1; i <= 40; ++i) {
            const QPointF pt = a.pointAtPercent(double(i) / 40.0);
            if (pt.x() <= prev.x()) monotonic = false;
            maxSlope = std::max(maxSlope, std::fabs(pt.y() - prev.y()) / (pt.x() - prev.x() + 1e-9));
            prev = pt;
        }
        check(monotonic, "crest x is monotonic (no vertical stems / backtracking)");
        check(maxSlope < 1.5, "crest slope is gentle (no near-vertical edge)");

        // Not a rectangle: the crest is far wider than it is tall (a swoosh, not a square/stem).
        const QRectF r = a.boundingRect();
        check(r.width() > 2.0 * r.height(), "crest is wide, not a tall rectangle");
        // It rises into the upper middle of the 1920x1080 canvas.
        check(r.top() < 140.0, "crest peaks in the upper middle");
        check(r.top() > 40.0, "crest does not leave the upper band");
    }

    if (failures) {
        std::fprintf(stderr, "store-wave: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("store-wave: %d checks passed\n", checks);
    return 0;
}
