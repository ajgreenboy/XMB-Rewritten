#include "osk_composition.h"
#include <QString>
#include <cstdio>

// The composition word under the caret (audit/OSK-RECORDED-SPEC-2026-11-22.md 2). Build with
// -DXMB_OSK_COMPOSITION_BUILD_TESTS=ON.
using namespace xmb;

static int failures=0;
static void check(bool ok,const char* what) {
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static bool is(const QString& text,int cursor,int start,int end) {
    const auto r=oskCompositionRange(text,cursor);
    return r.first==start && r.second==end;
}

int main() {
    check(is(QString(),0,0,0),"empty field has no composition");
    check(is(QStringLiteral("h"),1,0,1),"'h' is underlined");
    check(is(QStringLiteral("hi"),2,0,2),"'hi' is underlined");
    check(is(QStringLiteral("hi "),3,3,3),"a space ends the composition");
    check(is(QStringLiteral("hi there"),8,3,8),"only the last word is underlined");
    check(is(QStringLiteral("hi"),0,0,0),"caret at the start has none");
    check(is(QStringLiteral("hello"),2,0,2),"mid-word: only the part before the caret");
    check(is(QStringLiteral("hi-fi"),5,0,5),"an inner hyphen stays in the word");
    check(is(QStringLiteral("don't"),5,0,5),"an inner apostrophe stays in the word");
    check(is(QStringLiteral("hi-"),3,3,3),"a trailing hyphen is not a word character");
    check(is(QStringLiteral("'hi"),3,1,3),"a leading apostrophe is not a word character");
    check(is(QStringLiteral("a,b"),3,2,3),"punctuation ends the composition");
    check(is(QStringLiteral("abc"),99,0,3),"an out-of-range caret is clamped");
    check(is(QStringLiteral("abc"),-4,0,0),"a negative caret is clamped");
    check(is(QStringLiteral("caf\u00e9"),4,0,4),"accented letters are word characters");
    std::printf("%s\n",failures?"FAILED":"ALL PASSED");
    return failures?1:0;
}
