#pragma once

// Choosing a candidate in the on-screen keyboard's Prediction column. Every rule here is the firmware's, traced in the 4.93
// modules (audit/OSK-FIRMWARE.md 17): the panel side in oskfullkeypanel_plugin.prx, the selection arithmetic in ime.prx (the
// decrypted vsh/module/ime.sprx). Qt-free and header-only so tests/zi8_dictionary_tests.cpp drives it without a window.
//
// * The candidate list is one focus element (id 0x43) in the two columns right of the key grid (focus table 0x41c80), enabled
//   only while it has candidates (0x70c). Moving into it with nothing chosen asks the IME for the next candidate from -1, i.e.
//   the first (0x317c8 -> event 0x8000102).
// * Up / Down while it has focus are the IME's previous / next candidate (0x8000103 / 0x8000102). ime.prx steps the index by
//   one and wraps at either end on a fresh press; an auto-repeat stops at the end instead (0x1cec0 / 0x1d200).
// * L2 / R2 while it has focus are the IME's page up / page down (0x8000113 / 0x8000112): the index moves by the panel's lines
//   per page and is clamped to the first / last candidate, no wrap (ime.prx 0x8000112 handler).
// * The panel shows one page of lines; the page is the one holding the chosen candidate (0x36078: first index / lines per
//   page), the knob is the strip's height divided by the page count and sits at that page (0x36078 attributes 0x16 / 0x14).
// * Cross commits the chosen candidate (0x317c8 -> 0x800000d; ime.prx commits it through the converter and clears the list).
#include <cstddef>
#include <string>

namespace xmb {

// The recording shows eight lines in the column; the panel reports its lines per page to the IME (vtable +0x30).
constexpr int kOskPredictLinesPerPage=8;

// ime.prx 0x8000102 (direction +1) / 0x8000103 (direction -1). From -1 (nothing chosen) "next" gives the first candidate.
inline int oskPredictStep(int index,int count,int direction,bool repeat) {
    if(count<=0)return -1;
    if(direction>0) {
        const int next=index+1;
        if(next<count)return next;
        return repeat?index:0;
    }
    const int previous=index-1;
    if(previous>=0)return previous;
    return repeat?index:count-1;
}

// ime.prx 0x8000112 (direction +1, R2) / 0x8000113 (direction -1, L2): one page, clamped, never wrapping.
inline int oskPredictPageStep(int index,int count,int perPage,int direction) {
    if(count<=0 || perPage<=0)return -1;
    int next=(index<0?0:index)+(direction>0?perPage:-perPage);
    if(next>=count)next=count-1;
    if(next<0)next=0;
    return next;
}

inline int oskPredictPageCount(int count,int perPage) {
    return (count<=0 || perPage<=0)?0:(count+perPage-1)/perPage;
}

// The first line of the page the panel shows: the page holding the chosen candidate, the first page when none is chosen.
inline int oskPredictPageFirst(int index,int perPage) {
    return (index<0 || perPage<=0)?0:(index/perPage)*perPage;
}

// The scroll knob over a strip of trackHeight starting at trackTop (0x36078: size = strip / pages, position = page * size).
struct OskPredictKnob { double top=0,height=0; bool visible=false; };
inline OskPredictKnob oskPredictKnob(double trackTop,double trackHeight,int count,int perPage,int index) {
    OskPredictKnob knob;
    const int pages=oskPredictPageCount(count,perPage);
    if(pages<=0)return knob;
    knob.visible=true;
    knob.height=trackHeight/pages;
    knob.top=trackTop+knob.height*double(oskPredictPageFirst(index,perPage)/perPage);
    return knob;
}

// Committing a candidate: the composition word [wordStart, cursor) is replaced by the candidate and the caret ends after it.
// The returned composeFloor is where the next composition may start: the committed word is no longer being composed (ime.prx
// clears the list on the commit), so it is neither underlined nor completed again until something is typed after it.
struct OskPredictCommit { std::u16string text; int cursor=0; int composeFloor=-1; };
inline OskPredictCommit oskPredictCommit(const std::u16string& text,int wordStart,int cursor,const std::u16string& candidate) {
    OskPredictCommit out;
    const int size=int(text.size());
    if(cursor<0)cursor=0;
    if(cursor>size)cursor=size;
    if(wordStart<0)wordStart=0;
    if(wordStart>cursor)wordStart=cursor;
    out.text=text.substr(0,std::size_t(wordStart))+candidate+text.substr(std::size_t(cursor));
    out.cursor=wordStart+int(candidate.size());
    out.composeFloor=out.cursor;
    return out;
}

// The composition word's start once a commit has happened: nothing before the floor is composed again while the caret is at or
// after it (the caller clears the floor when the caret or a deletion moves before it).
inline int oskPredictComposeStart(int start,int cursor,int composeFloor) {
    if(composeFloor>=0 && cursor>=composeFloor && start<composeFloor)return composeFloor;
    return start;
}

}  // namespace xmb
