#pragma once
// The OSK accent page's key rows (BEHAVIOUR-GAPS-SPEC section 3, audit/OSK-FIRMWARE.md section 3.4).
//
// TRACED: the eight 10-character UTF-16BE row strings sit in the decrypted oskfullkeypanel_plugin module at file
// offsets 0x409c0..0x40a77 (VA 0x408d0..0x40987) and are listed by the module's own 24-byte label table as two
// consecutive layout groups, ids 0x5cc..0x5dc (lower accent) and 0x5e0..0x5f0 (capital accent), each followed by the
// next layout (the Polish and Turkish variants at 0x5f4/0x608 and beyond). The glyph codepoints below are those rows
// transcribed as code constants; no firmware bytes are stored. The page replaces all four character rows of the
// lower/caps page: the digit row becomes "a-grave .. c-cedilla [ ]".
//
// Not part of the module data (INFERRED, labelled where used): how the page is entered and left, and that the
// lower/capital accent pair follows the lower/caps toggle. See OSK-FIRMWARE.md 3.4 and 6.1.
#include <QString>
#include <QStringList>

namespace xmb {

// Four rows of ten glyphs for the accent page: lower case, or the capital variant.
inline QStringList oskAccentRows(bool capital) {
    if (!capital) {
        return QStringList{
            QString::fromUtf16(u"\u00e0\u00e1\u00e2\u00e3\u00e4\u00e5\u00e6\u00e7[]"),
            QString::fromUtf16(u"\u00e8\u00e9\u00ea\u00eb\u00ec\u00ed\u00ee\u00ef;:"),
            QString::fromUtf16(u"\u00f1\u00f2\u00f3\u00f4\u00f5\u00f6\u00f8\u0153`\u00a1"),
            QString::fromUtf16(u"\u00df\u00f9\u00fa\u00fb\u00fc\u00fd\u00ff,.\u00bf")};
    }
    return QStringList{
        QString::fromUtf16(u"\u00c0\u00c1\u00c2\u00c3\u00c4\u00c5\u00c6\u00c7<>"),
        QString::fromUtf16(u"\u00c8\u00c9\u00ca\u00cb\u00cc\u00cd\u00ce\u00cf=+"),
        QString::fromUtf16(u"\u00d1\u00d2\u00d3\u00d4\u00d5\u00d6\u00d8\u0152~\""),
        QString::fromUtf16(u"\u00df\u00d9\u00da\u00db\u00dc\u00dd\u0178-_/")};
}

// The mode cell's glyph for the accent pages: the module names the textures MODE_GUIDE_ALPHABET_ACCENT and
// MODE_GUIDE_ALPHABET_ACCENT_CAP (traced names); that they show a-grave / A-grave is INFERRED from the msg_abc_accent
// caption ("ABC" + A-grave) and the L2 key label `a/a-grave`, not read from the pictures.
inline QString oskAccentModeCaption(bool capital) {
    return QString(QChar(capital ? 0x00c0 : 0x00e0));
}

} // namespace xmb
