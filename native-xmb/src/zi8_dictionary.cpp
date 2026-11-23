#include "zi8_dictionary.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

// See audit/ZI8-DICTIONARY-FORMAT.md for the layout and for the firmware addresses each step mirrors.

namespace xmb {
namespace {

// ime_zi.prx's node flag table (16 halfwords at 0x2fa60, reached through TOC-0x7f54): the high nibble of a node's first
// byte selects one. 0x01 has a child, 0x02 the child is reached through a pointer (else it follows the node), 0x04 has a
// next sibling, 0x08 the sibling is reached through a pointer (else it follows the node's subtree), 0x10 a word ends here.
// 0x20 (entry 15 only) is not used by any node of the English files and is not interpreted.
constexpr int kNodeFlags[16]={0x10,0x01,0x03,0x14,0x05,0x07,0x1c,0x0d,0x0f,0x11,0x13,0x15,0x17,0x1d,0x1f,0x35};
constexpr int kChild=0x01,kChildPointer=0x02,kSibling=0x04,kSiblingPointer=0x08,kWordEnd=0x10;

constexpr std::size_t kHeaderSlots=32;          // (size, offset) pairs of 24-bit big-endian values from byte 4
constexpr std::size_t kHeaderBytes=4+6*kHeaderSlots;
constexpr std::size_t kMainSlot=0,kIndexSlot=1;  // the trie and its two-key index (ime_zi walks slot n with slot n+1)
// A walk that visits more nodes than this is treated as a malformed file (the English tries hold ~31k words in ~100k nodes).
constexpr std::size_t kStepBudget=4'000'000;
constexpr std::uint8_t kChineseLanguageId=0x01;

bool fail(std::string* error,const char* message) {
    if(error)*error=message;
    return false;
}

}  // namespace

char16_t zi8FoldChar(char16_t c) {
    // The scripts the alphabetic dictionaries' character tables use (D: every table is ASCII, Latin-1, Latin Extended-A or
    // Cyrillic): Basic Latin, Latin-1 (not the multiplication sign; Y-diaeresis lives in Extended-A), Latin Extended-A's
    // case pairs, Cyrillic.
    if(c>=u'A' && c<=u'Z')return char16_t(c+32);
    if(c>=0xc0 && c<=0xde && c!=0xd7)return char16_t(c+32);
    if(c==0x178)return 0xff;                                        // Y-diaeresis
    if(c>=0x100 && c<=0x137 && !(c&1))return char16_t(c+1);         // A-macron .. K-cedilla (upper case even)
    if(c>=0x139 && c<=0x148 && (c&1))return char16_t(c+1);          // L-acute .. N-caron (upper case odd)
    if(c>=0x14a && c<=0x177 && !(c&1))return char16_t(c+1);         // Eng .. Y-circumflex, OE (upper case even)
    if(c>=0x179 && c<=0x17e && (c&1))return char16_t(c+1);          // Z-acute .. Z-caron (upper case odd)
    if(c>=0x410 && c<=0x42f)return char16_t(c+0x20);                // Cyrillic A .. YA
    if(c>=0x400 && c<=0x40f)return char16_t(c+0x50);                // Cyrillic IE-grave .. DZHE (IO among them)
    return c;
}

std::u16string zi8Fold(const std::u16string& text) {
    std::u16string out=text;
    for(char16_t& c:out)c=zi8FoldChar(c);
    return out;
}

std::string zi8DictionaryCode(const std::string& language,const std::string& territory) {
    // The alphabetic dictionaries dev_flash/data/dic carries (D: Zi8Dat<code>.z8d) by the ISO 639 language and ISO 3166
    // territory of the system locale. Regional files: English US/UK, French Canada/Europe, Spanish Spain/Latin America,
    // Portuguese Brazil/Europe. Languages without a file here (Japanese uses ATOK, Chinese a different block layout, the
    // rest none) fall back to English, as the keyboard did before it read any other file.
    const auto is=[&](const char* code){ return language==code; };
    if(is("fr"))return territory=="CA"?"FRCA":"FREU";
    if(is("es"))return territory=="ES"?"ESEU":"ESSA";
    if(is("pt"))return territory=="BR"?"PTBZ":"PTEU";
    if(is("de"))return "DE";
    if(is("it"))return "IT";
    if(is("nl"))return "NL";
    if(is("da"))return "DA";
    if(is("sv"))return "SV";
    if(is("fi"))return "FI";
    if(is("nb") || is("nn") || is("no"))return "NO";
    if(is("pl"))return "PL";
    if(is("ru"))return "RU";
    return territory=="US"?"ENAM":"ENUK";
}

bool Zi8Dictionary::load(std::vector<std::uint8_t> bytes,std::string* error) {
    *this=Zi8Dictionary();
    if(bytes.size()<kHeaderBytes)return fail(error,"file is shorter than the Zi8 header");
    // Language id 1 is the Chinese files (D): their block 0 is a different record layout (12-byte records, an alternate
    // slot table when byte 3 > 3), which this reader does not decode.
    if(bytes[0]==kChineseLanguageId)return fail(error,"Chinese (id 1) dictionaries use a different block layout");
    bytes_=std::move(bytes);
    const auto slotSize=[&](std::size_t slot){ return std::size_t(u24(4+slot*6)); };
    const auto slotOffset=[&](std::size_t slot){ return std::size_t(u24(7+slot*6)); };
    const std::size_t size=slotSize(kMainSlot),block=slotOffset(kMainSlot);
    if(size<4 || block<kHeaderBytes || block+size>bytes_.size()) {
        *this=Zi8Dictionary();
        return fail(error,"main block lies outside the file");
    }
    // The main block: a halfword (1 in the English files), the byte length of the two character tables (3 per character:
    // UTF-16BE then an 8-bit copy), the tables, then the trie's first root node (ime_zi 0x15650).
    const std::size_t tableBytes=u16(block+2);
    if(tableBytes==0 || tableBytes%3!=0 || block+4+tableBytes>=block+size) {
        *this=Zi8Dictionary();
        return fail(error,"character table is malformed (not an alphabetic Zi8 dictionary)");
    }
    const std::size_t count=tableBytes/3;
    for(std::size_t i=0;i<count;++i)chars_.push_back(char16_t(u16(block+4+2*i)));
    blockEnd_=block+size;
    root_=block+4+tableBytes;
    // The two-key index (ime_zi 0x15674): the phone key of every character, then a table of 10-byte entries
    // {child count, key, start (24-bit, from the root), stop (24-bit, from the root; 0 = none)} nested two keys deep.
    const std::size_t indexSize=slotSize(kIndexSlot),index=slotOffset(kIndexSlot);
    if(indexSize!=0) {
        if(index<kHeaderBytes || index+indexSize>bytes_.size() || indexSize<2) {
            *this=Zi8Dictionary();
            return fail(error,"index block lies outside the file");
        }
        const std::size_t keys=u16(index);
        const std::size_t base=index+2+keys*2;
        if(base+10>index+indexSize) {
            *this=Zi8Dictionary();
            return fail(error,"index block is truncated");
        }
        for(std::size_t i=0;i<keys;++i)keys_.push_back(std::uint16_t(u16(index+2+2*i)));
        index_=base;
        indexEnd_=std::min(index+indexSize,base+10+std::size_t(u16(base+4))*10);
    }
    return true;
}

bool Zi8Dictionary::pointer(std::size_t base,std::size_t& at,std::size_t& out) const {
    // A forward offset from the node's first byte: two bytes when the top bit is clear, otherwise three bytes holding
    // (value & 0x7fffff) + 0x8000 (ime_zi 0x152e8 / 0x153cc).
    if(at>=blockEnd_)return false;
    std::size_t value=0;
    if(bytes_[at]&0x80) {
        if(at+3>blockEnd_)return false;
        value=(std::size_t(bytes_[at]&0x7f)<<16|std::size_t(bytes_[at+1])<<8|bytes_[at+2])+0x8000;
        at+=3;
    } else {
        if(at+2>blockEnd_)return false;
        value=u16(at);
        at+=2;
    }
    if(value==0 || base+value>=blockEnd_)return false;
    out=base+value;
    return true;
}

bool Zi8Dictionary::node(std::size_t at,Node& out) const {
    // One node (ime_zi 0x155cc for the character, 0x152e8 for the child): the low nibble is the character index, 15
    // meaning "15 + the next byte"; then the child pointer if any, then the sibling pointer if any; an implicit child
    // starts right after those bytes.
    if(at<root_ || at>=blockEnd_)return false;
    const std::uint8_t first=bytes_[at];
    out=Node();
    out.flags=kNodeFlags[first>>4];
    out.index=first&0x0f;
    std::size_t cursor=at+1;
    if(out.index==15) {
        if(cursor>=blockEnd_)return false;
        out.index=15+bytes_[cursor++];
    }
    if(std::size_t(out.index)>=chars_.size())return false;
    if((out.flags&(kChild|kChildPointer))==(kChild|kChildPointer) && !pointer(at,cursor,out.child))return false;
    if((out.flags&kSiblingPointer) && !pointer(at,cursor,out.sibling))return false;
    if((out.flags&(kChild|kChildPointer))==kChild) {
        if(cursor>=blockEnd_)return false;
        out.child=cursor;
    }
    out.end=cursor;
    return true;
}

std::size_t Zi8Dictionary::nextSibling(std::size_t at) const {
    // ime_zi 0x153cc: an explicit sibling is the pointer; an implicit one follows this node's subtree, found by walking
    // the bytes and counting inline levels (a node with an inline child opens one, a node that ends its list or whose
    // sibling is elsewhere closes one).
    Node n;
    if(!node(at,n) || !(n.flags&kSibling))return 0;
    if(n.flags&kSiblingPointer)return n.sibling;
    int depth=0;
    std::size_t cursor=at;
    for(std::size_t guard=0;guard<kStepBudget;++guard) {
        Node m;
        if(!node(cursor,m))return 0;
        cursor=m.end;
        if((m.flags&(kChild|kChildPointer))==kChild)++depth;
        if(!(m.flags&kSibling) || (m.flags&kSiblingPointer))--depth;
        if(depth<1)return cursor<blockEnd_?cursor:0;
    }
    return 0;
}

bool Zi8Dictionary::range(const std::u16string& prefix,std::size_t& start,std::size_t& stop) const {
    start=root_;
    stop=0;
    if(keys_.empty() && index_==0)return true;
    // ime_zi 0x15674 refuses an index whose first two halfwords are not zero: no candidates at all.
    if(u16(index_)!=0 || u16(index_+2)!=0)return false;
    std::size_t first=index_+10,end=indexEnd_;
    for(std::size_t level=0;level<2 && level<prefix.size();++level) {
        const std::size_t character=chars_.find(prefix[level]);
        if(character==std::u16string::npos || character>=keys_.size())return false;
        const std::uint16_t key=keys_[character];
        std::size_t entry=first;
        while(entry+10<=end && u16(entry+2)!=key)entry+=std::size_t(u16(entry))*10+10;
        if(entry+10>end)return false;
        start=root_+u24(entry+4);
        const std::size_t stopOffset=u24(entry+7);
        stop=stopOffset==0?0:root_+stopOffset;
        if(start>=blockEnd_)return false;
        const std::size_t children=u16(entry);
        if(children==0)break;
        first=entry+10;
        end=std::min(end,entry+10+children*10);
    }
    return true;
}

std::vector<std::u16string> Zi8Dictionary::walk(const std::u16string& rawPrefix,Pass pass,std::size_t limit) const {
    std::vector<std::u16string> out;
    if(!loaded() || limit==0)return out;
    const std::u16string prefix=zi8Fold(rawPrefix);
    const std::size_t length=prefix.size();
    if(pass!=Pass::All && length==0)return out;
    if(length>=std::size_t(kMaxDepth))return out;
    std::size_t start=root_,stop=0;
    if(pass!=Pass::All && !range(prefix,start,stop))return out;

    // The firmware's walk (ime_zi 0x89c0) keeps one frame per letter: at a depth inside the typed word a node must carry
    // that letter; the exact pass reports a word end at the word's own length and does not go deeper; the completion pass
    // goes on and reports every word end below it. Reaching the index entry's stop node ends the whole walk.
    struct Frame { std::size_t node; };
    std::vector<Frame> frames;
    std::u16string word;
    std::size_t steps=0;
    frames.push_back({start});
    word.push_back(0);
    while(!frames.empty() && out.size()<limit && ++steps<kStepBudget) {
        const std::size_t at=frames.back().node;
        const std::size_t depth=frames.size();
        bool descend=false;
        Node n;
        if(at==0 || (stop!=0 && at==stop) || !node(at,n))break;
        word.back()=chars_[std::size_t(n.index)];
        if(pass==Pass::All) {
            if(n.flags&kWordEnd)out.push_back(word);
            descend=true;
        } else if(depth<=length) {
            if(chars_[std::size_t(n.index)]==prefix[depth-1]) {
                if(depth<length)descend=true;
                else if(pass==Pass::Exact) {
                    if(n.flags&kWordEnd)out.push_back(word);
                } else descend=true;
            }
        } else {
            if(n.flags&kWordEnd)out.push_back(word);
            descend=true;
        }
        if(descend && (n.flags&kChild) && depth<std::size_t(kMaxDepth)) {
            frames.push_back({n.child});
            word.push_back(0);
            continue;
        }
        // Next sibling, or pop until a level has one.
        for(;;) {
            const std::size_t next=nextSibling(frames.back().node);
            if(next!=0) { frames.back().node=next; break; }
            frames.pop_back();
            word.pop_back();
            if(frames.empty())break;
        }
    }
    return out;
}

std::vector<std::u16string> Zi8Dictionary::exactMatches(const std::u16string& prefix,std::size_t limit) const {
    return walk(prefix,Pass::Exact,limit);
}

std::vector<std::u16string> Zi8Dictionary::completions(const std::u16string& prefix,std::size_t limit) const {
    return walk(prefix,Pass::Complete,limit);
}

std::vector<std::u16string> Zi8Dictionary::allWords(std::size_t limit) const {
    return walk(std::u16string(),Pass::All,limit);
}

std::vector<std::u16string> zi8Candidates(const Zi8Dictionary& dictionary,const std::u16string& typed,std::size_t limit) {
    std::vector<std::u16string> out;
    // No dictionary, no list: the column stays empty (the native build before the dictionary was read).
    if(typed.empty() || limit==0 || !dictionary.loaded())return out;
    std::unordered_set<std::u16string> seen;
    const auto add=[&](const std::u16string& word) {
        if(out.size()<limit && seen.insert(zi8Fold(word)).second)out.push_back(word);
    };
    // The typed word heads the list (recording: "h", not a dictionary word, is the first line). A word the dictionary has
    // nothing for lists nothing: the recording never shows that case, and an empty column is what the keyboard drew before.
    std::vector<std::u16string> found=dictionary.exactMatches(typed,limit);
    for(const auto& word:dictionary.completions(typed,limit))found.push_back(word);
    if(found.empty())return out;
    add(typed);
    for(const auto& word:found)add(word);
    return out;
}

}  // namespace xmb
