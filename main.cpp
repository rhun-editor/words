// words: completes the word being typed with the words of the open documents, the most used
// first, and counts a document's distinct words. C++17 and the standard library on rhunext.h
// (https://github.com/rhun-editor/extension-protocol/tree/main/c).
#define RHUNEXT_IMPLEMENTATION
#include "rhunext.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

const size_t kMaxItems = 500;   // the most items one answer offers
const size_t kMinWord = 3;      // bytes
const size_t kMaxWord = 128;    // longer runs (hashes, base64) are not words anyone types

using Counts = std::unordered_map<std::string, int>;

// a document rhun reported with `opened`
struct Doc {
    std::string path;       // "-" when untitled
    int64_t version = 0;    // the latest version an event named
    int64_t counted = -1;   // the version `counts` was made from; -1 for none yet
    Counts counts;
};

struct State {
    std::map<rx_doc, Doc> docs;     // by id, which is the order they opened in
};

bool word_byte(unsigned char c) {
    return c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}

// whether s holds whole, well-formed UTF-8 characters only (a Latin-1 file's bytes make no label)
bool valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t more;
        if (c < 0x80) more = 0;
        else if (c >= 0xc2 && c <= 0xdf) more = 1;
        else if (c >= 0xe0 && c <= 0xef) more = 2;
        else if (c >= 0xf0 && c <= 0xf4) more = 3;
        else return false;
        if (n - i <= more) return false;
        for (size_t k = 1; k <= more; k++)
            if ((s[i + k] & 0xc0) != 0x80) return false;
        unsigned char c1 = more ? s[i + 1] : 0;
        if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 > 0x9f) || (c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 > 0x8f))
            return false;   // overlong, a surrogate, past U+10FFFF
        i += more + 1;
    }
    return true;
}

// calls f(start, end) for each word of the text: a run of letters, digits, _ and bytes of
// non-ASCII characters, three bytes or longer, not starting with a digit
template <class F>
void each_word(const unsigned char *t, size_t len, F f) {
    size_t i = 0;
    while (i < len) {
        if (!word_byte(t[i])) {
            i++;
            continue;
        }
        size_t s = i;
        while (i < len && word_byte(t[i])) i++;
        size_t n = i - s;
        if (n >= kMinWord && n <= kMaxWord && !(t[s] >= '0' && t[s] <= '9') && valid_utf8(t + s, n)) f(s, i);
    }
}

// the byte offset of a 1-based line and byte column, clamped to the text
size_t offset_of(const unsigned char *t, size_t len, rx_pos at) {
    size_t off = 0;
    for (int line = 1; line < at.line && off < len; line++) {
        while (off < len && t[off] != '\n') off++;
        if (off < len) off++;
    }
    size_t end = off;
    while (end < len && t[end] != '\n') end++;
    size_t col = at.col > 1 ? static_cast<size_t>(at.col - 1) : 0;
    return std::min(off + col, end);
}

Counts count_words(const unsigned char *t, size_t len) {
    Counts c;
    each_word(t, len, [&](size_t s, size_t e) { c[std::string(reinterpret_cast<const char *>(t) + s, e - s)]++; });
    return c;
}

// whether the typed characters appear in the word in order, ASCII letters in either case: the
// words rhun's list can still show once it filters by what was typed (it matches loosely: "ap"
// keeps "grape")
bool may_match(const std::string &word, const std::string &typed) {
    size_t k = 0;
    for (size_t i = 0; i < word.size() && k < typed.size(); i++) {
        unsigned char a = static_cast<unsigned char>(word[i]), b = static_cast<unsigned char>(typed[k]);
        if (a < 0x80) a = static_cast<unsigned char>(a >= 'A' && a <= 'Z' ? a + 32 : a);
        if (b < 0x80) b = static_cast<unsigned char>(b >= 'A' && b <= 'Z' ? b + 32 : b);
        if (a == b) k++;
    }
    return k == typed.size();
}

