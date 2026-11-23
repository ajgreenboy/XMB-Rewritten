#include "zi8_dictionary.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

// The Zi8 predictive-text dictionary reader (audit/ZI8-DICTIONARY-FORMAT.md). Build with -DXMB_ZI8_BUILD_TESTS=ON.
//
// Every dictionary here is SYNTHETIC: the test builds a trie from its own word list and encodes it in the format the
// audit documents (header slots, character tables, node bytes, inline and pointer children and siblings, the two-key
// index). No firmware bytes are embedded. When XMB_DEV_FLASH points at a local dev_flash, an extra check reads the real
// English (UK) file and compares the candidates for "h" and "hi" with the real-console recording; CI leaves it unset.
using namespace xmb;

static int failures=0;
static void check(bool ok,const std::string& what) {
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what.c_str());
    if(!ok)++failures;
}
static std::u16string u(const char* text) {
    std::u16string out;
    for(const char* p=text;*p;++p)out.push_back(char16_t(static_cast<unsigned char>(*p)));
    return out;
}
static std::string narrow(const std::u16string& text) {
    std::string out;
    for(char16_t c:text)out.push_back(c<0x80?char(c):'?');
    return out;
}
static std::string join(const std::vector<std::u16string>& words) {
    std::string out;
    for(const auto& w:words) { if(!out.empty())out+=' '; out+=narrow(w); }
    return out;
}

// ---- a synthetic encoder for the documented format ----

namespace {

constexpr int kFlags[16]={0x10,0x01,0x03,0x14,0x05,0x07,0x1c,0x0d,0x0f,0x11,0x13,0x15,0x17,0x1d,0x1f,0x35};

struct Trie {
    struct Node { char16_t c=0; bool end=false; std::vector<int> kids; };
    std::vector<Node> nodes{Node()};
    // Adds a word. newGroup starts a fresh root-level node even when one with the same letter exists (the firmware's
    // files repeat root letters: their root list is a run of groups, not one node per letter).
    void add(const std::u16string& word,bool newGroup=false) {
        int at=0;
        for(std::size_t i=0;i<word.size();++i) {
            int found=-1;
            if(!(i==0 && newGroup)) {
                const auto& kids=nodes[std::size_t(at)].kids;
                for(auto it=kids.rbegin();it!=kids.rend();++it)
                    if(nodes[std::size_t(*it)].c==word[i]) { found=*it; break; }
                if(i==0 && found!=-1 && found!=nodes[0].kids.back())found=-1;  // only the latest root group extends
            }
            if(found==-1) {
                nodes.push_back(Node());
                nodes.back().c=word[i];
                found=int(nodes.size()-1);
                nodes[std::size_t(at)].kids.push_back(found);
            }
            at=found;
        }
        nodes[std::size_t(at)].end=true;
    }
    void dfs(int at,std::u16string& word,std::vector<std::u16string>& out) const {
        for(int kid:nodes[std::size_t(at)].kids) {
            word.push_back(nodes[std::size_t(kid)].c);
            if(nodes[std::size_t(kid)].end)out.push_back(word);
            dfs(kid,word,out);
            word.pop_back();
        }
    }
    // A root group: the first word starts a new root node, the rest extend it.
    void group(std::initializer_list<const char*> list) {
        bool first=true;
        for(const char* w:list) { std::u16string s; for(const char* p=w;*p;++p)s.push_back(char16_t(static_cast<unsigned char>(*p))); add(s,first); first=false; }
    }
    std::vector<std::u16string> words() const { std::vector<std::u16string> out; std::u16string w; dfs(0,w,out); return out; }
};

struct Options {
    bool childInline=true;       // children follow their node (else a pointer to a list placed after the parent's list)
    bool siblingPointer=false;   // siblings through a pointer (else implicit)
    std::size_t rootGap=0;       // filler bytes before the second root node (forces a 3-byte sibling pointer)
    std::size_t childGap=0;      // filler bytes before the root nodes' child lists (forces 3-byte child pointers)
};

struct Encoder {
    const Trie& trie;
    std::u16string chars;
    Options options;
    std::vector<std::uint8_t> out;
    std::vector<std::size_t> rootPositions;   // trie offset of each root-level node, relative to the trie start
    std::size_t trieStart=0;

