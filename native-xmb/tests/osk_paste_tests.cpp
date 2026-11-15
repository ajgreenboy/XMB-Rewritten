#include "osk_paste.h"
#include <QCoreApplication>
#include <QString>
#include <cstdio>

// The pure part of clipboard paste into the on-screen keyboard (audit/OSK-PASTE.md): line-break trimming, tab and control
// handling, single-line versus multiline, the field limit (never splitting a surrogate pair), the size refusal and the
// messages. No window and no clipboard. Build with -DXMB_OSK_PASTE_BUILD_TESTS=ON; exit code 0 = all pass.
namespace op=xmb::oskpaste;

static int failures=0;
static void check(bool ok,const char* what) {
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static op::Result run(const QString& raw,int current=0,int max=4096,bool multiline=false) {
    return op::prepare({raw,current,max,multiline});
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    using V=op::Verdict;

    auto r=run(QStringLiteral("eyJhbGciOiJIUzI1NiJ9.token"));
    check(r.verdict==V::Inserted && r.text==QStringLiteral("eyJhbGciOiJIUzI1NiJ9.token") && r.characters==26,"a plain token passes through unchanged");
    check(op::message(r)==QStringLiteral("Pasted 26 characters"),"confirmation names the character count");
    check(op::message(run(QStringLiteral("x")))==QStringLiteral("Pasted 1 character"),"one character is singular");

    r=run(QStringLiteral("\r\n\r\nhttp://host:8123/path\r\n"));
    check(r.text==QStringLiteral("http://host:8123/path"),"CR and LF at both ends are trimmed");
    r=run(QStringLiteral("  spaced value \t"));
    check(r.text==QStringLiteral("  spaced value  "),"spaces and tabs at the ends are kept (a tab becomes a space)");
    r=run(QStringLiteral("a\r\nb\rc\nd"));
    check(r.text==QStringLiteral("a b c d"),"single line: each inner line break becomes one space (CRLF is one break)");
    r=run(QStringLiteral("a\r\nb\rc\nd"),0,4096,true);
    check(r.text==QStringLiteral("a\nb\nc\nd"),"multiline: inner line breaks become LF");
    r=run(QStringLiteral("a\tb"));
    check(r.text==QStringLiteral("a b"),"a tab becomes a space");
    r=run(QStringLiteral("a b c"));
    check(r.text==QStringLiteral("a b c"),"Unicode line and paragraph separators count as line breaks");
    r=run(QStringLiteral("a\x01\x1b[31m\x7f\u0085z"));
    check(r.text==QStringLiteral("a[31mz") && r.removed==4,"other control characters are dropped and counted");

    check(run(QString()).verdict==V::Empty && op::message(run(QString()))==QStringLiteral("Clipboard is empty"),"an empty clipboard is reported");
    check(run(QStringLiteral("\r\n\r\n")).verdict==V::Empty,"only line breaks is empty");
    check(run(QStringLiteral("\x01\x02")).verdict==V::Empty,"only control characters is empty");
    check(op::messageNoText()==QStringLiteral("Clipboard has no text"),"the no-text message");

    r=run(QStringLiteral("abcdefghij"),0,4);
    check(r.verdict==V::Truncated && r.text==QStringLiteral("abcd") && r.characters==4,"over the limit: cut at the limit");
    check(op::message(r)==QStringLiteral("Pasted 4 characters (field limit reached)"),"truncation is said on screen");
    r=run(QStringLiteral("abcdefghij"),30,32);
    check(r.verdict==V::Truncated && r.text==QStringLiteral("ab"),"the limit counts what the field already holds");
    r=run(QStringLiteral("abc"),4,4);
    check(r.verdict==V::NoRoom && r.text.isEmpty() && op::message(r)==QStringLiteral("The field is full"),"a full field takes nothing");
    r=run(QStringLiteral("abc"),9,4);
    check(r.verdict==V::NoRoom,"an over-full field (already past the limit) takes nothing");
    r=run(QStringLiteral("abc"),3,6);
    check(r.verdict==V::Inserted && r.text==QStringLiteral("abc"),"exactly filling the field is not a truncation");

    const QString pair=QString::fromUcs4(U"a\U0001F600b");   // a, a surrogate pair, b: 4 UTF-16 units
    r=run(pair,0,2);
    check(r.verdict==V::Truncated && r.text==QStringLiteral("a"),"the limit never splits a surrogate pair");
    r=run(pair,0,3);
    check(r.text==pair.left(3) && r.characters==2,"a pair that fits is kept whole and counts as one character");
    r=run(QString::fromUcs4(U"\U0001F600"),0,1);
    check(r.verdict==V::NoRoom,"a lone pair that cannot fit leaves nothing");

    QString big(op::kMaxClipboardUnits,QLatin1Char('a'));
    check(run(big,0,100000).verdict==V::Inserted,"the size cap itself is accepted");
    big+=QLatin1Char('a');
    r=run(big);
    check(r.verdict==V::TooLarge && r.text.isEmpty() && op::message(r)==QStringLiteral("Clipboard text is too large to paste"),
          "text over the cap is refused with a message");
    QString huge(5000000,QLatin1Char('x'));
    check(run(huge).verdict==V::TooLarge,"a 5 MB clipboard is refused");

    std::printf("%s\n",failures?"FAILED":"all passed");
    return failures?1:0;
}
