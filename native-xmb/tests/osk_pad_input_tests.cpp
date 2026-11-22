#include "osk_pad_input.h"
#include "osk_accent_rows.h"
#include <QCoreApplication>
#include <cstdio>

// The pure part of the on-screen keyboard's pad extras: L1/R1 caret, L2/R2 pages, SELECT, the trigger latch and the
// open/close fade (audit/BEHAVIOUR-GAPS-SPEC.md 3). No window and no pad. Build with -DXMB_OSK_PAD_BUILD_TESTS=ON.
using namespace xmb;

static int failures=0;
static int checks=0;
static void check(bool ok,const char* what) {
    ++checks;
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static QVector<OskPage> pagesNamed(const QStringList& names) {
    QVector<OskPage> pages;
    for(const QString& name:names){OskPage page;page.name=name;pages.push_back(page);}
    return pages;
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    using A=OskAction;

    OskTriggerLatch latch;
    check(oskPadActions(0,0,0,latch).isEmpty(),"no buttons, no actions");
    check(oskPadActions(oskPadL1,0,0,latch)==QVector<A>{A::CaretLeft},"L1 moves the caret left");
    check(oskPadActions(oskPadR1,0,0,latch)==QVector<A>{A::CaretRight},"R1 moves the caret right");
    check(oskPadActions(oskPadSelect,0,0,latch)==QVector<A>{A::Mode},"SELECT is Input Mode");
    check(oskPadActions(0x1000|0x0010|0x0001,0,0,latch).isEmpty(),"Cross, START and the d-pad are not extras");
    check((oskPadActions(oskPadL1|oskPadR1|oskPadSelect,0,0,latch)==QVector<A>{A::CaretLeft,A::CaretRight,A::Mode}),"several buttons keep the fixed order");

    OskTriggerLatch triggers;
    check(oskPadActions(0,0.4f,0,triggers).isEmpty(),"a trigger below 0.5 does nothing");
    check(oskPadActions(0,0.9f,0,triggers)==QVector<A>{A::Accent},"L2 past 0.5 is the accent page");
    check(oskPadActions(0,1.0f,0,triggers).isEmpty(),"a held L2 fires once");
    check(oskPadActions(0,0.3f,0,triggers).isEmpty() && triggers.l2,"L2 between 0.25 and 0.5 stays latched (no chatter)");
    check(oskPadActions(0,0.1f,0,triggers).isEmpty() && !triggers.l2,"L2 below 0.25 re-arms");
    check(oskPadActions(0,0.9f,0,triggers)==QVector<A>{A::Accent},"L2 fires again after release");
    OskTriggerLatch second;
    check(oskPadActions(0,0,0.7f,second)==QVector<A>{A::Shift},"R2 past 0.5 is the caps page");
    OskTriggerLatch both;
    check((oskPadActions(0,0.8f,0.8f,both)==QVector<A>{A::Accent,A::Shift}),"both triggers in one poll: L2 then R2");

    const auto twoPages=pagesNamed({"lower","caps"});
    check(oskPageForAction(twoPages,0,A::Shift)==1,"R2 on lower goes to caps");
    check(oskPageForAction(twoPages,1,A::Shift)==0,"R2 on caps goes back to lower");
    check(oskPageForAction(twoPages,0,A::Accent)==-1,"L2 does nothing while a layout carries no accent page (the tenkey panel)");
    check(oskPageForAction(twoPages,1,A::Accent)==-1,"L2 does nothing on caps either without an accent page");
    check(oskPageForAction(twoPages,0,A::CaretLeft)==-1,"a caret action never changes the page");
    const auto threePages=pagesNamed({"lower","caps","accent"});
    check(oskPageForAction(threePages,0,A::Accent)==2,"with an accent page, L2 opens it");
    check(oskPageForAction(threePages,2,A::Accent)==0,"L2 on the accent page returns to lower");
    check(oskPageForAction(threePages,2,A::Shift)==1,"R2 on the accent page goes to caps");
    // The shipping set: lower, caps, accent, accentCaps (indices 0..3). L2 and R2 are two switches over the four pages.
    const auto four=pagesNamed({"lower","caps","accent","accentCaps"});
    check(oskPageForAction(four,0,A::Accent)==2,"four pages: L2 on lower opens accent");
    check(oskPageForAction(four,1,A::Accent)==3,"four pages: L2 on caps opens accentCaps (capital state kept)");
    check(oskPageForAction(four,2,A::Accent)==0,"four pages: L2 on accent returns to lower");
    check(oskPageForAction(four,3,A::Accent)==1,"four pages: L2 on accentCaps returns to caps");
    check(oskPageForAction(four,2,A::Shift)==3,"four pages: R2 on accent goes to accentCaps (accent state kept)");
    check(oskPageForAction(four,3,A::Shift)==2,"four pages: R2 on accentCaps goes back to accent");
    check(oskPageForAction(four,0,A::Shift)==1 && oskPageForAction(four,1,A::Shift)==0,"four pages: R2 still toggles lower <-> caps");
    {
        // Every path through the two switches visits the page whose two flags match, and two presses undo themselves.
        bool inverse=true,flags=true;
        for(int start=0;start<4;++start) {
            const int viaAccent=oskPageForAction(four,start,A::Accent),viaShift=oskPageForAction(four,start,A::Shift);
            inverse=inverse && oskPageForAction(four,viaAccent,A::Accent)==start && oskPageForAction(four,viaShift,A::Shift)==start;
            flags=flags && (viaAccent&1)==(start&1) && (viaShift&2)==(start&2)   // L2 keeps capital (bit 0), R2 keeps accent (bit 1)
                        && ((viaAccent^start)&2) && ((viaShift^start)&1);
        }
        check(inverse,"four pages: each of L2 and R2 is its own inverse");
        check(flags,"four pages: L2 flips only the accent state, R2 only the capital state");
    }
    check(oskPageForAction(four,0,A::MoveUp)==-1 && oskPageForAction(four,2,A::CaretRight)==-1,"non-page actions never change the page");
    // A pad-driven walk: L2 then R2 reaches accentCaps from lower, whichever trigger is pulled first.
    {
        OskTriggerLatch walk;int page=0;
        for(const A a:oskPadActions(0,0.9f,0,walk)){const int t=oskPageForAction(four,page,a);if(t>=0)page=t;}
        for(const A a:oskPadActions(0,0.1f,0.9f,walk)){const int t=oskPageForAction(four,page,a);if(t>=0)page=t;}
        check(page==3,"L2 then R2 from lower lands on accentCaps");
    }

    // The accent rows (osk_accent_rows.h; module ids 0x5cc..0x5dc and 0x5e0..0x5f0). Codepoints are exact, not glyph look-alikes.
    const QStringList lowerRows=oskAccentRows(false),capitalRows=oskAccentRows(true);
    check(lowerRows.size()==4 && capitalRows.size()==4,"accent: four character rows on each page");
    bool tens=true;
    for(const QString& row:lowerRows+capitalRows)tens=tens && row.size()==10;
    check(tens,"accent: every row is ten keys wide (the grid has ten columns)");
    check(lowerRows.at(0)==QString::fromUtf16(u"\u00e0\u00e1\u00e2\u00e3\u00e4\u00e5\u00e6\u00e7[]"),"accent lower row 1: a-grave..c-cedilla then [ ]");
    check(lowerRows.at(1)==QString::fromUtf16(u"\u00e8\u00e9\u00ea\u00eb\u00ec\u00ed\u00ee\u00ef;:"),"accent lower row 2: e-grave..i-diaeresis then ; :");
    check(lowerRows.at(2)==QString::fromUtf16(u"\u00f1\u00f2\u00f3\u00f4\u00f5\u00f6\u00f8\u0153`\u00a1"),"accent lower row 3: n-tilde..o-slash, oe ligature, grave, inverted !");
    check(lowerRows.at(3)==QString::fromUtf16(u"\u00df\u00f9\u00fa\u00fb\u00fc\u00fd\u00ff,.\u00bf"),"accent lower row 4: sharp s, u-grave..y-diaeresis, comma, dot, inverted ?");
    check(capitalRows.at(0)==QString::fromUtf16(u"\u00c0\u00c1\u00c2\u00c3\u00c4\u00c5\u00c6\u00c7<>"),"accent capital row 1: A-grave..C-cedilla then < >");
    check(capitalRows.at(1)==QString::fromUtf16(u"\u00c8\u00c9\u00ca\u00cb\u00cc\u00cd\u00ce\u00cf=+"),"accent capital row 2: E-grave..I-diaeresis then = +");
    check(capitalRows.at(2)==QString::fromUtf16(u"\u00d1\u00d2\u00d3\u00d4\u00d5\u00d6\u00d8\u0152~\""),"accent capital row 3: N-tilde..O-slash, OE ligature, tilde, quote");
    check(capitalRows.at(3)==QString::fromUtf16(u"\u00df\u00d9\u00da\u00db\u00dc\u00dd\u0178-_/"),"accent capital row 4: sharp s, U-grave..Y-diaeresis, then - _ /");
    // The module carries each capital as the case pair of the lower glyph, except the sharp s (kept lower on both pages).
    bool pairs=true;
    for(int r=0;r<4;++r)for(int c=0;c<10;++c) {
        const QChar lo=lowerRows.at(r).at(c),up=capitalRows.at(r).at(c);
        if(lo.isLetter())pairs=pairs && (lo.unicode()==0x00df ? up==lo : up==lo.toUpper());
    }
    check(pairs,"accent: every letter key's capital is its upper-case form (sharp s stays)");
    bool bmp=true;int glyphs=0;
    for(const QString& row:lowerRows+capitalRows)for(const QChar ch:row){++glyphs;bmp=bmp && !ch.isSurrogate() && !ch.isNull();}
    check(bmp && glyphs==80,"accent: 80 single-code-unit glyphs, no blanks (a key inserts exactly its label)");
    check(oskAccentModeCaption(false)==QString(QChar(0x00e0)) && oskAccentModeCaption(true)==QString(QChar(0x00c0)),"accent mode-cell glyphs (inferred a-grave / A-grave)");

    check(oskPageForAction(pagesNamed({"tenkey"}),0,A::Shift)==-1,"a one-page layout has nowhere to go");
    check(oskPageForAction(pagesNamed({"a","b","c"}),0,A::Shift)==1,"unnamed pages fall back to the next-page step");

    check(oskFadeAlpha(0,true)==0.,"open fade starts at 0");
    check(oskFadeAlpha(oskOpenFadeMilliseconds,true)==1. && oskFadeAlpha(500,true)==1.,"open fade ends at 1 and stays");
    check(oskFadeAlpha(oskOpenFadeMilliseconds/2,true)>0.45 && oskFadeAlpha(oskOpenFadeMilliseconds/2,true)<0.55,"open fade is linear at its midpoint");
    check(oskFadeAlpha(0,false)==1.,"close fade starts at 1");
    check(oskFadeAlpha(oskCloseFadeMilliseconds,false)==0. && oskFadeAlpha(500,false)==0.,"close fade ends at 0");
    check(oskFadeAlpha(-5,true)==0. && oskFadeAlpha(-5,false)==1.,"a negative elapsed clamps");
    check(!oskFadeDone(oskOpenFadeMilliseconds-1,true) && oskFadeDone(oskOpenFadeMilliseconds,true),"open fade finishes at its duration");
    check(!oskFadeDone(oskCloseFadeMilliseconds-1,false) && oskFadeDone(oskCloseFadeMilliseconds,false),"close fade finishes at its duration");
    check(oskOpenFadeMilliseconds>=66 && oskOpenFadeMilliseconds<=96 && oskCloseFadeMilliseconds==104,"durations sit inside the audited ranges");

    std::printf("%d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
