// Single-player cheats, see cheats.h.
//
// Game state: eboot+0xc1dc40 -> g. g+0x90 -> player manager (player i at +0x120 + 0x20*i), g+0x80 -> session
// records (+0xa8 + 0x23b0*i; bit 0 of eboot+0x271a0(record) = local human), g+0x78 -> online session (flags at
// +0x16908, masked by eboot+0xc15b20: any bit = in an online room, cheats off).
//
// Gold: the getter at eboot+0x81810 returns table[idx] from eboot+0xc1dc60 (one hero save block per entry); gold is
// the int32 at +0xaf238, clamped by the game to 0..99,999,999 (shop: eboot+0x52e0b3, pickup: eboot+0x495176).
//
// Damage apply, every unit (players and enemies): ebx = HP, [rbp-0x224] = damage, [rbp-0x200] = unit;
//   0xd8a00  mov rcx, [rbp-0x200]   <- jmp relay (+2-byte nop); also a direct branch target (0xd88b1, 0xd88c5)
//   0xd8a07  mov eax, ebx ; sub eax, [rbp-0x224] ; ... (death call if HP crosses 0) ; 0xd8a83 mov [unit+0xb0], eax
// The relay passes (unit, HP, damage) to on_damage and stores the damage it returns before the game uses it. A player
// character's unit is player(g, i)+0xd0. The hit's reaction (flinch / death) was already chosen from the original
// damage (eboot+0x29dcb0), and the death call only fires when HP crosses from > 0 to <= 0, so scaled damage stops at
// 1 HP: the kill always comes from a hit that is lethal unscaled (else the enemy stays at HP 0, walking, never dies).
#include "cheats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

#include <sys/mman.h>

#include "loader/hle.h"

namespace dcp::cheats {
namespace {

uint64_t g_base = 0;

template <class T> T rd(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}

uint64_t gs() { return rd<uint64_t>(g_base + 0xc1dc40); }

uint64_t player(uint64_t g, int i) {
    uint64_t mgr = rd<uint64_t>(g + 0x90);
    return mgr ? rd<uint64_t>(mgr + 0x120 + 0x20 * uint64_t(i)) : 0;
}

uint64_t record(uint64_t g, int i) {
    uint64_t srv = rd<uint64_t>(g + 0x80);
    return srv ? srv + 0xa8 + 0x23b0 * uint64_t(i) : 0;
}

bool in_room(uint64_t g) {
    uint64_t ps = rd<uint64_t>(g + 0x78);
    return ps && (rd<uint32_t>(ps + 0x16908) & rd<uint32_t>(g_base + 0xc15b20));
}

bool on(const char* env) {
    const char* v = std::getenv(env);
    return v && *v && std::strcmp(v, "0") != 0;
}

// ---- gold ----

constexpr uint32_t kHeroTable = 0xc1dc60, kGold = 0xaf238;
constexpr int32_t kGoldMax = 99999999;

bool guest(uint64_t a) { return a >= 0x200000000ull && a < 0x300000000ull; }

void gold_tick() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t g = gs();
        if (!g || in_room(g) || !player(g, 0)) continue;  // only with a hero loaded (no actor on the title screen)
        for (int i = 0; i < 4; ++i) {
            uint64_t hs = rd<uint64_t>(g_base + kHeroTable + 8 * i);
            if (guest(hs)) *reinterpret_cast<int32_t*>(hs + kGold) = kGoldMax;
        }
    }
}

// ---- HP / damage ----

constexpr uint32_t kDmgSite = 0xd8a00, kDmgCont = 0xd8a07;
constexpr uint8_t kDmgBytes[] = {0x48, 0x8b, 0x8d, 0x00, 0xfe, 0xff, 0xff};

bool g_god = false;
float g_dmg_mul = 1.0f;

using Fn1 = DCP_SYSV uint64_t (*)(uint64_t);

bool local_unit(uint64_t g, uint64_t unit, bool* any_player) {
    for (int i = 0; i < 4; ++i) {
        uint64_t a = player(g, i);
        if (!a || rd<uint64_t>(a + 0xd0) != unit) continue;
        *any_player = true;
        uint64_t rec = record(g, i);
        return rec && reinterpret_cast<Fn1>(g_base + 0x271a0)(rec) & 1;
    }
    return false;
}