std::string file_name(const std::string &path) {
    if (path.empty() || path == "-") return "untitled";
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// the counts of another open document, fetched again only when an event named a newer version
const Counts *counts_of(rx_ext *ext, rx_doc id, Doc &d) {
    if (d.counted < 0 || d.counted != d.version) {
        size_t len = 0;
        int64_t version = 0;
        unsigned char *text = rx_text(ext, id, &len, &version);
        if (!text) return nullptr;      // closed meanwhile: its closed event is on the way
        d.counts = count_words(text, len);
        d.counted = version;
        d.version = std::max(d.version, version);
        std::free(text);
    }
    return &d.counts;
}

struct Item {
    std::string word;
    int uses;               // in this document, or in the others when only there
    std::string where;      // "" for this document, else the other file's name
};

int on_complete(rx_ext *ext, const rx_ask *req, rx_buf *out, void *user) {
    State *st = static_cast<State *>(user);
    rx_list list{};
    size_t len = 0;
    int64_t version = 0;
    unsigned char *text = rx_text(ext, req->doc, &len, &version);
    if (!text) {                // not a document any more: nothing to offer
        rx_list_finish(&list, out);
        return 1;
    }
    // what is being typed: the word from its start up to the cursor
    size_t cur = offset_of(text, len, req->at);
    rx_pos start = rx_word_start(text, len, req->at);
    size_t from = cur - std::min(cur, static_cast<size_t>(req->at.col - start.col));
    std::string typed(reinterpret_cast<const char *>(text) + from, cur - from);

    // this document's words, leaving out the one at the cursor
    Counts here;
    each_word(text, len, [&](size_t s, size_t e) {
        if (s < cur && cur <= e) return;
        here[std::string(reinterpret_cast<const char *>(text) + s, e - s)]++;
    });
    std::free(text);

    std::vector<Item> items;
    for (auto &kv : here) items.push_back({kv.first, kv.second, ""});

    // the other documents' words that this one lacks, named after the file that uses them most
    std::unordered_map<std::string, size_t> elsewhere;      // word -> its index in items
    std::unordered_map<std::string, int> best;              // word -> uses in that file
    for (auto &kv : st->docs) {
        if (kv.first == req->doc) continue;
        const Counts *c = counts_of(ext, kv.first, kv.second);
        if (!c) continue;
        std::string name = file_name(kv.second.path);
        for (auto &w : *c) {
            if (here.count(w.first)) continue;
            auto it = elsewhere.find(w.first);
            if (it == elsewhere.end()) {
                elsewhere.emplace(w.first, items.size());
                best[w.first] = w.second;
                items.push_back({w.first, w.second, name});
                continue;
            }
            Item &item = items[it->second];
            item.uses += w.second;
            if (w.second > best[w.first]) {
                best[w.first] = w.second;
                item.where = name;
            }
        }
    }

    // All of them when they fit, and rhun filters as the user types. When they do not, only those
    // that can match what is typed, the most used, and the list says it is incomplete: rhun asks
    // again as typing goes on (a backspace included), and the next answer fits the word by then.
    if (items.size() > kMaxItems) {
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &i) { return !may_match(i.word, typed); }),
                    items.end());
        list.more = 1;
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        return a.uses != b.uses ? a.uses > b.uses : a.word < b.word;
    });
    if (items.size() > kMaxItems) items.resize(kMaxItems);
    rx_range r = {start, req->at};
    for (const Item &item : items) {
        std::string detail = item.where.empty()
            ? std::to_string(item.uses) + (item.uses == 1 ? " use" : " uses")
            : "in " + item.where;
        rx_list_item(&list, r, "text", item.word.c_str(), nullptr, detail.c_str(), nullptr);
    }
    rx_list_finish(&list, out);
    return 1;
}

void on_command(rx_ext *ext, const char *name, rx_doc doc, void *) {
    if (std::string(name) != "stats") return;
    if (doc == 0) {
        rx_toast(ext, "Open a document first");
        return;
    }
    size_t len = 0;
    unsigned char *text = rx_text(ext, doc, &len, nullptr);
    if (!text) {
        std::string msg = std::string("Could not read the document: ") + rx_error(ext);
        rx_toast(ext, msg.c_str());
        return;
    }
    std::unordered_set<std::string> distinct;
    each_word(text, len, [&](size_t s, size_t e) { distinct.emplace(reinterpret_cast<const char *>(text) + s, e - s); });
    std::free(text);
    size_t n = distinct.size();
    std::string msg = std::to_string(n) + (n == 1 ? " distinct word" : " distinct words");
    rx_toast(ext, msg.c_str());
}

void on_event(rx_ext *, const rx_event *ev, void *user) {
    State *st = static_cast<State *>(user);
    std::string name = ev->name;
    if (name == "opened" && ev->argc >= 4) {
        // opened DOC VERSION LANG PATH: the path is the rest of the line after three fields
        const char *path = ev->rest;
        for (int k = 0; k < 3 && path; k++) {
            path = std::strchr(path, ' ');
            if (path) path++;
        }
        Doc &d = st->docs[std::atoll(ev->argv[0])];
        d = Doc();
        d.path = path ? path : "-";
        d.version = std::atoll(ev->argv[1]);
    } else if (name == "closed" && ev->argc >= 1) {
        st->docs.erase(std::atoll(ev->argv[0]));
    } else if ((name == "edits" || name == "resync" || name == "changed" || name == "saved") && ev->argc >= 2) {
        auto it = st->docs.find(std::atoll(ev->argv[0]));
        if (it != st->docs.end()) it->second.version = std::max(it->second.version, static_cast<int64_t>(std::atoll(ev->argv[1])));
    }
}

}  // namespace

int main() {
    static State st;
    rx_handlers h{};
    h.event = on_event;
    h.command = on_command;
    h.complete = on_complete;
    return rx_run("complete", &h, &st);
}