    int index(char16_t c) const { return int(chars.find(c)); }
    static int nibble(int want) {
        for(int i=0;i<15;++i)if(kFlags[i]==want)return i;
        std::printf("encoder: no nibble for flags %x\n",want);
        std::exit(2);
    }
    void patch(std::size_t node,std::size_t at,std::size_t target,bool wide) {
        const std::size_t value=target-node;
        if(wide) {
            if(value<0x8000) { std::printf("encoder: wide pointer too short\n"); std::exit(2); }
            const std::size_t raw=value-0x8000;
            out[at]=std::uint8_t(0x80|((raw>>16)&0x7f));out[at+1]=std::uint8_t(raw>>8);out[at+2]=std::uint8_t(raw);
        } else {
            if(value==0 || value>=0x8000) { std::printf("encoder: pointer out of range\n"); std::exit(2); }
            out[at]=std::uint8_t(value>>8);out[at+1]=std::uint8_t(value);
        }
    }
    void list(const std::vector<int>& nodes,int depth) {
        struct Pending { std::size_t node,at; int kid; bool wide; };
        std::vector<Pending> childPatches;
        bool sibPending=false;
        std::size_t sibNode=0,sibAt=0;
        bool sibWide=false;
        for(std::size_t i=0;i<nodes.size();++i) {
            const auto& n=trie.nodes[std::size_t(nodes[i])];
            if(depth==0 && i==1 && options.rootGap)out.insert(out.end(),options.rootGap,std::uint8_t(0xee));
            const std::size_t pos=out.size();
            if(depth==0)rootPositions.push_back(pos-trieStart);
            if(sibPending) { patch(sibNode,sibAt,pos,sibWide); sibPending=false; }
            const bool kids=!n.kids.empty(),last=i+1==nodes.size();
            int want=(n.end?0x10:0)|(kids?(options.childInline?0x01:0x03):0)|(!last?(options.siblingPointer?0x0c:0x04):0);
            const int nib=nibble(want);
            const int idx=index(n.c);
            if(idx<15)out.push_back(std::uint8_t(nib<<4|idx));
            else { out.push_back(std::uint8_t(nib<<4|15)); out.push_back(std::uint8_t(idx-15)); }
            if(kids && !options.childInline) {
                const bool wide=depth==0 && options.childGap>0;
                childPatches.push_back({pos,out.size(),nodes[i],wide});
                out.insert(out.end(),wide?3:2,std::uint8_t(0));
            }
            if(!last && options.siblingPointer) {
                sibWide=depth==0 && i==0 && options.rootGap>0;
                sibPending=true; sibNode=pos; sibAt=out.size();
                out.insert(out.end(),sibWide?3:2,std::uint8_t(0));
            }
            if(kids && options.childInline)list(n.kids,depth+1);
        }
        if(depth==0 && options.childGap && !childPatches.empty())out.insert(out.end(),options.childGap,std::uint8_t(0xee));
        for(const auto& p:childPatches) {
            patch(p.node,p.at,out.size(),p.wide);
            list(trie.nodes[std::size_t(p.kid)].kids,depth+1);
        }
    }
};

struct IndexEntry { std::uint16_t key; std::size_t start,stop; std::vector<IndexEntry> kids; };

void put16(std::vector<std::uint8_t>& b,std::size_t v) { b.push_back(std::uint8_t(v>>8)); b.push_back(std::uint8_t(v)); }
void put24(std::vector<std::uint8_t>& b,std::size_t v) { b.push_back(std::uint8_t(v>>16)); b.push_back(std::uint8_t(v>>8)); b.push_back(std::uint8_t(v)); }
std::size_t countEntries(const std::vector<IndexEntry>& e) { std::size_t n=0; for(const auto& x:e)n+=1+countEntries(x.kids); return n; }
void putEntries(std::vector<std::uint8_t>& b,const std::vector<IndexEntry>& e) {
    for(const auto& x:e) { put16(b,countEntries(x.kids)); put16(b,x.key); put24(b,x.start); put24(b,x.stop); putEntries(b,x.kids); }
}

// A whole file: 196-byte header of (size, offset) slots, the main block at 0xc8, then the index block if any.
std::vector<std::uint8_t> buildFile(const Trie& trie,const std::u16string& chars,Options options,
                                    std::vector<std::size_t>* rootPositions=nullptr,
                                    const std::vector<std::uint16_t>* keys=nullptr,
                                    const std::vector<IndexEntry>* index=nullptr,bool badIndexHeader=false) {
    std::vector<std::uint8_t> block;
    put16(block,1);
    put16(block,chars.size()*3);
    for(char16_t c:chars)put16(block,c);
    for(char16_t c:chars)block.push_back(std::uint8_t(c));
    Encoder enc{trie,chars,options,{},{},0};
    enc.out=block;
    enc.trieStart=block.size();
    enc.list(trie.nodes[0].kids,0);
    block=enc.out;
    if(rootPositions)*rootPositions=enc.rootPositions;
    std::vector<std::uint8_t> indexBlock;
    if(keys && index) {
        put16(indexBlock,keys->size());
        for(auto k:*keys)put16(indexBlock,k);
        put16(indexBlock,badIndexHeader?1:0); put16(indexBlock,0); put16(indexBlock,countEntries(*index)); put16(indexBlock,0); put16(indexBlock,0);
        putEntries(indexBlock,*index);
    }
    std::vector<std::uint8_t> file(0xc8,0);
    file[0]=0x3a; file[1]=0x80;
    const auto slot=[&](std::size_t k,std::size_t size,std::size_t offset) {
        file[4+k*6]=std::uint8_t(size>>16);file[5+k*6]=std::uint8_t(size>>8);file[6+k*6]=std::uint8_t(size);
        file[7+k*6]=std::uint8_t(offset>>16);file[8+k*6]=std::uint8_t(offset>>8);file[9+k*6]=std::uint8_t(offset);
    };
    slot(0,block.size(),0xc8);
    if(!indexBlock.empty())slot(1,indexBlock.size(),0xc8+block.size());
    file.insert(file.end(),block.begin(),block.end());
    file.insert(file.end(),indexBlock.begin(),indexBlock.end());
    return file;
}

std::vector<std::u16string> referenceExact(const std::vector<std::u16string>& all,const std::u16string& p) {
    std::vector<std::u16string> out; for(const auto& w:all)if(w==p)out.push_back(w); return out;
}
std::vector<std::u16string> referenceCompletions(const std::vector<std::u16string>& all,const std::u16string& p) {
    std::vector<std::u16string> out; for(const auto& w:all)if(w.size()>p.size() && w.compare(0,p.size(),p)==0)out.push_back(w); return out;
}

// 30 characters, so indices 15..29 take the extension byte.
const std::u16string kChars=u("etaoinshrdlucmfwypvbgkqjxz-'.");

Trie sampleTrie() {
    Trie t;
    // Two root groups for 'h' and a repeated 't' group, the shape the firmware files have.
    t.group({"the","then","them","they","to","too","top"});
    t.group({"he","her","here","hen"});
    t.group({"hat","hats","hatch","ha","halo","hall"});
    t.group({"thy","tea","team","teams"});
    for(const char* w:{"zebra","jazz","quay","yo-yo","won't","x.y","wax","box","fizz","vex","pygmy","juxtapose"})t.group({w});
    return t;
}

}  // namespace