DCP_SYSV int32_t on_damage(uint64_t unit, int32_t hp, int32_t dmg) {
    uint64_t g = gs();
    if (dmg <= 0 || !g || in_room(g)) return dmg;  // heals untouched; never online
    bool any_player = false;
    if (local_unit(g, unit, &any_player)) return g_god ? 0 : dmg;
    if (any_player || g_dmg_mul == 1.0f || dmg >= hp) return dmg;  // companions normal; lethal hits as they are
    return int32_t(std::min<double>(double(dmg) * g_dmg_mul, hp - 1));
}

void write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    uint64_t lo = reinterpret_cast<uint64_t>(at) & ~0xfffull, hi = (reinterpret_cast<uint64_t>(at) + n + 0xfff) & ~0xfffull;
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(at, bytes, n);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC);
}

uint8_t* alloc_near(uint64_t from, size_t size) {
    for (uint64_t a = (from + 0xffff) & ~0xffffull; a < from + (1ull << 30); a += 0x10000) {
        void* p = mmap(reinterpret_cast<void*>(a), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (p == reinterpret_cast<void*>(a)) return static_cast<uint8_t*>(p);
        munmap(p, size);
    }
    return nullptr;
}

void install_damage_hook(uint8_t* base) {
    uint8_t* site = base + kDmgSite;
    if (std::memcmp(site, kDmgBytes, sizeof kDmgBytes)) {
        std::fprintf(stderr, "cheats: unexpected bytes at eboot+0x%x; HP / damage cheats not installed\n", kDmgSite);
        return;
    }
    uint8_t* r = alloc_near(g_base + 0x1400000, 0x1000);
    if (!r) { std::fprintf(stderr, "cheats: no relay memory near the image; HP / damage cheats not installed\n"); return; }
    uint8_t* p = r;
    auto b = [&](std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; };
    auto q = [&](uint64_t v) { std::memcpy(p, &v, 8), p += 8; };
    b({0x89, 0xde});                                       // mov esi, ebx            (HP)
    b({0x53, 0x48, 0x89, 0xe3, 0x48, 0x83, 0xe4, 0xf0});   // push rbx; mov rbx, rsp; and rsp, -16
    b({0x48, 0x8b, 0xbd, 0x00, 0xfe, 0xff, 0xff});         // mov rdi, [rbp-0x200]    (unit)
    b({0x8b, 0x95, 0xdc, 0xfd, 0xff, 0xff});               // mov edx, [rbp-0x224]    (damage)
    b({0x48, 0xb8}), q(reinterpret_cast<uint64_t>(&on_damage)), b({0xff, 0xd0});  // call on_damage (sysv_abi)
    b({0x89, 0x85, 0xdc, 0xfd, 0xff, 0xff});               // mov [rbp-0x224], eax
    b({0x48, 0x89, 0xdc, 0x5b});                           // mov rsp, rbx; pop rbx
    b({0x48, 0x8b, 0x8d, 0x00, 0xfe, 0xff, 0xff});         // mov rcx, [rbp-0x200]    (the replaced instruction)
    b({0xff, 0x25, 0, 0, 0, 0}), q(g_base + kDmgCont);     // jmp eboot+0xd8a07
    int64_t rel = int64_t(r - (site + 5));
    if (rel != int32_t(rel)) { std::fprintf(stderr, "cheats: relay out of range; HP / damage cheats not installed\n"); return; }
    uint8_t j[7] = {0xe9, 0, 0, 0, 0, 0x66, 0x90};
    int32_t r32 = int32_t(rel);
    std::memcpy(j + 1, &r32, 4);
    write_code(site, j, sizeof j);
}


// XP: the results-screen tally (eboot+0x48e260..) moves a slice per frame from the pending pool (record+0x04) into
// XP-in-level (record+0x08); when that reaches the next level's need it levels up (+0x26) and recalculates stats.
//   0x48e31b  sub [rdx+4], eax ; add eax, ecx ; mov [rdx+8], eax     <- jmp relay (+3-byte nop)
// The relay takes only slice / multiplier from the pool, so the same pool yields multiplier x XP through the game's
// own level-up path (the tally just runs longer).
constexpr uint32_t kXpSite = 0x48e31b, kXpCont = 0x48e323;
constexpr uint8_t kXpBytes[] = {0x29, 0x42, 0x04, 0x01, 0xc8, 0x89, 0x42, 0x08};

float g_xp_mul = 1.0f;

DCP_SYSV int32_t xp_cost(int32_t slice) {
    uint64_t g = gs();
    if (slice <= 0 || !g || in_room(g)) return slice;
    int32_t c = int32_t(double(slice) / g_xp_mul + 0.999);
    return c < 1 ? 1 : c > slice ? slice : c;
}

void install_xp_hook(uint8_t* base) {
    uint8_t* site = base + kXpSite;
    if (std::memcmp(site, kXpBytes, sizeof kXpBytes)) {
        std::fprintf(stderr, "cheats: unexpected bytes at eboot+0x%x; XP multiplier not installed\n", kXpSite);
        return;
    }
    uint8_t* r = alloc_near(g_base + 0x1500000, 0x1000);
    if (!r) { std::fprintf(stderr, "cheats: no relay memory near the image; XP multiplier not installed\n"); return; }
    uint8_t* p = r;
    auto b = [&](std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; };
    auto q = [&](uint64_t v) { std::memcpy(p, &v, 8), p += 8; };
    b({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rax rcx rdx rsi rdi r8-r11
    b({0x53, 0x48, 0x89, 0xe3, 0x48, 0x83, 0xe4, 0xf0});   // push rbx; mov rbx, rsp; and rsp, -16
    b({0x89, 0xc7});                                       // mov edi, eax            (slice)
    b({0x48, 0xb8}), q(reinterpret_cast<uint64_t>(&xp_cost)), b({0xff, 0xd0});  // call xp_cost
    b({0x48, 0x89, 0xdc, 0x5b});                           // mov rsp, rbx; pop rbx
    b({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59});  // pop r11-r8 rdi rsi rdx rcx
    b({0x29, 0x42, 0x04});                                 // sub [rdx+4], eax        (pool -= cost)
    b({0x58});                                             // pop rax                 (slice)
    b({0x01, 0xc8, 0x89, 0x42, 0x08});                     // add eax, ecx ; mov [rdx+8], eax
    b({0xff, 0x25, 0, 0, 0, 0}), q(g_base + kXpCont);      // jmp eboot+0x48e323
    int64_t rel = int64_t(r - (site + 5));
    if (rel != int32_t(rel)) { std::fprintf(stderr, "cheats: relay out of range; XP multiplier not installed\n"); return; }
    uint8_t j[8] = {0xe9, 0, 0, 0, 0, 0x0f, 0x1f, 0x00};
    int32_t r32 = int32_t(rel);
    std::memcpy(j + 1, &r32, 4);
    write_code(site, j, sizeof j);
}

}  // namespace

void install(uint8_t* base) {
    g_base = reinterpret_cast<uint64_t>(base);
    if (on("DCP_CHEAT_GOLD")) {
        std::fprintf(stderr, "cheats: infinite gold (offline only)\n");
        std::thread(gold_tick).detach();
    }
    g_god = on("DCP_CHEAT_GOD");
    if (const char* m = std::getenv("DCP_CHEAT_DAMAGE"); m && *m) {
        float f = std::strtof(m, nullptr);
        if (f >= 0.1f && f <= 1000.0f) g_dmg_mul = f;
    }
    if (g_god || g_dmg_mul != 1.0f) {
        install_damage_hook(base);
        std::fprintf(stderr, "cheats: infinite HP %s, damage x%g (offline only)\n", g_god ? "on" : "off", g_dmg_mul);
    }
    if (const char* m = std::getenv("DCP_CHEAT_XP"); m && *m) {
        float f = std::strtof(m, nullptr);
        if (f > 1.0f && f <= 100.0f) {
            g_xp_mul = f;
            install_xp_hook(base);
            std::fprintf(stderr, "cheats: XP x%g (offline only)\n", g_xp_mul);
        }
    }
}

}  // namespace dcp::cheats
