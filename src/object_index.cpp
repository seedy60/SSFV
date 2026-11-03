#include "object_index.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

#include "common.h"

namespace sfva {
namespace {

// Facts about build v07.011, from the SFV Access project's engine work.
constexpr uintptr_t kNamesPointer = 0x3A75490;  // a pointer to the FName table
constexpr uintptr_t kObjectsHead = 0x3978730;   // flat TArray: data, count, capacity
constexpr int kNameOffset = 0x10;               // text inside an FNameEntry
constexpr int kNamesPerChunk = 16384;
constexpr int kNameChunks = 128;
constexpr int kNameCountOffset = kNameChunks * 8;
constexpr int kClassOffset = 0x10;  // UObject's class pointer
constexpr int kNameIndexOffset = 0x18;

bool printable(const std::wstring& text) {
    if (text.empty() || text.size() > 120) return false;
    for (wchar_t c : text) {
        if (c < 32 || c > 126) return false;
    }
    return true;
}

}  // namespace

bool ObjectIndex::init(uintptr_t moduleBase) {
    base_ = moduleBase;

    if (!readValue(base_ + kNamesPointer, names_) || !plausiblePointer(names_)) {
        logLine(L"radar: no name table at the known offset");
        return false;
    }
    if (!readValue(names_ + kNameCountOffset, nameCount_)) {
        logLine(L"radar: name table count unreadable");
        return false;
    }
    int chunks = 0;
    if (!readValue(names_ + kNameCountOffset + 4, chunks) || nameCount_ < 1000 ||
        nameCount_ > 4000000 || chunks <= 0 || chunks > kNameChunks) {
        logLine(L"radar: name table shape is not this build's");
        return false;
    }
    if (nameAt(0) != L"None") {
        logLine(L"radar: name table does not start with None");
        return false;
    }

    objectsHead_ = base_ + kObjectsHead;
    if (!refreshObjects()) {
        logLine(L"radar: object list is not this build's");
        return false;
    }
    ready_ = true;
    logLine(L"radar: object graph ready, " + std::to_wstring(objectCount_) + L" objects, " +
            std::to_wstring(nameCount_) + L" names");
    return true;
}

bool ObjectIndex::refreshObjects() {
    uintptr_t data = 0;
    int count = 0;
    int capacity = 0;
    if (!readValue(objectsHead_, data) || !readValue(objectsHead_ + 8, count) ||
        !readValue(objectsHead_ + 12, capacity)) {
        return false;
    }
    if (!plausiblePointer(data) || count < 1000 || count > 8000000 || capacity < count ||
        capacity > 8000000) {
        return false;
    }
    objectData_ = data;
    objectCount_ = count;
    objectCapacity_ = capacity;
    return true;
}

std::wstring ObjectIndex::nameAt(int index) const {
    if (index < 0 || index >= nameCount_ || !plausiblePointer(names_)) return {};
    const int chunkIndex = index / kNamesPerChunk;
    if (chunkIndex >= kNameChunks) return {};
    uintptr_t chunk = 0;
    if (!readValue(names_ + chunkIndex * 8, chunk) || !plausiblePointer(chunk)) return {};
    uintptr_t entry = 0;
    if (!readValue(chunk + static_cast<uintptr_t>(index % kNamesPerChunk) * 8, entry) ||
        !plausiblePointer(entry)) {
        return {};
    }
    uint32_t header = 0;
    if (!readValue(entry, header)) return {};

    if (header & 1) {
        wchar_t buffer[128]{};
        if (!readRaw(entry + kNameOffset, buffer, sizeof(buffer))) return {};
        std::wstring out;
        for (wchar_t c : buffer) {
            if (!c) break;
            out.push_back(c);
        }
        return printable(out) ? out : std::wstring{};
    }

    char buffer[128]{};
    if (!readRaw(entry + kNameOffset, buffer, sizeof(buffer))) return {};
    std::wstring out;
    for (char c : buffer) {
        if (!c) break;
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return printable(out) ? out : std::wstring{};
}

std::wstring ObjectIndex::objectName(uintptr_t object) const {
    if (!plausiblePointer(object)) return {};
    int index = 0;
    if (!readValue(object + kNameIndexOffset, index)) return {};
    return nameAt(index);
}

std::wstring ObjectIndex::classNameOf(uintptr_t object) const {
    uintptr_t cls = 0;
    if (!readValue(object + kClassOffset, cls) || !plausiblePointer(cls)) return {};
    return objectName(cls);
}

bool ObjectIndex::isClassKind(const std::wstring& name) {
    return name == L"Class" || name == L"BlueprintGeneratedClass" || name == L"ScriptClass" ||
           name == L"DynamicClass";
}

bool ObjectIndex::sameNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towupper(a[i]) != towupper(b[i])) return false;
    }
    return true;
}

uintptr_t ObjectIndex::classAddress(const std::wstring& name) {
    if (classCache_ && sameNoCase(classCacheName_, name) &&
        plausiblePointer(classCache_) && !objectName(classCache_).empty()) {
        return classCache_;
    }
    if (!refreshObjects()) return 0;

    raw_.resize(static_cast<size_t>(objectCount_));
    if (!readRaw(objectData_, raw_.data(), raw_.size() * sizeof(uintptr_t))) return 0;

    for (int i = 0; i < objectCount_; ++i) {
        const uintptr_t object = raw_[static_cast<size_t>(i)];
        if (!plausiblePointer(object)) continue;
        const std::wstring kind = classNameOf(object);
        if (!isClassKind(kind)) {
            // Only a class object's own class is one of these; every other
            // object is not a candidate and is skipped cheaply.
            continue;
        }
        const std::wstring candidate = objectName(object);
        if (candidate.empty() || candidate.rfind(L"Default__", 0) == 0) continue;
        if (sameNoCase(candidate, name)) {
            classCache_ = object;
            classCacheName_ = candidate;
            logLine(L"radar: class " + candidate + L" at 0x" + std::to_wstring(object));
            return object;
        }
    }
    return 0;
}

std::vector<uintptr_t> ObjectIndex::instances(uintptr_t classAddress) {
    std::vector<uintptr_t> found;
    if (!ready_ || !plausiblePointer(classAddress)) return found;
    if (!refreshObjects()) return found;

    raw_.resize(static_cast<size_t>(objectCount_));
    if (!readRaw(objectData_, raw_.data(), raw_.size() * sizeof(uintptr_t))) return found;

    for (int i = 0; i < objectCount_; ++i) {
        const uintptr_t object = raw_[static_cast<size_t>(i)];
        if (!plausiblePointer(object)) continue;
        uintptr_t cls = 0;
        if (!readValue(object + kClassOffset, cls) || cls != classAddress) continue;
        found.push_back(object);
    }
    return found;
}

bool ObjectIndex::instanceAlive(uintptr_t object, uintptr_t classAddress) const {
    if (!plausiblePointer(object)) return false;
    uintptr_t cls = 0;
    if (!readValue(object + kClassOffset, cls)) return false;
    // Passing zero asks only whether the object still exists.
    return classAddress == 0 || cls == classAddress;
}

}  // namespace sfva
