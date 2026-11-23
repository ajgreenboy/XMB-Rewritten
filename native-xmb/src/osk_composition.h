#pragma once

// The on-screen keyboard's composition word: the run of word characters that ends at the caret. The recording
// (audit/OSK-RECORDED-SPEC-2026-11-22.md 2) underlines that run in the entry row while it is being typed and drops the
// underline as soon as a space is typed. Pure and header-only so tests/osk_composition_tests.cpp covers it without a
// window. The same range is the word the Prediction column completes (View::oskPredictions, audit/ZI8-DICTIONARY-FORMAT.md).

#include <QChar>
#include <QString>
#include <utility>

namespace xmb {

// A word character is a letter, a digit, or an apostrophe or hyphen that sits between two of those (the recorded
// candidates include "hi-fi"). UNKNOWN: whether the console treats digits as composition; the recording types none.
inline bool oskIsWordChar(const QString& text,int index) {
    const QChar c=text.at(index);
    if(c.isLetterOrNumber())return true;
    if(c!=QLatin1Char('\'') && c!=QLatin1Char('-'))return false;
    return index>0 && index+1<text.size() && text.at(index-1).isLetterOrNumber() && text.at(index+1).isLetterOrNumber();
}

// [start, end) of the word the caret ends, or an empty range (start == end == cursor) when the character before the
// caret is not a word character (empty field, caret after a space, caret at the start). Characters after the caret are
// not part of the composition: typing in the middle of a word underlines only the part before the caret.
inline std::pair<int,int> oskCompositionRange(const QString& text,int cursor) {
    cursor=qBound(0,cursor,int(text.size()));
    int start=cursor;
    while(start>0 && oskIsWordChar(text,start-1))--start;
    return {start,cursor};
}

}  // namespace xmb
