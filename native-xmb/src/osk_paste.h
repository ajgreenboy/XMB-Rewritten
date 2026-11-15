#pragma once
#include <QString>

// Clipboard paste into the on-screen keyboard (audit/OSK-PASTE.md). This is the pure part: raw clipboard text in, the text that
// may be inserted out, with no window, no clipboard and no logging, so tests/osk_paste_tests.cpp can drive it directly.
//
// The pasted text is never logged anywhere, masked field or not: callers log counts and verdict names only.
namespace xmb::oskpaste {

// More than this many UTF-16 units on the clipboard is refused outright (nothing is inserted, nothing is scanned): the largest
// field is 4,096 units, so a larger clipboard is a file, a log or a mistake, and a refusal beats a stall.
constexpr int kMaxClipboardUnits=65536;

enum class Verdict {
    Inserted,     // all of the (cleaned) text fits
    Truncated,    // the text was cut at the field's limit (on a code point boundary); the part that fits is inserted
    Empty,        // nothing left after cleaning (empty clipboard, or only line breaks and control characters)
    NoRoom,       // the field is already full
    TooLarge      // the clipboard text is over kMaxClipboardUnits
};

struct Request {
    QString raw;               // the clipboard text
    int currentLength=0;       // UTF-16 units already in the field
    int maxLength=0;           // the field's limit in UTF-16 units
    bool multiline=false;      // false for every field this build has; true keeps line breaks (as "\n")
};

struct Result {
    Verdict verdict=Verdict::Empty;
    QString text;              // what to insert at the cursor (empty unless Inserted or Truncated)
    int characters=0;          // code points in `text`
    int removed=0;             // control characters dropped (not counting line breaks and tabs, which become spaces or "\n")
};

// Cleaning, in order:
//  1. CR and LF (and U+2028/U+2029) at both ends are trimmed; spaces and tabs at the ends are kept, since a value may start
//     with a space (a password) and the owner's paste is verbatim apart from the line breaks a copy usually drags in.
//  2. Inside the text, CRLF, CR, LF, U+2028 and U+2029 each become one space (single-line field) or one "\n" (multiline); a tab
//     becomes one space. Other control characters (below U+0020, U+007F..U+009F) are dropped.
//  3. The result is cut to the room left (maxLength - currentLength), never splitting a surrogate pair.
Result prepare(const Request& request);

// The on-screen confirmation or refusal for a result ("Pasted 12 characters", "Clipboard is empty"). `hasText` false reports a
// clipboard that holds no text at all (an image or files); pass the Result of prepare() otherwise.
QString message(const Result& result);
QString messageNoText();

}
