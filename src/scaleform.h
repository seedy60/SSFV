#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sfva {

// One Scaleform text field as the game is drawing it.
struct Field {
    std::wstring text;
    float x = 0.0f;  // stage pixels on the 1920x1080 stage
    float y = 0.0f;
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // effective colour multiplier
    bool hidden = false;  // an ancestor is switched off
    bool rooted = false;  // the chain ends at a live movie root
    bool chosen = false;  // a prompt button marked by its structure, not gold
    bool listSelected = false;  // a non-gold list item carrying the cursor highlight
    bool button = false;  // a short label sitting in a button's slot
    bool rowSelected = false;  // a command list move row holding the cursor
    uintptr_t row = 0;         // that row, so one row's fields cannot merge with another's
    uintptr_t docview = 0;
    uintptr_t group = 0;  // the container holding the group of buttons
    uintptr_t panel = 0;  // that container's parent, which holds the question
    std::vector<uintptr_t> chain;  // the field, then each parent to the root

    bool gold() const;   // the game's selection tint, RGB 255 227 140
    bool shown() const;  // on stage, attached, opaque enough, real text
};

class Reader {
public:
    explicit Reader(uintptr_t moduleBase);

    // Proves the vtable offsets below still describe this executable. A game
    // update that moves them must not be read as if nothing changed.
    bool verifyBuild();

    // Sweeps Scaleform's heap for text fields. `wide` walks every small
    // read-write region; the quick sweep only walks blocks shaped like the
    // allocations text fields have been found in.
    void scan(bool wide);

    // Reads every known field. Fields that keep failing are forgotten.
    std::vector<Field> read();

    // Marks the selected button of a prompt. Dialog buttons are not tinted
    // gold: the chosen one is built with extra layers, and its label sits a
    // level deeper than the other labels.
    void markChoices(std::vector<Field>& shown);

    // Marks a list item whose focus is drawn as a full-row highlight rather
    // than a gold text tint. Used for disabled/greyed menu entries.
    void markListFocus(std::vector<Field>& shown);

    // Marks the move row of the Command List that carries the cursor. Each
    // row is a holder of parts, and one part of the cursor's row is switched
    // on where every other row hides it.
    void markCommandRow(std::vector<Field>& shown);

    // The V-Skill, V-Trigger and EX badges on the selected move row, as words.
    std::vector<std::wstring> commandBadges();

    // True when this grid label's cell carries the visible checkmark that
    // marks the cursor on grids whose labels are all left bright (Battle BGM).
    bool gridChosen(const Field& field);

    size_t fieldCount() const { return docviews_.size(); }
    int scanMillis() const { return scanMs_; }
    bool sawFields() const { return sawFields_; }

private:
    bool readText(uintptr_t docview, std::wstring& out);
    bool readField(uintptr_t docview, Field& out);
    bool readNode(uintptr_t object, float matrix[6], float colour[4], uint16_t& flags,
                  uintptr_t& parent);
    uint32_t childCount(uintptr_t object);
    std::vector<uintptr_t> childrenOf(uintptr_t object);
    bool appearance(uintptr_t object, float colour[4], uint16_t& flags);

    uintptr_t base_ = 0;
    std::vector<uintptr_t> docviews_;
    std::vector<int> misses_;
    // Block sizes Scaleform has been seen to keep text fields in. A quick
    // sweep reads only these; a full sweep learns any new size.
    std::vector<size_t> quickSizes_;
    int scanMs_ = 0;
    bool sawFields_ = false;
    int emptyScans_ = 0;
    int failText_ = 0;
    int failChain_ = 0;
    uint32_t lastEmptyLog_ = 0;
    uintptr_t commandRow_ = 0;  // the row markCommandRow found the cursor on
};

}  // namespace sfva