static void modeTest(const char* name,Options options) {
    const Trie trie=sampleTrie();
    const auto file=buildFile(trie,kChars,options);
    Zi8Dictionary d;
    std::string error;
    check(d.load(file,&error),std::string(name)+": loads ("+error+")");
    const auto all=trie.words();
    check(d.allWords(1000)==all,std::string(name)+": depth-first word list matches the trie ("+join(d.allWords(1000))+")");
    for(const char* p:{"h","he","ha","hat","t","th","te","z","yo-","won","x.","q","hx"}) {
        const std::u16string prefix=u(p);
        check(d.exactMatches(prefix,100)==referenceExact(all,prefix),std::string(name)+": exact '"+p+"'");
        check(d.completions(prefix,100)==referenceCompletions(all,prefix),std::string(name)+": completions '"+p+"' = "+join(d.completions(prefix,100)));
    }
    check(d.completions(u("t"),3).size()==3,std::string(name)+": completion limit holds");
}

int main() {
    modeTest("inline children, implicit siblings",{true,false,0});
    modeTest("child pointers, implicit siblings",{false,false,0});
    modeTest("child and sibling pointers",{false,true,0});
    modeTest("inline children, sibling pointers",{true,true,0});
    modeTest("3-byte sibling pointer across a 40 KB gap",{true,true,0xa000});
    modeTest("3-byte child pointers across a 36 KB gap",{false,true,0,0x9000});
    modeTest("3-byte child pointers, implicit siblings",{false,false,0,0x8800});

    // Order is the stored depth-first order, not alphabetical and not by length.
    {
        const Trie trie=sampleTrie();
        Zi8Dictionary d; d.load(buildFile(trie,kChars,{}));
        check(join(d.completions(u("h"),100))=="he her here hen ha hat hats hatch halo hall","completions keep the stored order across root groups: "+join(d.completions(u("h"),100)));
        check(join(d.exactMatches(u("ha"),100))=="ha","exact pass reports only the typed length");
        check(join(d.completions(u("ha"),100))=="hat hats hatch halo hall","completion pass skips the typed length itself");
        check(join(zi8Candidates(d,u("he"),100))=="he her here hen","candidates: the typed word, then completions");
        check(join(zi8Candidates(d,u("hx"),100))=="hx","candidates: an unknown word still lists itself");
        check(join(zi8Candidates(d,u("ha"),4))=="ha hat hats hatch","candidates: typed word not repeated, limit holds");
        check(join(zi8Candidates(d,u("HA"),3))=="HA hat hats","candidates: matching folds case, the typed word stays as typed");
        check(zi8Candidates(d,std::u16string(),10).empty(),"candidates: empty word gives nothing");
        check(d.characters()==kChars && d.languageId()==0x3a,"character table and language id are read");
        check(d.completions(u("hats"),10).empty(),"no completions past a leaf");
        check(d.completions(u("€"),10).empty(),"a character outside the table matches nothing");
        Zi8Dictionary none;
        check(zi8Candidates(none,u("he"),10).empty(),"candidates: no dictionary, empty column");
    }

    // The two-key index: a walk starts at the entry's node and ends at its stop node.
    {
        Trie t;
        t.group({"tan","tab"});   // root 0: t  (key 2)
        t.group({"sat","sap"});   // root 1: s  (key 3)
        t.group({"sun","sup"});   // root 2: s  (key 3)
        t.group({"tin"});         // root 3: t  (key 2) - outside the key-2 range on purpose
        std::vector<std::size_t> roots;
        buildFile(t,kChars,{},&roots);
        std::vector<std::uint16_t> keys(kChars.size(),0xeff1);
        keys[kChars.find(u't')]=0xeff2;
        keys[kChars.find(u's')]=0xeff3;
        keys[kChars.find(u'u')]=0xeff8;
        keys[kChars.find(u'a')]=0xeff7;
        std::vector<IndexEntry> index{
            {0xeff2,roots[0],roots[1],{}},
            {0xeff3,roots[1],roots[3],{{0xeff7,roots[1],roots[2],{}},{0xeff8,roots[2],roots[3],{}}}},
        };
        Zi8Dictionary d;
        std::string error;
        check(d.load(buildFile(t,kChars,{},nullptr,&keys,&index),&error),"index fixture loads ("+error+")");
        check(join(d.completions(u("t"),10))=="tan tab","index: the key-2 range stops before the later 't' group");
        check(join(d.completions(u("s"),10))=="sat sap sun sup","index: one key covers both 's' groups: "+join(d.completions(u("s"),10)));
        check(join(d.completions(u("sa"),10))=="sat sap","index: the second key narrows the range: "+join(d.completions(u("sa"),10)));
        check(join(d.completions(u("su"),10))=="sun sup","index: the second key's own range: "+join(d.completions(u("su"),10)));
        check(d.completions(u("o"),10).empty(),"index: a key without an entry lists nothing");
        Zi8Dictionary bad;
        bad.load(buildFile(t,kChars,{},nullptr,&keys,&index,true));
        check(bad.loaded() && bad.completions(u("s"),10).empty(),"index: a non-zero index header gives no candidates (as ime_zi)");
    }

    // Malformed input never crashes or hangs.
    {
        Zi8Dictionary d;
        check(!d.load({}),"empty file is refused");
        check(!d.load(std::vector<std::uint8_t>(0xc8,0)),"header without a main block is refused");
        auto file=buildFile(sampleTrie(),kChars,{});
        auto truncated=file; truncated.resize(truncated.size()/2);
        check(!d.load(truncated),"a main block past the end of the file is refused");
        std::uint32_t seed=12345;
        int loadedCount=0;
        for(int round=0;round<400;++round) {
            auto mutated=file;
            for(int k=0;k<8;++k) {
                seed=seed*1103515245u+12345u;
                const std::size_t at=0xc8+4+kChars.size()*3+(seed>>8)%(mutated.size()-0xc8-4-kChars.size()*3);
                seed=seed*1103515245u+12345u;
                mutated[at]=std::uint8_t(seed>>16);
            }
            Zi8Dictionary m;
            if(m.load(mutated)) { ++loadedCount; m.allWords(5000); m.completions(u("h"),100); m.exactMatches(u("the"),10); }
        }
        check(loadedCount==400,"400 corrupted tries load and walk without crashing");
    }

    // Opt-in: the owner's real dictionary against the real-console recording (audit/OSK-RECORDED-SPEC-2026-11-22.md 4).
    if(const char* flash=std::getenv("XMB_DEV_FLASH")) {
        const std::string path=std::string(flash)+"/data/dic/Zi8DatENUK.z8d";
        std::ifstream in(path,std::ios::binary);
        if(!in) std::printf("SKIP  real dictionary: %s not readable\n",path.c_str());
        else {
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
            Zi8Dictionary d;
            std::string error;
            check(d.load(bytes,&error),"real ENUK dictionary loads ("+error+")");
            check(join(zi8Candidates(d,u("hi"),8))=="hi his him himself hi-fi hibernate hibernates hibernation",
                  "real ENUK 'hi' = the recording's eight lines: "+join(zi8Candidates(d,u("hi"),8)));
            check(join(zi8Candidates(d,u("h"),8))=="h here had have has ha hack habit",
                  "real ENUK 'h' = the recording's eight lines: "+join(zi8Candidates(d,u("h"),8)));
        }
    } else std::printf("SKIP  real dictionary: XMB_DEV_FLASH not set\n");

    std::printf("%s\n",failures?"FAILED":"ALL PASS");
    return failures?1:0;
}
