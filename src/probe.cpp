// MK11BoxLab - runtime function probes (reverse-engineering aid)
// "probe <rva> [calls] [dumpbytes]": hook the game function at module+rva and log, for the next calls,
// the return address (as rva), the first four arguments and optionally a hex dump of [rcx] / [rdx] / [r8].
#include "log.h"
#include "memory.h"
#include "MinHook.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace probe {
using Fn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
struct Slot { uintptr_t target = 0; Fn orig = nullptr; std::atomic<int> left{0}; int dump = 0; };
static Slot g_slot[4];

static void Dump(const char* what, uintptr_t p, int n) {
    if (n <= 0 || p < 0x10000) return;
    uint8_t b[512];
    if (n > (int)sizeof(b)) n = sizeof(b);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)p, b, n, &got) || !got) return;
    std::string line;
    char t[16];
    for (SIZE_T i = 0; i < got; i += 4) {
        if (i % 32 == 0) { if (!line.empty()) Log("%s", line.c_str()); snprintf(t, sizeof(t), "  %s+%03X:", what, (unsigned)i); line = t; }
        uint32_t v; memcpy(&v, b + i, 4);
        snprintf(t, sizeof(t), " %08X", v); line += t;
    }
    if (!line.empty()) Log("%s", line.c_str());
}

template <int I>
static uintptr_t Hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {
    Slot& s = g_slot[I];
    if (s.left.load() > 0) {
        int n = --s.left;
        uintptr_t base = mem::ModuleBase();
        uintptr_t ret = (uintptr_t)__builtin_return_address(0);
        Log("probe %llX: ret rva %llX  rcx %llX rdx %llX r8 %llX r9 %llX%s", (unsigned long long)(s.target - base),
            (unsigned long long)(ret - base), (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
            (unsigned long long)d, n == 0 ? "  (last)" : "");
        Dump("rcx", a, s.dump); Dump("rdx", b, s.dump); Dump("r8", c, s.dump);
    }
    return s.orig(a, b, c, d, e, f, g, h);
}
static Fn g_hooks[4] = {Hook<0>, Hook<1>, Hook<2>, Hook<3>};

void Add(uintptr_t rva, int calls, int dump) {
    uintptr_t target = mem::ModuleBase() + rva;
    for (auto& s : g_slot)
        if (s.target == target) { s.dump = dump; s.left = calls; Log("probe %llX re-armed (%d calls)", (unsigned long long)rva, calls); return; }
    for (int i = 0; i < 4; ++i) {
        Slot& s = g_slot[i];
        if (s.target) continue;
        if (MH_CreateHook((void*)target, (void*)g_hooks[i], (void**)&s.orig) != MH_OK || MH_EnableHook((void*)target) != MH_OK) {
            Log("probe %llX: hook failed", (unsigned long long)rva);
            return;
        }
        s.target = target; s.dump = dump; s.left = calls;
        Log("probe %llX armed (%d calls, dump %d bytes)", (unsigned long long)rva, calls, dump);
        return;
    }
    Log("probe: all 4 slots in use (reload to clear)");
}
}  // namespace probe
