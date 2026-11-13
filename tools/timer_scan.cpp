// Temporary read-only diagnostic: find four-byte values that tick down about
// once a second in a live Street Fighter V process. Not part of the mod.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

struct Region {
    uintptr_t base;
    size_t size;
};

struct Value {
    uintptr_t address;
    int kind;  // 0 int, 1 float
    int number;
    float seconds;
};

DWORD FindProcess() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD best = 0;
    SIZE_T bestWorkingSet = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"StreetFighterV.exe") != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                         entry.th32ProcessID);
            if (!process) continue;
            PROCESS_MEMORY_COUNTERS counters{};
            if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
                if (counters.WorkingSetSize > bestWorkingSet) {
                    bestWorkingSet = counters.WorkingSetSize;
                    best = entry.th32ProcessID;
                }
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return best;
}

std::vector<Region> Regions(HANDLE process) {
    std::vector<Region> regions;
    MEMORY_BASIC_INFORMATION info{};
    uintptr_t address = 0;
    while (address < 0x7FFFFFFFFFFFULL &&
           VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) {
        const uintptr_t size = static_cast<uintptr_t>(info.RegionSize);
        const DWORD protect = info.Protect;
        const bool readable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                              protect == PAGE_EXECUTE_READWRITE ||
                              protect == PAGE_EXECUTE_WRITECOPY;
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && readable && size &&
            size <= (256u << 20)) {
            regions.push_back({reinterpret_cast<uintptr_t>(info.BaseAddress), size});
        }
        address = reinterpret_cast<uintptr_t>(info.BaseAddress) + (size ? size : 0x1000);
    }
    return regions;
}

bool Read(HANDLE process, uintptr_t address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &got) &&
           got == size;
}

std::unordered_map<uintptr_t, Value> Snapshot(HANDLE process, const std::vector<Region>& regions) {
    std::unordered_map<uintptr_t, Value> values;
    values.reserve(200000);
    std::vector<uint8_t> buffer;
    buffer.reserve(8u << 20);
    for (const auto& region : regions) {
        for (size_t offset = 0; offset < region.size; offset += (8u << 20)) {
            const size_t chunk =
                static_cast<size_t>(std::min<size_t>(8u << 20, region.size - offset));
            buffer.resize(chunk);
            if (!Read(process, region.base + offset, buffer.data(), chunk)) continue;
            const size_t words = chunk / 4;
            for (size_t i = 0; i < words; ++i) {
                int32_t raw = 0;
                std::memcpy(&raw, buffer.data() + i * 4, sizeof(raw));
                const uintptr_t address = region.base + offset + i * 4;
                if (raw >= 10 && raw <= 99) {
                    values[address] = {address, 0, raw, 0.0f};
                    continue;
                }
                float seconds = 0.0f;
                std::memcpy(&seconds, buffer.data() + i * 4, sizeof(seconds));
                if (std::isfinite(seconds) && seconds >= 10.0f && seconds <= 99.0f) {
                    values[address] = {address, 1, 0, seconds};
                }
            }
        }
    }
    return values;
}

int Delta(const Value& before, const Value& after) {
    if (before.kind != after.kind) return 0;
    return before.number - after.number;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const DWORD pid = FindProcess();
    if (!pid) {
        std::printf("StreetFighterV.exe is not running\n");
        return 1;
    }
    HANDLE process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    const auto regions = Regions(process);
    size_t bytes = 0;
    for (const auto& region : regions) bytes += region.size;
    std::printf("pid %lu, %zu regions, %zu MB\n", pid, regions.size(), bytes >> 20);
    std::printf("before pass\n");

    const auto first = Snapshot(process, regions);
    std::printf("candidates %zu\n", first.size());
    Sleep(1050);
    const auto second = Snapshot(process, regions);
    std::printf("second pass %zu\n", second.size());
    Sleep(1050);
    const auto third = Snapshot(process, regions);
    std::printf("third pass %zu\n", third.size());
    Sleep(1050);
    const auto fourth = Snapshot(process, regions);
    std::printf("fourth pass %zu\n", fourth.size());

    struct Hit {
        uintptr_t address;
        int kind;
        float values[4];
    };
    std::vector<Hit> hits;
    for (const auto& [address, value] : first) {
        const auto b = second.find(address);
        const auto c = third.find(address);
        const auto d = fourth.find(address);
        if (b == second.end() || c == third.end() || d == fourth.end()) continue;
        if (value.kind != b->second.kind || value.kind != c->second.kind ||
            value.kind != d->second.kind) continue;
        float values[4] = {value.seconds, b->second.seconds, c->second.seconds,
                           d->second.seconds};
        if (value.kind == 0) {
            values[0] = static_cast<float>(value.number);
            values[1] = static_cast<float>(b->second.number);
            values[2] = static_cast<float>(c->second.number);
            values[3] = static_cast<float>(d->second.number);
        }
        bool ticking = true;
        for (int i = 1; i < 4; ++i) {
            const float delta = values[i - 1] - values[i];
            if (delta < 0.5f || delta > 3.5f) ticking = false;
        }
        if (ticking) hits.push_back({address, value.kind, {values[0], values[1], values[2], values[3]}});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.values[0] != b.values[0]) return a.values[0] > b.values[0];
        return a.address < b.address;
    });
    std::printf("ticking candidates: %zu\n", hits.size());
    size_t shown = 0;
    for (const auto& hit : hits) {
        if (shown++ >= 300) break;
        std::printf("0x%llx %s %.3f -> %.3f -> %.3f -> %.3f\n",
                    static_cast<unsigned long long>(hit.address), hit.kind ? "float" : "int",
                    hit.values[0], hit.values[1], hit.values[2], hit.values[3]);
    }
    CloseHandle(process);
    return 0;
}
