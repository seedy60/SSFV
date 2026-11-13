// Temporary read-only diagnostic: find pointers into the live round-clock
// allocation. Not part of the mod.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

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
            if (GetProcessMemoryInfo(process, &counters, sizeof(counters)) &&
                counters.WorkingSetSize > bestWorkingSet) {
                bestWorkingSet = counters.WorkingSetSize;
                best = entry.th32ProcessID;
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return best;
}

bool Read(HANDLE process, uintptr_t address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &got) &&
           got == size;
}

}  // namespace

int main(int argc, char** argv) {
    uintptr_t lo = 0x26baba87000;
    uintptr_t hi = lo + 0x2000;
    if (argc >= 2) {
        unsigned long long parsed = 0;
        if (std::sscanf(argv[1], "%llx", &parsed) == 1) lo = static_cast<uintptr_t>(parsed);
        hi = lo + 0x2000;
    }
    const DWORD pid = FindProcess();
    if (!pid) return 1;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 1;

    std::vector<uint8_t> buffer;
    buffer.reserve(8u << 20);
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION info{};
    size_t hits = 0;
    while (address < 0x7FFFFFFFFFFFULL &&
           VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) {
        const uintptr_t size = static_cast<uintptr_t>(info.RegionSize);
        const DWORD protect = info.Protect;
        const bool readable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                              protect == PAGE_EXECUTE_READWRITE ||
                              protect == PAGE_EXECUTE_WRITECOPY;
        if (info.State == MEM_COMMIT && readable && size && size <= (256u << 20)) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
            for (uintptr_t offset = 0; offset < size; offset += (8u << 20)) {
                const size_t chunk =
                    static_cast<size_t>(std::min<uintptr_t>(8u << 20, size - offset));
                buffer.resize(chunk);
                if (!Read(process, base + offset, buffer.data(), chunk)) continue;
                const size_t words = chunk / 8;
                for (size_t i = 0; i < words; ++i) {
                    uint64_t value = 0;
                    std::memcpy(&value, buffer.data() + i * 8, sizeof(value));
                    if (value < lo || value >= hi) continue;
                    if (hits++ < 200) {
                        std::printf("at 0x%llx -> 0x%llx (offset 0x%llx)\n",
                                    static_cast<unsigned long long>(base + offset + i * 8),
                                    static_cast<unsigned long long>(value),
                                    static_cast<unsigned long long>(value - lo));
                    }
                }
            }
        }
        address = reinterpret_cast<uintptr_t>(info.BaseAddress) + (size ? size : 0x1000);
    }
    std::printf("hits %zu\n", hits);
    CloseHandle(process);
    return 0;
}
