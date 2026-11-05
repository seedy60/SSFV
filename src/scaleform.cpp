#include "scaleform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common.h"

namespace sfva {
namespace {

// Function tables and field offsets, as module offsets, confirmed against the
// shipped build (v07.011). The same facts were first published by the SFV
// Access project. Every one of these is a guess about a particular build, so
// verifyBuild() checks the executable before any of them is trusted.
constexpr uintptr_t kDocViewVtable = 0x34C8568;
constexpr uintptr_t kStyledTextVtable = 0x34C8530;

constexpr uintptr_t kDocViewText = 0x10;      // StyledText*
constexpr uintptr_t kDocViewListener = 0x20;  // listener of the owning field
constexpr uintptr_t kListenerOwner = -0x98;   // back-pointer to that field

constexpr uintptr_t kTextParagraphs = 0x18;
constexpr uintptr_t kTextParagraphCount = 0x20;
constexpr uintptr_t kParagraphChars = 0x00;  // UTF-16, then a length including the terminator
constexpr uintptr_t kParagraphSize = 0x08;
constexpr uintptr_t kParagraphRuns = 0x20;      // format runs: start, length, TextFormat*
constexpr uintptr_t kParagraphRunCount = 0x28;
constexpr uintptr_t kRunStride = 0x18;
constexpr uintptr_t kFormatImage = 0x30;        // the picture drawn in place of a run's character
constexpr uintptr_t kImageUrl = 0x50;           // GFx String, tagged in its low two bits
constexpr uintptr_t kStringSize = 0x00;         // length in the low bits, a flag on top
constexpr uintptr_t kStringChars = 0x0C;
constexpr uint64_t kMaxRuns = 64;
constexpr uint64_t kMaxPictureUrl = 512;

constexpr uintptr_t kDisplayParent = 0x38;
constexpr uintptr_t kDisplayRenderNode = 0x48;
constexpr uintptr_t kDisplayChildren = 0xD8;  // array of display object pointers
constexpr uintptr_t kDisplayChildCount = 0xE0;
constexpr uintptr_t kDisplayChildStride = 0x10;
constexpr uint32_t kMaxChildren = 4096;
constexpr uintptr_t kNodeData = 0x10;
constexpr uintptr_t kNodeFlags = 0x0A;    // u16, bit 0 while visible
constexpr uintptr_t kNodeMatrix = 0x10;   // 2 by 4 affine, in twips
constexpr uintptr_t kNodeColour = 0x50;   // multiply r, g, b, a
constexpr uintptr_t kNodeBounds = 0x70;   // two rectangles of four floats
constexpr size_t kNodeBytes = kNodeBounds + 0x20;
constexpr uint16_t kNodeVisible = 0x0001;
constexpr uint16_t kMovieRootFlags = 0x1800;

constexpr float kTwipsPerPixel = 20.0f;
constexpr float kGold[3] = {1.0f, 0.89f, 0.549f};
constexpr float kGoldTolerance = 0.05f;
constexpr int kMaxDepth = 40;
constexpr uint64_t kMaxParagraphs = 512;
constexpr uint64_t kMaxParagraphChars = 4096;
constexpr uintptr_t kMaxRegion = 0x100000;  // Scaleform's pages are all smaller
constexpr uintptr_t kScaleformChunk = 0x10000;

// The read-write regions the sweep looks in. Walking the address space costs
// a few tens of milliseconds and used to stall the narrator (and the game with
// it) whenever a screen had many fields. Keep the list here and refresh it on
// a helper thread; a sweep takes a snapshot and reads while the walk runs.
struct ScanBlock {
    uintptr_t base = 0;
    size_t size = 0;
    bool shaped = false;
};

std::mutex g_blocksLock;
std::vector<ScanBlock> g_blocks;
std::atomic<bool> g_blocksWalking{false};
std::atomic<uint32_t> g_blocksAt{0};

void walkBlocks() {
    std::vector<ScanBlock> next;
    next.reserve(4096);
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (address < 0x7FFFFFFFFFFFULL &&
           VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) {
        const uintptr_t size = static_cast<uintptr_t>(mbi.RegionSize);
        const bool candidate =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && size > 0 &&
            size <= kMaxRegion &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
             mbi.Protect == PAGE_EXECUTE_READWRITE ||
             mbi.Protect == PAGE_EXECUTE_WRITECOPY);
        if (candidate) {
            // Text fields have only ever been found in allocations shaped like
            // Scaleform's: whole 64 KB chunks plus one page.
            const bool shaped = size > kScaleformChunk && size % kScaleformChunk == 0x1000;
            next.push_back({reinterpret_cast<uintptr_t>(mbi.BaseAddress), size, shaped});
        }
        address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (size ? size : 0x1000);
    }
    std::lock_guard<std::mutex> lock(g_blocksLock);
    g_blocks = std::move(next);
    g_blocksAt.store(GetTickCount());
    g_blocksWalking.store(false);
}

struct Affine {
    float sx, shx, tx, shy, sy, ty;
};

Affine compose(const Affine& outer, const Affine& inner) {
    return {
        outer.sx * inner.sx + outer.shx * inner.shy,
        outer.sx * inner.shx + outer.shx * inner.sy,
        outer.sx * inner.tx + outer.shx * inner.ty + outer.tx,
        outer.shy * inner.sx + outer.sy * inner.shy,
        outer.shy * inner.shx + outer.sy * inner.sy,
        outer.shy * inner.tx + outer.sy * inner.ty + outer.ty,
    };
}

bool isPlaceholder(const std::wstring& text) {
    std::wstring letters;
    for (wchar_t c : text) {
        if (!iswspace(c)) letters.push_back(c);
    }
    if (letters.size() >= 3) {
        bool onlyW = true;
        for (wchar_t c : letters) {
            if (c != L'w' && c != L'W') {
                onlyW = false;
                break;
            }
        }
        if (onlyW) return true;
    }
    if (letters.size() < 8) return false;
    size_t ws = 0;
    for (wchar_t c : letters) {
        if (c == L'w' || c == L'W') ++ws;
    }
    return static_cast<double>(ws) >= 0.8 * static_cast<double>(letters.size());
}

bool plausibleFloat(float value) {
    return std::isfinite(value) && std::fabs(value) < 1e7f;
}

std::wstring collapseSpaces(const std::wstring& text) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : text) {
        if (iswspace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(L' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

void replaceAll(std::wstring& text, const std::wstring& from, const std::wstring& to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::wstring::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

// The game prints a bare numeral before a picture that already names a
// strength: "M" then a medium punch icon. The picture wins.
std::wstring commandText(std::wstring text) {
    replaceAll(text, L"(STANDING)", L"");
    replaceAll(text, L"(CROUCHING)", L"down plus");
    replaceAll(text, L"(CROUCH)", L"down plus");
    replaceAll(text, L"(JUMPING)", L"jump");
    replaceAll(text, L"(JUMP)", L"jump");
    if (text.find(L'+') != std::wstring::npos) replaceAll(text, L"+", L" plus ");
    return collapseSpaces(text);
}

const wchar_t* directionWord(wchar_t d) {
    switch (d) {
        case L'1': return L"down back";
        case L'2': return L"down";
        case L'3': return L"down forward";
        case L'4': return L"back";
        case L'5': return L"neutral";
        case L'6': return L"forward";
        case L'7': return L"up back";
        case L'8': return L"up";
        case L'9': return L"up forward";
        default: return nullptr;
    }
}

// Input pictures are named in the game's assets: cmd_236 is a quarter circle
// forward, cmd_2 a down arrow, punch_h a heavy punch. The names were mapped
// against a screenshot by the Street Fighter V Access project.
std::wstring inputWords(const std::string& name) {
    if (name == "plus") return L"plus";
    if (name == "next") return L",";
    if (name == "rapid") return L"rapidly";
    if (name.rfind("cmd_", 0) == 0) {
        std::string digits = name.substr(4);
        const bool held = !digits.empty() && digits.back() == 'c';
        if (held) digits.pop_back();
        if (!held) {
            if (digits == "236") return L"quarter circle forward";
            if (digits == "214") return L"quarter circle back";
            if (digits == "41236") return L"half circle forward";
            if (digits == "63214") return L"half circle back";
            if (digits == "0") return L"full circle";
        }
        std::wstring words;
        for (char d : digits) {
            const wchar_t* word = directionWord(static_cast<wchar_t>(d));
            if (!word) return {};
            if (!words.empty()) words += L", ";
            words += word;
        }
        if (words.empty()) return {};
        if (held) words = L"hold " + words;
        return words;
    }
    const size_t underscore = name.find('_');
    const std::string button = name.substr(0, underscore);
    if (button != "punch" && button != "kick") return {};
    std::wstring words = wide(button);
    if (underscore == std::string::npos) return words;
    const std::string strength = name.substr(underscore + 1);
    if (strength == "l") return L"light " + words;
    if (strength == "m") return L"medium " + words;
    if (strength == "h") return L"heavy " + words;
    return {};
}

std::string pictureName(uintptr_t format) {
    if (!format) return {};
    uintptr_t desc = 0;
    if (!readValue(format + kFormatImage, desc) || !plausiblePointer(desc)) return {};
    uint64_t tagged = 0;
    if (!readValue(desc + kImageUrl, tagged) || !tagged) return {};
    const uintptr_t data = static_cast<uintptr_t>(tagged & ~3ULL);
    if (!plausiblePointer(data)) return {};
    uint64_t size = 0;
    if (!readValue(data + kStringSize, size)) return {};
    size &= 0x7FFFFFFFFFFFFFFFULL;
    if (!size || size > kMaxPictureUrl) return {};
    char raw[kMaxPictureUrl]{};
    if (!readRaw(data + kStringChars, raw, size)) return {};
    std::string url(raw, static_cast<size_t>(size));
    const size_t nul = url.find('\0');
    if (nul != std::string::npos) url.resize(nul);
    const size_t slash = url.find_last_of('/');
    const std::string file = slash == std::string::npos ? url : url.substr(slash + 1);
    const size_t dot = file.find('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

struct PicturePiece {
    bool picture = false;
    std::string name;
    std::wstring text;
};

bool buttonPhrase(const std::wstring& words) {
    static const wchar_t* kButtons[] = {L"punch",       L"kick",        L"light punch",
                                        L"medium punch", L"heavy punch", L"light kick",
                                        L"medium kick",  L"heavy kick"};
    for (const wchar_t* button : kButtons) {
        if (words == button) return true;
    }
    return false;
}

std::wstring describePieces(const std::vector<PicturePiece>& pieces) {
    std::wstring out;
    int previous = 0;  // 1 direction, 2 button, 3 plus, 4 next
    std::string previousName;
    std::wstring previousWords;
    for (const auto& piece : pieces) {
        std::wstring words;
        int kind = 0;
        if (piece.picture) {
            words = inputWords(piece.name);
            if (words.empty()) continue;
            if (piece.name == "plus") {
                kind = 3;
            } else if (piece.name == "next") {
                kind = 4;
            } else if (piece.name.rfind("cmd_", 0) == 0) {
                kind = 1;
            } else {
                kind = 2;
            }
        } else {
            words = commandText(piece.text);
            if (words.empty()) continue;
            if (buttonPhrase(words)) kind = 2;
        }

        if (kind == 3) {
            while (!out.empty() && out.back() == L' ') out.pop_back();
            out += L" plus ";
            previous = 3;
            previousName.clear();
            previousWords.clear();
            continue;
        }
        if (kind == 4) {
            while (!out.empty() && out.back() == L' ') out.pop_back();
            out += L", ";
            previous = 4;
            previousName.clear();
            previousWords.clear();
            continue;
        }
        // "M" before a medium punch picture is the same thing twice.
        if (kind == 2 && (words.rfind(L"light ", 0) == 0 || words.rfind(L"medium ", 0) == 0 ||
                          words.rfind(L"heavy ", 0) == 0)) {
            if (out.size() >= 2 && (out.back() == L'H' || out.back() == L'M' || out.back() == L'L') &&
                !iswalnum(out[out.size() - 2])) {
                out.pop_back();
                while (!out.empty() && out.back() == L' ') out.pop_back();
            }
        }
        if (!out.empty() && out.back() != L' ') {
            if (kind == 1 && previous == 1) {
                out += L", ";
            } else if (kind == 2 && previous == 2 &&
                       (piece.picture ? piece.name != previousName : words != previousWords)) {
                // Buttons drawn side by side are pressed together. The game
                // draws no plus picture between them, so supply the word.
                out += L" plus ";
            } else {
                out += L' ';
            }
        }
        out += words;
        previous = kind;
        previousName = piece.picture ? piece.name : std::string();
        previousWords = words;
    }
    return collapseSpaces(out);
}

bool describeParagraph(uintptr_t runs, uint64_t count, const std::wstring& text,
                       std::wstring& out) {
    if (!runs || !count || count > kMaxRuns) return false;
    std::vector<PicturePiece> pieces;
    bool anyPicture = false;
    size_t cursor = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t run[kRunStride]{};
        if (!readRaw(runs + i * kRunStride, run, sizeof(run))) continue;
        uint64_t start = 0, length = 0, format = 0;
        std::memcpy(&start, run, sizeof(start));
        std::memcpy(&length, run + 0x08, sizeof(length));
        std::memcpy(&format, run + 0x10, sizeof(format));
        if (!length || start >= text.size()) continue;
        const size_t from = static_cast<size_t>(start);
        if (from < cursor) continue;
        const size_t to = std::min(text.size(), from + static_cast<size_t>(length));
        if (to <= from) continue;
        if (from > cursor) pieces.push_back({false, {}, text.substr(cursor, from - cursor)});
        std::wstring chunk = text.substr(from, to - from);
        const std::string name = pictureName(static_cast<uintptr_t>(format));
        if (!name.empty() && !inputWords(name).empty()) {
            pieces.push_back({true, name, {}});
            anyPicture = true;
            if (chunk.size() > 1) pieces.push_back({false, {}, chunk.substr(1)});
        } else {
            pieces.push_back({false, {}, chunk});
        }
        cursor = to;
    }
    if (cursor < text.size()) pieces.push_back({false, {}, text.substr(cursor)});
    if (!anyPicture) return false;
    out = describePieces(pieces);
    return !out.empty();
}

}  // namespace

bool Field::gold() const {
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(tint[i] - kGold[i]) > kGoldTolerance) return false;
    }
    return true;
}

bool Field::shown() const {
    if (hidden || !rooted) return false;
    // Alpha is not reliable for menus. The six Arcade path cards, the character
    // select name and its description are drawn with an effective alpha of
    // zero while plainly on screen; one of their container layers carries a
    // zero colour that the renderer does not apply to the text. Hidden movies
    // and off-stage text are already rejected above, so a visible chain with a
    // real position is taken at face value.
    if (x < 0.0f || x >= 1920.0f || y < 0.0f || y >= 1080.0f) return false;
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return false;
    return !isPlaceholder(text);
}

Reader::Reader(uintptr_t moduleBase) : base_(moduleBase) {
    // Every text field the SFV Access project recorded lived in one of these
    // three block sizes. Larger blocks are mostly screens that have closed;
    // sweeping them was most of the cost of a quick scan.
    quickSizes_ = {0x11000, 0x21000, 0x31000};
}

bool Reader::verifyBuild() {
    if (!base_) return false;
    uintptr_t docViewVtableFunction = 0;
    uintptr_t styledVtableFunction = 0;
    if (!readValue(base_ + kDocViewVtable, docViewVtableFunction) ||
        !readValue(base_ + kStyledTextVtable, styledVtableFunction)) {
        logLine(L"build check: the vtable addresses are not readable");
        return false;
    }
    // PE image size from the in-memory headers, to prove the entries point
    // inside this executable rather than at a recycled address.
    uintptr_t imageSize = 0;
    uint32_t peOffset = 0;
    if (readValue(base_ + 0x3C, peOffset) && peOffset < 0x1000) {
        uint16_t magic = 0;
        readValue(base_ + peOffset + 24, magic);
        if (magic == 0x20B) {
            uint32_t size = 0;
            readValue(base_ + peOffset + 24 + 56, size);
            imageSize = size;
        }
    }
    if (!imageSize) {
        logLine(L"build check: could not read the PE headers");
        return false;
    }
    auto inside = [&](uintptr_t value) { return value >= base_ && value < base_ + imageSize; };
    if (!inside(docViewVtableFunction) || !inside(styledVtableFunction)) {
        logLine(L"build check: vtable contents do not look like code in this executable");
        return false;
    }
    logLine(L"build check: offsets match this executable");
    return true;
}

bool Reader::readNode(uintptr_t object, float matrix[6], float colour[4], uint16_t& flags,
                      uintptr_t& parent) {
    uint8_t links[0x40]{};
    if (!readRaw(object + kDisplayParent, links, sizeof(links))) return false;
    std::memcpy(&parent, links, sizeof(parent));
    uintptr_t entry = 0;
    std::memcpy(&entry, links + (kDisplayRenderNode - kDisplayParent), sizeof(entry));
    if (!plausiblePointer(entry)) return false;

    uintptr_t data = 0;
    if (!readValue(entry + kNodeData, data) || !plausiblePointer(data)) return false;

    uint8_t raw[kNodeBytes]{};
    if (!readRaw(data, raw, sizeof(raw))) return false;

    std::memcpy(&flags, raw + kNodeFlags, sizeof(flags));
    float values[8]{};
    std::memcpy(values, raw + kNodeMatrix, sizeof(values));
    float cx[4]{};
    std::memcpy(cx, raw + kNodeColour, sizeof(cx));
    if (!plausibleFloat(values[0]) || !plausibleFloat(values[1]) || !plausibleFloat(values[3]) ||
        !plausibleFloat(values[4]) || !plausibleFloat(values[5]) || !plausibleFloat(values[7]) ||
        !plausibleFloat(cx[0]) || !plausibleFloat(cx[1]) || !plausibleFloat(cx[2]) ||
        !plausibleFloat(cx[3])) {
        return false;
    }

    // A zero alpha with empty bounds is a container that never had anything
    // to fade, not invisible text: the Arcade opponent cards sit under one
    // while plainly on screen.
    float bounds[8]{};
    std::memcpy(bounds, raw + kNodeBounds, sizeof(bounds));
    bool emptyBounds = true;
    for (float value : bounds) {
        if (value != 0.0f) {
            emptyBounds = false;
            break;
        }
    }
    if (cx[3] <= 0.01f && emptyBounds) cx[3] = 1.0f;

    matrix[0] = values[0];
    matrix[1] = values[1];
    matrix[2] = values[3];
    matrix[3] = values[4];
    matrix[4] = values[5];
    matrix[5] = values[7];
    std::memcpy(colour, cx, sizeof(cx));
    return true;
}

bool Reader::readText(uintptr_t docview, std::wstring& out) {
    uintptr_t styled = 0;
    if (!readValue(docview + kDocViewText, styled) || !plausiblePointer(styled)) return false;

    uint8_t head[0x28]{};
    if (!readRaw(styled, head, sizeof(head))) return false;
    uintptr_t vtable = 0;
    std::memcpy(&vtable, head, sizeof(vtable));
    if (vtable != base_ + kStyledTextVtable) return false;

    uintptr_t paragraphs = 0;
    uint64_t count = 0;
    std::memcpy(&paragraphs, head + kTextParagraphs, sizeof(paragraphs));
    std::memcpy(&count, head + kTextParagraphCount, sizeof(count));
    if (count > kMaxParagraphs || (count && !plausiblePointer(paragraphs))) return false;

    out.clear();
    for (uint64_t i = 0; i < count; ++i) {
        uintptr_t paragraph = 0;
        if (!readValue(paragraphs + i * 8, paragraph) || !paragraph) continue;

        uint8_t paragraphHead[0x30]{};
        if (!readRaw(paragraph, paragraphHead, sizeof(paragraphHead))) continue;
        uintptr_t chars = 0;
        uint64_t size = 0;
        std::memcpy(&chars, paragraphHead + kParagraphChars, sizeof(chars));
        std::memcpy(&size, paragraphHead + kParagraphSize, sizeof(size));
        if (!plausiblePointer(chars) || !size || size > kMaxParagraphChars) continue;

        std::wstring text(size, L'\0');
        if (!readRaw(chars, text.data(), size * sizeof(wchar_t))) continue;

        // The field's own terminator is part of the length. A read while the
        // game is writing the paragraph finds stale pointers instead, which
        // decode as stray CJK; those fail this check and wait for the next read.
        if (text[size - 1] != L'\0' && text[size - 1] != L'\r') return false;
        if (text.find(L'\0') != std::wstring::npos && text.find(L'\0') < size - 1) return false;

        text.resize(size - 1);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();

        // A command field is mostly spaces with pictures drawn over them. Turn
        // the pictures into words before the whitespace check, or the field is
        // discarded as empty and the notation is never heard.
        uintptr_t runs = 0;
        uint64_t runCount = 0;
        std::memcpy(&runs, paragraphHead + kParagraphRuns, sizeof(runs));
        std::memcpy(&runCount, paragraphHead + kParagraphRunCount, sizeof(runCount));
        std::wstring described;
        if (describeParagraph(runs, runCount, text, described)) text = std::move(described);

        if (!out.empty()) out.push_back(L'\n');
        out += text;
    }
    if (out.find_first_not_of(L" \t\r\n") == std::wstring::npos) return false;
    return true;
}

bool Reader::readField(uintptr_t docview, Field& out) {
    std::wstring text;
    if (!readText(docview, text)) {
        ++failText_;
        return false;
    }

    uintptr_t listener = 0;
    if (!readValue(docview + kDocViewListener, listener) || !plausiblePointer(listener)) {
        ++failChain_;
        return false;
    }
    uintptr_t object = 0;
    if (!readValue(listener + kListenerOwner, object) || !plausiblePointer(object)) {
        ++failChain_;
        return false;
    }

    struct Node {
        float matrix[6];
        float colour[4];
        uint16_t flags;
    };
    std::vector<Node> nodes;
    std::vector<uintptr_t> chain;
    while (object && plausiblePointer(object) && chain.size() < kMaxDepth) {
        if (std::find(chain.begin(), chain.end(), object) != chain.end()) break;
        Node node{};
        uintptr_t parent = 0;
        if (!readNode(object, node.matrix, node.colour, node.flags, parent)) break;
        nodes.push_back(node);
        chain.push_back(object);
        object = parent;
    }
    if (chain.empty()) {
        ++failChain_;
        return false;
    }

    Affine world{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        Affine local{it->matrix[0], it->matrix[1], it->matrix[2],
                     it->matrix[3], it->matrix[4], it->matrix[5]};
        world = compose(world, local);
        for (int c = 0; c < 4; ++c) tint[c] *= it->colour[c];
    }

    bool hidden = false;
    std::vector<uintptr_t> hiddenBy;
    for (size_t i = 1; i < nodes.size(); ++i) {
        if (!(nodes[i].flags & kNodeVisible)) {
            hidden = true;
            hiddenBy.push_back(chain[i]);
        }
    }
    const bool rooted = (nodes.back().flags & kMovieRootFlags) == kMovieRootFlags;

    out = Field{};
    out.text = std::move(text);
    out.x = world.tx / kTwipsPerPixel;
    out.y = world.ty / kTwipsPerPixel;
    std::memcpy(out.tint, tint, sizeof(tint));
    out.hidden = hidden;
    out.hiddenBy = std::move(hiddenBy);
    out.rooted = rooted;
    out.docview = docview;
    out.chain = std::move(chain);
    return true;
}

void Reader::scan(bool wide) {
    LARGE_INTEGER start{}, end{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    const uintptr_t wanted = base_ + kDocViewVtable;
    std::vector<uintptr_t> found;
    std::vector<uint8_t> buffer;
    buffer.reserve(kMaxRegion);
    size_t blockCount = 0;
    size_t bytes = 0;

    // The address-space walk costs tens of milliseconds. It runs on a helper
    // thread and the sweep reads the last completed list, so a screen with
    // hundreds of fields no longer stalls the game while the list is rebuilt.
    std::vector<ScanBlock> blocks;
    {
        std::lock_guard<std::mutex> lock(g_blocksLock);
        blocks = g_blocks;
    }
    const uint32_t now = GetTickCount();
    if (blocks.empty() || now - g_blocksAt.load() >= 1000) {
        if (!g_blocksWalking.exchange(true)) {
            if (blocks.empty()) {
                walkBlocks();  // first sweep: have a list before reading
            } else {
                try {
                    std::thread(walkBlocks).detach();
                } catch (...) {
                    g_blocksWalking.store(false);
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_blocksLock);
        blocks = g_blocks;
    }

    for (const auto& block : blocks) {
        if (!wide) {
            if (!block.shaped ||
                std::find(quickSizes_.begin(), quickSizes_.end(), block.size) ==
                    quickSizes_.end()) {
                continue;
            }
        }
        ++blockCount;
        bytes += block.size;
        buffer.resize(block.size);
        if (readRaw(block.base, buffer.data(), block.size)) {
            const size_t words = block.size / sizeof(uint64_t);
            const uint64_t* values = reinterpret_cast<const uint64_t*>(buffer.data());
            for (size_t i = 0; i < words; ++i) {
                if (values[i] == wanted) {
                    found.push_back(block.base + i * 8);
                }
            }
        }
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());

    // A full sweep is the only one that can find text in a block size the
    // quick sweep does not know about. Remember that size so the next quick
    // sweep covers it without walking every read-write page every time.
    if (wide && !found.empty()) {
        for (uintptr_t docview : found) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(docview), &info, sizeof(info))) continue;
            const size_t block = static_cast<size_t>(info.RegionSize);
            if (block <= kScaleformChunk || block > kMaxRegion ||
                block % kScaleformChunk != 0x1000) {
                continue;
            }
            if (std::find(quickSizes_.begin(), quickSizes_.end(), block) != quickSizes_.end()) {
                continue;
            }
            quickSizes_.push_back(block);
            logLine(L"scan: text found in a 0x" + std::to_wstring(block) +
                    L" block; quick sweeps now include it");
        }
    }

    // Keep entries that vanished for a moment; a field being rewritten is not
    // a field that closed. Three misses in a row means it is gone.
    std::vector<uintptr_t> next;
    std::vector<int> nextMisses;
    for (size_t i = 0; i < docviews_.size(); ++i) {
        if (std::binary_search(found.begin(), found.end(), docviews_[i])) continue;
        int misses = i < misses_.size() ? misses_[i] + 1 : 1;
        if (misses < 3) {
            next.push_back(docviews_[i]);
            nextMisses.push_back(misses);
        }
    }
    for (uintptr_t docview : found) {
        next.push_back(docview);
        nextMisses.push_back(0);
    }
    docviews_ = std::move(next);
    misses_ = std::move(nextMisses);

    QueryPerformanceCounter(&end);
    scanMs_ = static_cast<int>((end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart);

    if (!docviews_.empty()) {
        sawFields_ = true;
        emptyScans_ = 0;
    } else {
        ++emptyScans_;
    }
    // One line per change keeps a long session's log readable; time and
    // count also go out every minute so a silent mod still leaves a trail.
    static size_t lastCount = SIZE_MAX;
    static uint32_t lastScanLog = 0;
    if (found.size() != lastCount || GetTickCount() - lastScanLog > 60000) {
        lastCount = found.size();
        lastScanLog = GetTickCount();
        logLine(L"scan (" + std::wstring(wide ? L"wide" : L"quick") + L"): " +
                std::to_wstring(found.size()) + L" found, " + std::to_wstring(docviews_.size()) +
                L" known, " + std::to_wstring(blockCount) + L" blocks, " +
                std::to_wstring(bytes / (1024 * 1024)) + L" MB, " +
                std::to_wstring(scanMs_) + L" ms");
    }
}

uint32_t Reader::childCount(uintptr_t object) {
    uint32_t count = 0;
    if (!readValue(object + kDisplayChildCount, count)) return 0;
    return count > kMaxChildren ? 0 : count;
}

std::vector<uintptr_t> Reader::childrenOf(uintptr_t object) {
    const uint32_t count = childCount(object);
    if (!count) return {};
    uintptr_t data = 0;
    if (!readValue(object + kDisplayChildren, data) || !plausiblePointer(data)) return {};
    std::vector<uintptr_t> children;
    children.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t child = 0;
        if (!readValue(data + i * kDisplayChildStride, child) || !plausiblePointer(child) ||
            (child & 7)) {
            continue;
        }
        // A real child names its parent. Garbage left by a torn-down screen
        // almost never does, so the walk stops rather than inventing rows.
        uintptr_t parent = 0;
        if (!readValue(child + kDisplayParent, parent) || parent != object) continue;
        children.push_back(child);
    }
    return children;
}

bool Reader::appearance(uintptr_t object, float colour[4], uint16_t& flags) {
    float matrix[6]{};
    uintptr_t parent = 0;
    return readNode(object, matrix, colour, flags, parent);
}

bool Reader::gridChosen(const Field& field) {
    // A grid label sits two levels below its cell. The cell holds a
    // background, the label and a checkmark slot; only the cell under the
    // cursor switches the checkmark on. The BGM grid leaves every label at
    // full brightness, so this is the only mark of its selection.
    if (field.chain.size() < 3) return false;
    const auto parts = childrenOf(field.chain[2]);
    if (parts.size() < 3) return false;
    float colour[4]{};
    uint16_t flags = 0;
    if (!appearance(parts[2], colour, flags)) return false;
    return (flags & kNodeVisible) != 0 && colour[3] > 0.01f;
}

namespace {

// The selection highlight is a full-row child, not a badge or a lock icon.
bool highlightBounds(uintptr_t object, float& width, float& height) {
    uintptr_t entry = 0;
    if (!readValue(object + kDisplayRenderNode, entry) || !plausiblePointer(entry)) return false;
    uintptr_t data = 0;
    if (!readValue(entry + kNodeData, data) || !plausiblePointer(data)) return false;
    float bounds[8]{};
    if (!readRaw(data + kNodeBounds, bounds, sizeof(bounds))) return false;
    float minX = bounds[0], maxX = bounds[0], minY = bounds[1], maxY = bounds[1];
    for (int i = 2; i < 8; i += 2) {
        if (!std::isfinite(bounds[i]) || !std::isfinite(bounds[i + 1])) return false;
        minX = std::min(minX, bounds[i]);
        maxX = std::max(maxX, bounds[i]);
        minY = std::min(minY, bounds[i + 1]);
        maxY = std::max(maxY, bounds[i + 1]);
    }
    width = maxX - minX;
    height = maxY - minY;
    return std::isfinite(width) && std::isfinite(height);
}

}  // namespace

void Reader::markListFocus(std::vector<Field>& shown) {
    // A focused list item owns a highlight child that every sibling hides.
    // Group fields by their shared parent and compare each container's direct
    // children. A full-width child visible on exactly one item is the cursor.
    struct Group {
        std::map<uintptr_t, std::vector<size_t>> fields;
    };
    std::map<uintptr_t, Group> groups;
    for (size_t i = 0; i < shown.size(); ++i) {
        const auto& chain = shown[i].chain;
        for (size_t level = 2; level + 1 < chain.size() && level <= 5; ++level) {
            groups[chain[level + 1]].fields[chain[level]].push_back(i);
        }
    }

    size_t bestScore = 0;
    std::vector<size_t> bestFields;
    for (const auto& group : groups) {
        const auto& items = group.second.fields;
        if (items.size() < 2) continue;

        // The pause menu mixes simple left rows with wider right-hand value
        // containers. Their child counts differ, so comparing every container
        // together rejects the whole group. Compare same-shape containers in
        // their own subgroup instead.
        struct PartSet {
            std::vector<size_t> indices;
            std::vector<uintptr_t> parts;
        };
        std::map<size_t, std::vector<PartSet>> byWidth;
        for (const auto& [container, indices] : items) {
            std::vector<uintptr_t> parts = childrenOf(container);
            if (parts.size() < 2) continue;
            byWidth[parts.size()].push_back({indices, std::move(parts)});
        }

        for (const auto& [width, partSets] : byWidth) {
            if (partSets.size() < 2) continue;
            for (size_t child = 0; child < width; ++child) {
                int visible = 0;
                size_t visibleIndex = 0;
                bool readable = true;
                for (size_t item = 0; item < partSets.size(); ++item) {
                    float colour[4]{};
                    uint16_t flags = 0;
                    if (!appearance(partSets[item].parts[child], colour, flags)) {
                        readable = false;
                        break;
                    }
                    if ((flags & kNodeVisible) && colour[3] > 0.01f) {
                        ++visible;
                        visibleIndex = item;
                    }
                }
                if (!readable || visible != 1) continue;

                float w = 0.0f, h = 0.0f;
                if (!highlightBounds(partSets[visibleIndex].parts[child], w, h)) continue;
                if (w < 100.0f || h < 10.0f) continue;
                const size_t score = static_cast<size_t>(w * h);
                if (score > bestScore) {
                    bestScore = score;
                    bestFields = partSets[visibleIndex].indices;
                }
            }
        }
    }

    for (size_t i : bestFields) shown[i].listSelected = true;
}

void Reader::markCommandRow(std::vector<Field>& shown) {
    commandRow_ = 0;
    const Field* footer = nullptr;
    for (const auto& field : shown) {
        if (field.text.find(L"commands assume") != std::wstring::npos) {
            footer = &field;
            break;
        }
    }
    if (!footer || footer->chain.empty()) return;

    // The pause menu stays on screen behind the command overlay and shares the
    // movie root, so the root alone does not say which rows belong to the
    // list. A command row shares a closer, non-root ancestor with the footer.
    // Requiring that ancestor keeps the menu's own rows (and their values) out
    // of the command-row search.
    auto inCommandPanel = [&](const Field& field) {
        for (size_t i = 1; i + 1 < field.chain.size(); ++i) {
            for (size_t j = 1; j + 1 < footer->chain.size(); ++j) {
                if (field.chain[i] == footer->chain[j]) return true;
            }
        }
        return false;
    };

    std::map<uintptr_t, std::vector<uintptr_t>> cache;
    auto kids = [&](uintptr_t object) -> const std::vector<uintptr_t>& {
        const auto found = cache.find(object);
        if (found != cache.end()) return found->second;
        return cache.emplace(object, childrenOf(object)).first->second;
    };

    // A move row is a holder of eight or more parts, every part a background
    // and a holder. Group the rows by the list that holds them.
    std::map<uintptr_t, std::vector<uintptr_t>> groups;
    for (const auto& field : shown) {
        if (&field == footer) continue;
        if (field.x < 430.0f || field.x >= 520.0f || field.y < 260.0f || field.y >= 740.0f) {
            continue;
        }
        if (!inCommandPanel(field)) continue;
        for (size_t level = 1; level < field.chain.size(); ++level) {
            const uintptr_t object = field.chain[level];
            const auto& parts = kids(object);
            if (parts.size() < 8) continue;
            bool row = true;
            for (uintptr_t part : parts) {
                if (kids(part).size() != 2) {
                    row = false;
                    break;
                }
            }
            if (!row) continue;
            uintptr_t parent = 0;
            if (readValue(object + kDisplayParent, parent) && plausiblePointer(parent)) {
                groups[parent].push_back(object);
            }
            break;
        }
    }

    for (auto& [parent, rows] : groups) {
        (void)parent;
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

        uintptr_t selected = 0;
        if (rows.size() == 1) {
            // A section of one move has nothing to compare; the only row is
            // the one the cursor is on.
            selected = rows[0];
        } else {
            std::vector<std::vector<uintptr_t>> parts;
            size_t width = 0;
            bool alike = true;
            for (uintptr_t row : rows) {
                parts.push_back(kids(row));
                if (parts.back().size() < 2) {
                    alike = false;
                    break;
                }
                if (!width) {
                    width = parts.back().size();
                } else if (parts.back().size() != width) {
                    alike = false;
                    break;
                }
            }
            if (!alike || width < 2) continue;

            std::vector<std::vector<bool>> looks(width, std::vector<bool>(rows.size(), false));
            bool readable = true;
            for (size_t i = 0; i < width && readable; ++i) {
                for (size_t j = 0; j < rows.size(); ++j) {
                    float colour[4]{};
                    uint16_t flags = 0;
                    if (!appearance(parts[j][i], colour, flags)) {
                        readable = false;
                        break;
                    }
                    looks[i][j] = (flags & kNodeVisible) && colour[3] > 0.01f;
                }
            }
            if (!readable) continue;

            if (rows.size() < 4) {
                // Few rows: only one part may differ at all, and it must be
                // shown on exactly one of them.
                int differing = -1;
                bool several = false;
                for (size_t i = 0; i < width; ++i) {
                    bool same = true;
                    for (size_t j = 1; j < rows.size(); ++j) {
                        if (looks[i][j] != looks[i][0]) {
                            same = false;
                            break;
                        }
                    }
                    if (same) continue;
                    if (differing >= 0) {
                        several = true;
                        break;
                    }
                    differing = static_cast<int>(i);
                }
                if (several || differing < 0) continue;
                int only = -1;
                int count = 0;
                for (size_t j = 0; j < rows.size(); ++j) {
                    if (looks[differing][j]) {
                        only = static_cast<int>(j);
                        ++count;
                    }
                }
                if (count != 1) continue;
                selected = rows[only];
            } else {
                for (size_t i = 0; i < width && !selected; ++i) {
                    int only = -1;
                    int count = 0;
                    for (size_t j = 0; j < rows.size(); ++j) {
                        if (looks[i][j]) {
                            only = static_cast<int>(j);
                            ++count;
                        }
                    }
                    if (count == 1) selected = rows[only];
                }
                if (!selected) continue;
            }
        }

        bool marked = false;
        for (auto& field : shown) {
            if (std::find(field.chain.begin(), field.chain.end(), selected) != field.chain.end()) {
                field.rowSelected = true;
                field.row = selected;
                marked = true;
            }
        }
        if (marked) {
            commandRow_ = selected;
            return;
        }
    }
}

std::vector<std::wstring> Reader::commandBadges() {
    std::vector<std::wstring> words;
    if (!commandRow_) return words;
    const auto parts = childrenOf(commandRow_);
    // The badges are inline images inside the row's badge slots. The slots are
    // not at fixed indices on every tab, so look through every part. Button
    // icons share this format but have names such as punch_h, which map to
    // nothing here.
    static const std::string kTag = "TaggedImages/";
    static uintptr_t loggedRow = 0;
    if (commandRow_ != loggedRow) {
        loggedRow = commandRow_;
        logLine(L"badges: row " + std::to_wstring(parts.size()) + L" parts");
    }
    auto badgeFor = [](const std::string& name) -> std::wstring {
        if (name == "ex") return L"EX";
        if (name == "ex2") return L"EX 2";
        if (name == "ex3") return L"EX 3";
        if (name == "tv1") return L"V-Trigger I";
        if (name == "tv2") return L"V-Trigger II";
        if (name == "tv1_2" || name == "tv12") return L"V-Trigger I to II";
        if (name == "v") return L"V-Skill I";
        if (name == "v2") return L"V-Skill II";
        if (name == "v3") return L"V-Skill III";
        if (name == "v_trigger") return L"V-Trigger";
        return {};
    };
    int htmlLeaves = 0;
    for (uintptr_t part : parts) {
        std::vector<uintptr_t> level{part};
        for (int depth = 0; depth < 8 && !level.empty(); ++depth) {
            std::vector<uintptr_t> next;
            for (uintptr_t object : level) {
                const auto kids = childrenOf(object);
                if (!kids.empty()) {
                    next.insert(next.end(), kids.begin(), kids.end());
                    continue;
                }
                uintptr_t text = 0;
                if (!readValue(object + 0x100, text) || !plausiblePointer(text)) continue;
                char raw[1024]{};
                if (!readRaw(text, raw, sizeof(raw) - 1) || !std::strstr(raw, kTag.c_str())) continue;
                ++htmlLeaves;
                std::string html(raw);
                for (size_t at = html.find(kTag); at != std::string::npos;
                     at = html.find(kTag, at)) {
                    at += kTag.size();
                    size_t end = at;
                    while (end < html.size() &&
                           (std::isalnum(static_cast<unsigned char>(html[end])) || html[end] == '_')) {
                        ++end;
                    }
                    if (end == at || end >= html.size() || html[end] != '.') {
                        at = end;
                        continue;
                    }
                    const std::wstring badge = badgeFor(html.substr(at, end - at));
                    if (!badge.empty() &&
                        std::find(words.begin(), words.end(), badge) == words.end()) {
                        words.push_back(badge);
                    }
                    at = end;
                }
            }
            level.swap(next);
        }
    }
    if (words.empty()) {
        logLine(L"badges: no badge in " + std::to_wstring(htmlLeaves) + L" image texts");
    }
    return words;
}

void Reader::markChoices(std::vector<Field>& shown) {
    // Group the visible labels by the container above them and the immediate
    // slot each sits in. A prompt's buttons are slots of one container, one
    // short label each.
    struct Slot {
        size_t field = 0;
        int nesting = 0;
    };
    struct Group {
        uintptr_t container = 0;
        std::unordered_map<uintptr_t, std::vector<Slot>> slots;
    };

    // Every field above a long question is a possible prompt panel. Walk those
    // fields once. The old code searched every on-screen field for each button
    // group, so the character select, which has many layered cells, spent most
    // of each poll rebuilding the same answer.
    std::unordered_set<uintptr_t> questionAncestors;
    questionAncestors.reserve(shown.size() * 4);
    for (const auto& field : shown) {
        if (field.text.size() < 24) continue;
        questionAncestors.insert(field.chain.begin(), field.chain.end());
    }

    std::unordered_map<uintptr_t, size_t> index;
    index.reserve(shown.size() * 2);
    std::vector<Group> groups;
    groups.reserve(shown.size());
    for (size_t i = 0; i < shown.size(); ++i) {
        const auto& field = shown[i];
        for (size_t level = 1; level < field.chain.size(); ++level) {
            const uintptr_t container = field.chain[level];
            const auto at = index.emplace(container, groups.size());
            if (at.second) {
                groups.emplace_back();
                groups.back().container = container;
            }
            groups[at.first->second].slots[field.chain[level - 1]].push_back(
                {i, static_cast<int>(level) - 1});
        }
    }
    // Keep the old container-address order so two overlapping marks resolve
    // the same way they did before.
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.container < b.container;
    });

    for (auto& group : groups) {
        const uintptr_t container = group.container;
        auto& slots = group.slots;
        if (slots.size() == 1) {
            // A notice's only button, such as Close.
            const auto& [slot, held] = *slots.begin();
            if (held.size() != 1 || childCount(container) != 1) continue;
            const Slot& only = held[0];
            if (only.nesting != 2 || shown[only.field].text.size() > 24) continue;
            if (childCount(slot) != 6) continue;
            auto& field = shown[only.field];
            field.button = true;
            field.chosen = true;
            field.group = container;
            const auto at = std::find(field.chain.begin(), field.chain.end(), container);
            if (at != field.chain.end() && std::next(at) != field.chain.end()) {
                field.panel = *std::next(at);
            }
            continue;
        }

        bool usable = true;
        for (const auto& [slot, held] : slots) {
            (void)slot;
            if (held.size() != 1 || shown[held[0].field].text.size() > 24) {
                usable = false;
                break;
            }
        }
        if (!usable) continue;

        uint32_t most = 0;
        int winners = 0;
        uintptr_t winnerSlot = 0;
        for (const auto& [slot, held] : slots) {
            const uint32_t children = childCount(slot);
            if (children > most) {
                most = children;
                winners = 1;
                winnerSlot = slot;
            } else if (children == most) {
                ++winners;
            }
        }
        if (winners != 1 || most < 2) continue;

        const Slot& winner = slots[winnerSlot][0];
        bool deeper = true;
        for (const auto& [slot, held] : slots) {
            if (slot == winnerSlot) continue;
            if (winner.nesting <= held[0].nesting) {
                deeper = false;
                break;
            }
        }
        if (!deeper) continue;

        // A real prompt puts a question in the panel above its buttons. The
        // character select's 1P badge has the same layered-button shape, so
        // without this it was taken for a dialog and the whole screen was
        // reduced to "1P". Arcade path cards sit under the path description,
        // which also reads as a question and is welcome there.
        uintptr_t panel = 0;
        const auto at = std::find(shown[winner.field].chain.begin(),
                                  shown[winner.field].chain.end(), container);
        if (at != shown[winner.field].chain.end() && std::next(at) != shown[winner.field].chain.end()) {
            panel = *std::next(at);
        }
        if (questionAncestors.find(panel) == questionAncestors.end()) continue;

        for (const auto& [slot, held] : slots) {
            (void)slot;
            shown[held[0].field].button = true;
        }
        auto& field = shown[winner.field];
        field.chosen = true;
        field.group = container;
        field.panel = panel;
    }
}


std::vector<Field> Reader::read() {
    std::vector<Field> fields;
    fields.reserve(docviews_.size());
    failText_ = 0;
    failChain_ = 0;
    std::vector<uintptr_t> alive;
    std::vector<int> aliveMisses;

    for (size_t i = 0; i < docviews_.size(); ++i) {
        Field field;
        if (readField(docviews_[i], field)) {
            fields.push_back(std::move(field));
            alive.push_back(docviews_[i]);
            aliveMisses.push_back(0);
        } else {
            int misses = i < misses_.size() ? misses_[i] + 1 : 1;
            if (misses < 3) {
                alive.push_back(docviews_[i]);
                aliveMisses.push_back(misses);
            }
        }
    }
    docviews_ = std::move(alive);
    misses_ = std::move(aliveMisses);

    if (fields.empty() && GetTickCount() - lastEmptyLog_ > 10000) {
        lastEmptyLog_ = GetTickCount();
        logLine(L"read: no fields; " + std::to_wstring(failText_) + L" text failures, " +
                std::to_wstring(failChain_) + L" chain failures, " +
                std::to_wstring(docviews_.size()) + L" candidates");
    }
    return fields;
}

}  // namespace sfva
