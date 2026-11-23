#pragma once

// Reader for the PS3 firmware's own predictive-text dictionaries (dev_flash/data/dic/Zi8Dat*.z8d, the Zi "eZiText"
// format read by vsh/module/ime_zi.sprx). The format and the walk are documented in audit/ZI8-DICTIONARY-FORMAT.md,
// reversed from ime_zi.prx (block accessor 0x1e9e4, node primitives 0x152e8 / 0x153cc / 0x155b0 / 0x155cc, the
// two-key index 0x15674 and the trie walk 0x89c0). The file is the owner's firmware, read at runtime from the
// configured dev_flash; nothing from it is bundled or written anywhere.
//
// Qt-free on purpose: tests/zi8_dictionary_tests.cpp drives it with synthetic dictionaries built in the test.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xmb {

class Zi8Dictionary {
public:
    // The firmware's walk keeps a frame per letter and refuses deeper input (ime_zi passes 0x40 as the depth bound).
    static constexpr int kMaxDepth=64;

    // Parses the header and the main trie's character table. Returns false (and leaves the dictionary empty) for a
    // file that is not a Zi8 alphabetic dictionary or whose blocks fall outside the file. The bytes are kept in memory.
    bool load(std::vector<std::uint8_t> bytes,std::string* error=nullptr);
    bool loaded() const { return root_!=0; }
    std::uint8_t languageId() const { return bytes_.empty()?0:bytes_[0]; }
    const std::u16string& characters() const { return chars_; }

    // Dictionary words of exactly the prefix's length that spell the prefix (the firmware's exact pass), in stored order.
    std::vector<std::u16string> exactMatches(const std::u16string& prefix,std::size_t limit) const;
    // Dictionary words longer than the prefix that start with it (the completion pass), in the trie's depth-first order,
    // which is the order the console lists them in. Matching is by character, as the full keyboard sends characters.
    std::vector<std::u16string> completions(const std::u16string& prefix,std::size_t limit) const;
    // Every word of the main trie in depth-first order (tests and the audit tooling; not used by the keyboard).
    std::vector<std::u16string> allWords(std::size_t limit) const;

private:
    struct Node {
        int flags=0;           // the firmware's flag word: 0x01 child, 0x02 child pointer, 0x04 sibling, 0x08 sibling pointer, 0x10 word end
        int index=0;           // index into the character table
        std::size_t child=0;   // offset of the first child (0 = none)
        std::size_t sibling=0; // offset of an explicit next sibling (0 = none or implicit)
        std::size_t end=0;     // offset just past this node's bytes
    };
    bool node(std::size_t at,Node& out) const;
    std::size_t nextSibling(std::size_t at) const;
    bool range(const std::u16string& prefix,std::size_t& start,std::size_t& stop) const;
    bool pointer(std::size_t base,std::size_t& at,std::size_t& out) const;
    std::uint32_t u16(std::size_t at) const { return (std::uint32_t(bytes_[at])<<8)|bytes_[at+1]; }
    std::uint32_t u24(std::size_t at) const { return (std::uint32_t(bytes_[at])<<16)|(std::uint32_t(bytes_[at+1])<<8)|bytes_[at+2]; }
    enum class Pass { Exact, Complete, All };
    std::vector<std::u16string> walk(const std::u16string& prefix,Pass pass,std::size_t limit) const;

    std::vector<std::uint8_t> bytes_;
    std::u16string chars_;               // the main trie's characters, index order
    std::vector<std::uint16_t> keys_;    // the phone-key code of each character (index block), empty without an index
    std::size_t blockEnd_=0;             // end of the main trie block
    std::size_t root_=0;                 // first root node
    std::size_t index_=0,indexEnd_=0;    // the two-key index entries [index_, indexEnd_), 0 when absent
};

// Lower-cases the characters the English dictionaries hold (ASCII and Latin-1 letters); everything else is returned
// unchanged. The dictionaries store lower-case words.
std::u16string zi8Fold(const std::u16string& text);

// The prediction list the console shows for a composition word (audit/ZI8-DICTIONARY-FORMAT.md 6): the typed word
// itself, then the exact-length dictionary words, then the completions, without repeating a word, at most `limit` lines.
// An empty word or an unloaded dictionary gives an empty list.
std::vector<std::u16string> zi8Candidates(const Zi8Dictionary& dictionary,const std::u16string& typed,std::size_t limit);

}  // namespace xmb
