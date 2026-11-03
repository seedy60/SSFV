#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sfva {

// A small read-only view of the game's Unreal Engine object graph. The fight
// reader works from allocation scanning; the radar needs class instances,
// which the engine already indexes.
class ObjectIndex {
public:
    bool init(uintptr_t moduleBase);
    bool ready() const { return ready_; }

    // The UClass object for a class such as KBP_BattlePawn_C, or 0.
    uintptr_t classAddress(const std::wstring& name);

    // Every live instance whose class pointer is `classAddress`.
    std::vector<uintptr_t> instances(uintptr_t classAddress);

    // True while `object` still belongs to `classAddress`.
    bool instanceAlive(uintptr_t object, uintptr_t classAddress) const;

    // The FName of a UObject, or empty.
    std::wstring objectName(uintptr_t object) const;

private:
    bool refreshObjects();
    std::wstring nameAt(int index) const;
    std::wstring classNameOf(uintptr_t object) const;
    static bool isClassKind(const std::wstring& name);
    static bool sameNoCase(const std::wstring& a, const std::wstring& b);

    uintptr_t base_ = 0;
    uintptr_t names_ = 0;
    uintptr_t objectsHead_ = 0;
    uintptr_t objectData_ = 0;
    int nameCount_ = 0;
    int objectCount_ = 0;
    int objectCapacity_ = 0;
    bool ready_ = false;
    std::vector<uintptr_t> raw_;
    uintptr_t classCache_ = 0;
    std::wstring classCacheName_;
};

}  // namespace sfva
