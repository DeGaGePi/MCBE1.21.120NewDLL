#include "game/Game.hpp"
#include "sdk/Mem.hpp"
#include "sdk/InlineHook.hpp"
#include "core/Log.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
    
    constexpr uintptr_t RVA_BUILDBLOCK_IMPL = 0x5862A10;   

    constexpr uintptr_t RVA_REG_LOCK        = 0x558C7E0;

    constexpr uintptr_t RVA_DYN_TLS_INIT    = 0x8721128;

    constexpr uintptr_t RVA_CAMERASHAKE     = 0x6CF0720;
    constexpr uintptr_t RVA_TAA_MODE        = 0x9E123E0;

    constexpr uintptr_t RVA_ATTACK_IMPL     = 0x585FF90;

    constexpr uintptr_t OFF_GM_PLAYER   = 0x08;    
    constexpr uintptr_t OFF_ACTOR_AABB  = 0x220;   
    constexpr uintptr_t OFF_PLAYER_GM   = 0xA78;   

    std::atomic<bool>  g_scaffold{ false };
    std::atomic<bool>  g_noCameraShake{ false };
    std::atomic<bool>  g_noTaa{ false };
    std::atomic<int>   g_scaffoldY{ -1 };
    std::atomic<bool>  g_scaffoldDry{ false };   
    std::atomic<bool>  g_scaffoldRedirect{ true };   

    alignas(16) int32_t   g_myPos[8][3] = {};
    std::atomic<uint32_t> g_myPosIdx{ 0 };
    std::atomic<uint32_t> g_scafPlaced{ 0 };     

    std::atomic<uintptr_t> g_player{ 0 };
    std::atomic<uintptr_t> g_gameMode{ 0 };
    std::atomic<bool>      g_scanRun{ false };
    std::atomic<int>       g_linkLogs{ 0 };

    std::atomic<bool> g_hookTried{ false };      
    std::atomic<bool> g_rlHookTried{ false };    
    bool  g_hooksOk = false;
    void* g_trampBuildBlock = nullptr;
    void* g_trampRegLock    = nullptr;   

    thread_local bool t_ourAutoCall = false;

    inline uintptr_t RvaToVa(uintptr_t rva) { return Mem::Base() + rva; }

    inline bool Plausible(uintptr_t p)
    {
        return p >= 0x10000ull && p < 0x00007FFFFFFFFFFFull && (p & 7) == 0;
    }

    constexpr uintptr_t TLS_OFF_GUARD     = 40;    
    constexpr uintptr_t TLS_OFF_REG_PTR   = 424;   
    constexpr uintptr_t TLS_OFF_REG_CTRL  = 432;   
    constexpr uintptr_t OFF_CTRL_USES     = 8;     
    constexpr uintptr_t OFF_CTRL_WEAKS    = 12;    

    std::atomic<uintptr_t> g_regPtr{ 0 };          
    std::atomic<uintptr_t> g_regCtrl{ 0 };         
    std::atomic<uint32_t>  g_regCap{ 0 };          
    std::atomic<uint32_t>  g_regIns{ 0 };          

    std::atomic<uint32_t>  g_bblRaw{ 0 };          
    std::atomic<uint32_t>  g_regLockEnter{ 0 };    
    std::atomic<uint32_t>  g_regLockBad{ 0 };      
    std::atomic<uint32_t>  g_tlsDumps{ 0 };        
    std::atomic<uint32_t>  g_dumpBbl{ 0 };         
    std::atomic<uint32_t>  g_dumpAuto{ 0 };
    std::atomic<uint32_t>  g_dumpRl{ 0 };

    inline uintptr_t TlsBase()
    {
        return *reinterpret_cast<uintptr_t*>(__readgsqword(0x58));
    }

    void CaptureRegistryTls()
    {
        if (g_regCtrl.load(std::memory_order_relaxed) != 0) return;   

        const uintptr_t b = TlsBase();
        if (!Plausible(b)) return;

        const uintptr_t p  = Mem::ReadOr<uintptr_t>(b + TLS_OFF_REG_PTR, 0);
        const uintptr_t cc = Mem::ReadOr<uintptr_t>(b + TLS_OFF_REG_CTRL, 0);
        if (!Plausible(p) || !Plausible(cc)) return;
        if (Mem::ReadOr<int32_t>(cc + OFF_CTRL_USES, 0) <= 0) return;   

        const uintptr_t vcc  = Mem::ReadOr<uintptr_t>(cc, 0);
        const uintptr_t base = Mem::Base();
        const size_t    isz  = Mem::ImageSize();
        if (isz < 0x1000) return;
        if (vcc < base || vcc >= base + isz) return;

        g_regPtr.store(p, std::memory_order_relaxed);
        g_regCtrl.store(cc, std::memory_order_relaxed);
        g_regCap.fetch_add(1, std::memory_order_relaxed);
    }

    bool EnsureRegistryTls()
    {
        const uintptr_t b = TlsBase();
        if (!Plausible(b)) return false;

        const uintptr_t cur = Mem::ReadOr<uintptr_t>(b + TLS_OFF_REG_CTRL, 0);
        if (Plausible(cur) && Mem::ReadOr<int32_t>(cur + OFF_CTRL_USES, 0) > 0) return true;

        const uintptr_t p  = g_regPtr.load(std::memory_order_relaxed);
        const uintptr_t cc = g_regCtrl.load(std::memory_order_relaxed);
        if (!Plausible(p) || !Plausible(cc)) return false;
        if (Mem::ReadOr<int32_t>(cc + OFF_CTRL_USES, 0) <= 0) return false;

        if (Mem::ReadOr<uint8_t>(b + TLS_OFF_GUARD, 0) == 0) return false;

        ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(cc + OFF_CTRL_WEAKS));
        if (!Mem::Write<uintptr_t>(b + TLS_OFF_REG_PTR,  p))  return false;
        if (!Mem::Write<uintptr_t>(b + TLS_OFF_REG_CTRL, cc)) return false;

        g_regIns.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void DumpTlsOnce(const char* tag, std::atomic<uint32_t>& ctr, uint32_t limit = 4)
    {
        if (ctr.fetch_add(1, std::memory_order_relaxed) >= limit) return;

        const uintptr_t b = TlsBase();
        uintptr_t v416 = 0, v424 = 0, v432 = 0, v440 = 0;
        bool okB = Plausible(b), ok416 = false, ok424 = false, ok432 = false, ok440 = false;
        int guard = -1;
        if (okB)
        {
            ok416 = Mem::Read<uintptr_t>(b + 416, v416);
            ok424 = Mem::Read<uintptr_t>(b + 424, v424);
            ok432 = Mem::Read<uintptr_t>(b + 432, v432);
            ok440 = Mem::Read<uintptr_t>(b + 440, v440);
            guard = Mem::ReadOr<int32_t>(b + 40, -1);
        }
        const int uses = ok432 && Plausible(v432)
                             ? Mem::ReadOr<int32_t>(v432 + OFF_CTRL_USES, -1) : -1;
        const uintptr_t vtof = ok424 && Plausible(v424) ? Mem::ReadOr<uintptr_t>(v424, 0) : 0;

        Log::info("[tlsdump][%s] tid=%u b=%p okB=%d guard=%d | +416=%p(%d) +424=%p(%d) "
                  "+432=%p(%d) +440=%p(%d) | uses=%d objvt=%p base=%p imgsz=%llx",
                  tag, ::GetCurrentThreadId(), reinterpret_cast<void*>(b), (int)okB, guard,
                  reinterpret_cast<void*>(v416), (int)ok416,
                  reinterpret_cast<void*>(v424), (int)ok424,
                  reinterpret_cast<void*>(v432), (int)ok432,
                  reinterpret_cast<void*>(v440), (int)ok440,
                  uses, reinterpret_cast<void*>(vtof),
                  reinterpret_cast<void*>(Mem::Base()),
                  static_cast<unsigned long long>(Mem::ImageSize()));
    }

    void DynTlsInit()
    {
        using Fn = void(__cdecl*)();
        reinterpret_cast<Fn>(RvaToVa(RVA_DYN_TLS_INIT))();
    }

    void __fastcall HelperRegLock(void* a1, void* a2, char a3)
    {
        g_regLockEnter.fetch_add(1, std::memory_order_relaxed);
        const uintptr_t b = TlsBase();
        if (Plausible(b))
        {
            
            CaptureRegistryTls();
            if (g_scaffold.load(std::memory_order_relaxed))
            {
                if (Mem::ReadOr<uintptr_t>(b + TLS_OFF_REG_CTRL, 0) == 0)
                    g_regLockBad.fetch_add(1, std::memory_order_relaxed);
                DumpTlsOnce("reglock", g_dumpRl, 6);
            }

            if (t_ourAutoCall)
            {
                const uintptr_t cur = Mem::ReadOr<uintptr_t>(b + TLS_OFF_REG_CTRL, 0);
                const bool okNow = Plausible(cur) &&
                                   Mem::ReadOr<int32_t>(cur + OFF_CTRL_USES, 0) > 0;
                if (!okNow)
                {
                    const uintptr_t p  = g_regPtr.load(std::memory_order_relaxed);
                    const uintptr_t cc = g_regCtrl.load(std::memory_order_relaxed);
                    if (Plausible(p) && Plausible(cc) &&
                        Mem::ReadOr<int32_t>(cc + OFF_CTRL_USES, 0) > 0)
                    {
                        if (Mem::ReadOr<uint8_t>(b + TLS_OFF_GUARD, 0) == 0)
                            DynTlsInit();
                        ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(cc + OFF_CTRL_WEAKS));
                        if (Mem::Write<uintptr_t>(b + TLS_OFF_REG_PTR, p) &&
                            Mem::Write<uintptr_t>(b + TLS_OFF_REG_CTRL, cc))
                            g_regIns.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        using Fn = void(__fastcall*)(void*, void*, char);
        reinterpret_cast<Fn>(g_trampRegLock)(a1, a2, a3);
    }

    bool Verify(uintptr_t va, std::initializer_list<int> bytes)
    {
        for (int b : bytes)
        {
            if (b >= 0 && Mem::ReadOr<uint8_t>(va, 0xEE) != static_cast<uint8_t>(b)) return false;
            ++va;
        }
        return true;
    }

    bool ReadAABB(uintptr_t aabb, float& mnx, float& mny, float& mnz,
                                    float& mxx, float& mxy, float& mxz)
    {
        if (aabb < 0x10000ull || aabb >= 0x00007FFFFFFFFFFFull) return false;
        if (!Mem::Read<float>(aabb + 0x00, mnx)) return false;
        if (!Mem::Read<float>(aabb + 0x04, mny)) return false;
        if (!Mem::Read<float>(aabb + 0x08, mnz)) return false;
        if (!Mem::Read<float>(aabb + 0x0C, mxx)) return false;
        if (!Mem::Read<float>(aabb + 0x10, mxy)) return false;
        if (!Mem::Read<float>(aabb + 0x14, mxz)) return false;
        return true;
    }

    bool PlausibleAABB(float mnx, float mny, float mnz, float mxx, float mxy, float mxz)
    {
        auto fin = [](float f) { return f == f && f > -3.0e7f && f < 3.0e7f; };
        if (!fin(mnx) || !fin(mny) || !fin(mnz) || !fin(mxx) || !fin(mxy) || !fin(mxz)) return false;
        if (mxx < mnx || mxy < mny || mxz < mnz) return false;
        if (mxx - mnx > 16.f || mxy - mny > 16.f || mxz - mnz > 16.f) return false;
        return true;
    }

    void CacheFromGameMode(void* gm)
    {
        if (!gm) return;
        const uintptr_t g = reinterpret_cast<uintptr_t>(gm);
        if (!Plausible(g)) return;

        if (g_gameMode.load(std::memory_order_relaxed) == g) return;   

        static const uintptr_t s_base = Mem::Base();
        static const size_t    s_isz  = Mem::ImageSize();
        if (s_isz < 0x1000) return;
        const uintptr_t vtLo = s_base + 0x08000000;      
        const uintptr_t vtHi = s_base + s_isz;

        const uintptr_t vt = Mem::ReadOr<uintptr_t>(g, 0);
        if (vt < vtLo || vt >= vtHi) return;

        const uintptr_t p = Mem::ReadOr<uintptr_t>(g + OFF_GM_PLAYER, 0);
        if (!Plausible(p)) return;

        const uintptr_t pvt = Mem::ReadOr<uintptr_t>(p, 0);
        if (pvt < vtLo || pvt >= vtHi) return;

        g_gameMode.store(g, std::memory_order_relaxed);
        if (g_player.load(std::memory_order_relaxed) != p)
        {
            g_player.store(p, std::memory_order_relaxed);
            if (g_linkLogs.fetch_add(1, std::memory_order_relaxed) < 4)
                Log::info("[game] player linked via hook: gm=%p vt=%p player=%p pvt=%p",
                          reinterpret_cast<void*>(g), reinterpret_cast<void*>(vt),
                          reinterpret_cast<void*>(p), reinterpret_cast<void*>(pvt));
        }
    }

    std::atomic<uint32_t> g_scafEnter{ 0 };    
    std::atomic<uint32_t> g_scafHits{ 0 };     

    std::atomic<bool>     g_autoOn{ true };       
    std::atomic<int>      g_autoDelay{ 60 };      
    std::atomic<int>      g_learnedBlock{ 1 };    
    std::atomic<int>      g_learnedFlag{ 0 };     
    std::atomic<uint32_t> g_autoPlaced{ 0 };
    std::atomic<uint32_t> g_autoSkip{ 0 };        
    std::atomic<int32_t>  g_autoX{ 0 }, g_autoY{ 0 }, g_autoZ{ 0 };
    std::atomic<uint32_t> g_gameTid{ 0 };         
    std::atomic<int32_t>  g_scafFX{ 0 }, g_scafFY{ 0 }, g_scafFZ{ 0 };
    std::atomic<int32_t>  g_scafTX{ 0 }, g_scafTY{ 0 }, g_scafTZ{ 0 };
    std::atomic<int32_t>  g_scafFeet{ 0 };

    constexpr size_t kMaxAuraActors  = 256;
    constexpr float  kAuraScanRadius = 96.f;   

    struct AuraActor
    {
        uintptr_t p        = 0;      
        float     x = 0, y = 0, z = 0;
        float     w = 0, h = 0, d = 0;
        bool      isPlayer = false;
    };

    std::atomic<bool>     g_auraOn{ false };
    std::atomic<float>    g_auraRange{ 4.5f };
    std::atomic<int>      g_auraTargets{ 2 };     
    std::atomic<int>      g_auraMode{ 0 };        
    std::atomic<int>      g_auraDelay{ 250 };     
    std::atomic<bool>     g_auraWalls{ false };   

    std::atomic<uint32_t> g_auraAttacks{ 0 };     
    std::atomic<uint32_t> g_auraEnter{ 0 };       
    std::atomic<uint32_t> g_auraNoTls{ 0 };       
    std::atomic<uint32_t> g_auraScans{ 0 };       
    std::atomic<int32_t>  g_auraLastX{ 0 }, g_auraLastY{ 0 }, g_auraLastZ{ 0 };
    std::atomic<int>      g_auraLastKind{ -1 };   
    std::atomic<int>      g_auraLastDist100{ 0 };

    constexpr const char* kAuraTargetNames[3] = { "Players Only", "Mobs Only", "All" };
    constexpr const char* kAuraModeNames[3]   = { "Single", "Switch", "Multi" };

    AuraActor             g_auraList[kMaxAuraActors];
    std::atomic<int>      g_auraCount{ 0 };
    std::atomic<uint32_t> g_auraSeq{ 0 };
    std::atomic<bool>     g_auraScanRun{ false };
    std::atomic<uint32_t> g_auraScanTick{ 0 };

    thread_local int t_inHelper = 0;

    void __fastcall HelperBuildBlock(void* gm, void* pos, char face, char flag)
    {
        if (t_inHelper != 0) return;
        struct Reentry { Reentry() { ++t_inHelper; } ~Reentry() { --t_inHelper; } } guard_;

        CacheFromGameMode(gm);

        g_bblRaw.fetch_add(1, std::memory_order_relaxed);
        CaptureRegistryTls();
        if (g_scaffold.load(std::memory_order_relaxed)) DumpTlsOnce("bbl", g_dumpBbl, 6);

        if (!g_scaffold.load(std::memory_order_relaxed) || !pos) return;

        g_scafEnter.fetch_add(1, std::memory_order_relaxed);

        g_gameTid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        g_learnedBlock.store(static_cast<int>(static_cast<unsigned char>(face)),
                             std::memory_order_relaxed);
        g_learnedFlag.store(static_cast<int>(static_cast<unsigned char>(flag)),
                            std::memory_order_relaxed);

        const uintptr_t pp = reinterpret_cast<uintptr_t>(pos);
        if (!Plausible(pp)) return;

        int32_t ox = 0, oy = 0, oz = 0;
        
        if (!Mem::Read<int32_t>(pp + 0, ox)) return;
        if (!Mem::Read<int32_t>(pp + 4, oy)) return;
        if (!Mem::Read<int32_t>(pp + 8, oz)) return;

        if (ox < -30000000 || ox > 30000000) return;
        if (oz < -30000000 || oz > 30000000) return;
        if (oy < -2000 || oy > 2000) return;

        const uintptr_t player = g_player.load(std::memory_order_relaxed);
        if (!Plausible(player)) return;

        const uintptr_t aabb = Mem::ReadOr<uintptr_t>(player + OFF_ACTOR_AABB, 0);
        float mnx, mny, mnz, mxx, mxy, mxz;
        if (!ReadAABB(aabb, mnx, mny, mnz, mxx, mxy, mxz)) return;

        const int bx = static_cast<int>(std::floorf((mnx + mxx) * 0.5f));
        const int by = static_cast<int>(std::floorf(mny)) + g_scaffoldY.load(std::memory_order_relaxed);
        const int bz = static_cast<int>(std::floorf((mnz + mxz) * 0.5f));

        if (bx < -30000000 || bx > 30000000) return;
        if (bz < -30000000 || bz > 30000000) return;

        g_scafFX.store(ox, std::memory_order_relaxed);
        g_scafFY.store(oy, std::memory_order_relaxed);
        g_scafFZ.store(oz, std::memory_order_relaxed);
        g_scafTX.store(bx, std::memory_order_relaxed);
        g_scafTY.store(by, std::memory_order_relaxed);
        g_scafTZ.store(bz, std::memory_order_relaxed);
        g_scafFeet.store(static_cast<int32_t>(mny * 100.f), std::memory_order_relaxed);
        g_scafHits.fetch_add(1, std::memory_order_relaxed);

        if (g_scaffoldDry.load(std::memory_order_relaxed)) return;

        if (g_scaffoldRedirect.load(std::memory_order_relaxed))
        {
            if (!Mem::RangeOk(pp, 12, true)) return;

            Mem::Write<int32_t>(pp + 0, bx);
            Mem::Write<int32_t>(pp + 4, by);
            Mem::Write<int32_t>(pp + 8, bz);

            int32_t c0 = 0, c1 = 0, c2 = 0;
            if (Mem::Read<int32_t>(pp + 0, c0) && Mem::Read<int32_t>(pp + 4, c1) &&
                Mem::Read<int32_t>(pp + 8, c2) &&
                c0 == bx && c1 == by && c2 == bz)
                g_scafPlaced.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        {
            if (!g_trampBuildBlock) return;

            const uint32_t k = g_myPosIdx.fetch_add(1, std::memory_order_relaxed) & 7u;
            int32_t* mine = g_myPos[k];
            mine[0] = bx; mine[1] = by; mine[2] = bz;

            using Fn = void(__fastcall*)(void*, void*, char, char);
            reinterpret_cast<Fn>(g_trampBuildBlock)(gm, mine, face, flag);
            g_scafPlaced.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void TickAutoPlace()
    {
        if (!g_scaffold.load(std::memory_order_relaxed)) return;
        if (!g_autoOn.load(std::memory_order_relaxed)) return;
        if (g_scaffoldDry.load(std::memory_order_relaxed)) return;
        
        if (!g_trampBuildBlock || !g_trampRegLock) return;

        DumpTlsOnce("auto", g_dumpAuto, 3);   

        const uintptr_t gm = g_gameMode.load(std::memory_order_relaxed);
        const uintptr_t player = g_player.load(std::memory_order_relaxed);
        if (!Plausible(gm) || !Plausible(player)) return;

        const uintptr_t aabb = Mem::ReadOr<uintptr_t>(player + OFF_ACTOR_AABB, 0);
        float mnx, mny, mnz, mxx, mxy, mxz;
        if (!ReadAABB(aabb, mnx, mny, mnz, mxx, mxy, mxz)) return;

        const int bx = static_cast<int>(std::floorf((mnx + mxx) * 0.5f));
        const int by = static_cast<int>(std::floorf(mny)) +
                       g_scaffoldY.load(std::memory_order_relaxed);
        const int bz = static_cast<int>(std::floorf((mnz + mxz) * 0.5f));

        if (bx < -30000000 || bx > 30000000) return;
        if (bz < -30000000 || bz > 30000000) return;
        if (by < -2000 || by > 2000) return;

        int      delay = g_autoDelay.load(std::memory_order_relaxed);
        if (delay < 10) delay = 10;

        static int32_t  s_lx = INT32_MAX, s_ly = 0, s_lz = 0;
        static uint64_t s_last = 0;

        const bool     moved = (bx != s_lx || by != s_ly || bz != s_lz);
        const uint64_t now = ::GetTickCount64();
        const uint64_t need = moved ? static_cast<uint64_t>(delay)
                                    : static_cast<uint64_t>(delay) * 10ull;
        if (s_last != 0 && now - s_last < need)
        {
            g_autoSkip.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const uint32_t k = g_myPosIdx.fetch_add(1, std::memory_order_relaxed) & 7u;
        int32_t* mine = g_myPos[k];
        mine[0] = bx; mine[1] = by; mine[2] = bz;

        const char bid = static_cast<char>(g_learnedBlock.load(std::memory_order_relaxed));
        const char fl  = static_cast<char>(g_learnedFlag.load(std::memory_order_relaxed));

        if (g_regCtrl.load(std::memory_order_relaxed) == 0)
        {
            g_autoSkip.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        EnsureRegistryTls();      

        using Fn = void(__fastcall*)(void*, void*, char, char);
        t_ourAutoCall = true;     
        reinterpret_cast<Fn>(g_trampBuildBlock)(reinterpret_cast<void*>(gm), mine, bid, fl);
        t_ourAutoCall = false;

        s_last = now;
        s_lx = bx; s_ly = by; s_lz = bz;
        g_autoX.store(bx, std::memory_order_relaxed);
        g_autoY.store(by, std::memory_order_relaxed);
        g_autoZ.store(bz, std::memory_order_relaxed);
        g_autoPlaced.fetch_add(1, std::memory_order_relaxed);
    }

    constexpr UINT WM_SCAFFOLD_TICK = WM_APP + 0x5CA;

    std::atomic<DWORD> g_mainTid{ 0 };
    HHOOK              g_msgHook = nullptr;

    DWORD ResolveMainThread()
    {
        const DWORD learned = g_gameTid.load(std::memory_order_relaxed);
        if (learned != 0) return learned;

        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        const DWORD me = ::GetCurrentProcessId();
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);

        DWORD          best = 0;
        ULARGE_INTEGER bestT{};
        bestT.QuadPart = ~0ull;

        if (::Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != me) continue;

                HANDLE h = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;

                FILETIME c{}, e{}, k{}, u{};
                if (::GetThreadTimes(h, &c, &e, &k, &u))
                {
                    ULARGE_INTEGER cq{};
                    cq.LowPart  = c.dwLowDateTime;
                    cq.HighPart = c.dwHighDateTime;
                    if (cq.QuadPart < bestT.QuadPart) { bestT = cq; best = te.th32ThreadID; }
                }
                ::CloseHandle(h);
            } while (::Thread32Next(snap, &te));
        }
        ::CloseHandle(snap);
        return best;
    }

    void TickKillAura();     

    LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= 0 && lParam)
        {
            MSG* msg = reinterpret_cast<MSG*>(lParam);
            if (msg->message == WM_SCAFFOLD_TICK)
            {
                msg->message = WM_NULL;    
                TickAutoPlace();           
                TickKillAura();            
            }
        }
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    void EnsureMainThreadPump()
    {
        if (g_msgHook) return;

        const DWORD tid = ResolveMainThread();
        if (tid == 0) { return; }          

        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&GetMsgProc), &self))
            return;

        HHOOK h = ::SetWindowsHookExW(WH_GETMESSAGE, &GetMsgProc, self, tid);
        if (!h)
        {
            Log::error("[game] main-thread pump: SetWindowsHookEx failed tid=%lu err=%lu "
                       "(自动搭路暂不可用)", tid, ::GetLastError());
            return;
        }

        g_msgHook = h;
        g_mainTid.store(tid, std::memory_order_relaxed);
        Log::info("[game] main-thread pump installed: mainTid=%lu hook=%p（scaffold 将在主线程执行）",
                  tid, h);
    }

    void PostScaffoldTick()
    {
        const DWORD tid = g_mainTid.load(std::memory_order_relaxed);
        if (tid == 0) return;
        ::PostThreadMessageW(tid, WM_SCAFFOLD_TICK, 0, 0);
    }

    void EnsureRegLockHook()
    {
        bool expected = false;
        if (!g_rlHookTried.compare_exchange_strong(expected, true)) return;

        const uintptr_t rl = RvaToVa(RVA_REG_LOCK);

        if (Verify(rl, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                         0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60 }))
        {
            g_trampRegLock = InlineHook::Install(reinterpret_cast<void*>(rl),
                reinterpret_cast<void*>(&HelperRegLock), 16, &g_trampRegLock);
            Log::info("[game] regLock @ %p inline hook installed -> trampoline=%p",
                      reinterpret_cast<void*>(rl), g_trampRegLock);
        }
        else
        {
            Log::error("[game] regLock 字节不匹配 — 无法安装 hook（版本不对？）");
        }

        g_hooksOk = (g_trampBuildBlock != nullptr && g_trampRegLock != nullptr);
    }

    void EnsureHooks()
    {
        bool expected = false;
        if (g_hookTried.compare_exchange_strong(expected, true))
        {
            const uintptr_t t = RvaToVa(RVA_BUILDBLOCK_IMPL);

            if (Verify(t, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                            0x55, 0x57, 0x41, 0x54, 0x41, 0x56 }))
            {
                Log::info("[game] buildBlock @ %p  align16=%d（原子补丁）",
                          reinterpret_cast<void*>(t),
                          static_cast<int>((t & 15) == 0));

                g_trampBuildBlock = InlineHook::Install(reinterpret_cast<void*>(t),
                    reinterpret_cast<void*>(&HelperBuildBlock), 16, &g_trampBuildBlock);
                Log::info("[game] buildBlock inline hook installed -> trampoline=%p", g_trampBuildBlock);
            }
            else
            {
                Log::error("[game] buildBlock 字节不匹配 — 无法安装 hook（版本不对？）");
            }
        }

        EnsureRegLockHook();

        g_hooksOk = (g_trampBuildBlock != nullptr && g_trampRegLock != nullptr);
    }

    uintptr_t ScanOnce()
    {
        const uintptr_t base = Mem::Base();
        const size_t    isz  = Mem::ImageSize();
        if (base < 0x10000 || isz < 0x1000) return 0;

        const uintptr_t imgLo = base;
        const uintptr_t imgHi = base + isz;
        const uintptr_t rvaLo = base + 0x08800000;
        if (rvaLo >= imgHi) return 0;

        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        constexpr size_t kChunk = 1u << 20;             
        constexpr size_t kYield = 4u << 20;             
        std::vector<uint8_t> buf(kChunk);
        size_t sinceYield = 0;

        for (uintptr_t a = lo; a < hi; )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) break;

            const uintptr_t rs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t re = rs + mbi.RegionSize;
            const DWORD     pr = mbi.Protect & 0xFF;

            const bool usable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                pr == PAGE_READWRITE &&
                mbi.RegionSize >= 0x1000 &&
                mbi.RegionSize <= (256ull << 20) &&
                (mbi.Type == MEM_PRIVATE);

            if (usable && !(rs >= imgLo && rs < imgHi))
            {
                for (uintptr_t off = 0; off < mbi.RegionSize; off += kChunk)
                {
                    const size_t n = static_cast<size_t>(
                        (mbi.RegionSize - off) < kChunk ? (mbi.RegionSize - off) : kChunk);
                    const uintptr_t pa = rs + off;

                    SIZE_T got = 0;
                    if (::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(pa),
                                            buf.data(), n, &got) && got >= 32)
                    {
                        const size_t real = static_cast<size_t>(got);
                        const uintptr_t end = pa + real;
                        for (uintptr_t q = pa; q + 16 <= end; q += 8)
                        {
                            const size_t i = static_cast<size_t>(q - pa);
                            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(buf.data() + i);
                            if (vt < rvaLo || vt >= imgHi) continue;

                            const uintptr_t p = *reinterpret_cast<const uintptr_t*>(buf.data() + i + 8);
                            if (p < 0x10000 || (p & 7)) continue;
                            if (Mem::ReadOr<uintptr_t>(p + OFF_PLAYER_GM, 0) != q) continue;

                            const uintptr_t pvt = Mem::ReadOr<uintptr_t>(p, 0);
                            if (pvt < rvaLo || pvt >= imgHi) continue;

                            const uintptr_t aabb = Mem::ReadOr<uintptr_t>(p + OFF_ACTOR_AABB, 0);
                            float mnx, mny, mnz, mxx, mxy, mxz;
                            if (!ReadAABB(aabb, mnx, mny, mnz, mxx, mxy, mxz)) continue;
                            if (!PlausibleAABB(mnx, mny, mnz, mxx, mxy, mxz)) continue;

                            g_gameMode.store(q, std::memory_order_relaxed);
                            g_player.store(p, std::memory_order_relaxed);
                            Log::info("[game] player FOUND by scan: player=%p gm=%p aabb=(%.1f,%.1f,%.1f)",
                                      reinterpret_cast<void*>(p), reinterpret_cast<void*>(q),
                                      (mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f);
                            return p;
                        }
                        sinceYield += real;
                    }

                    if (sinceYield >= kYield)
                    {
                        sinceYield = 0;
                        ::Sleep(1);                          
                    }
                }
            }

            if (re <= a) break;
            a = re;
        }
        return 0;
    }

    void ScanThread()
    {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (g_player.load(std::memory_order_relaxed) == 0) ScanOnce();
        g_scanRun.store(false, std::memory_order_relaxed);
        Log::info("[game] player scan finished (player=%p)",
                  reinterpret_cast<void*>(g_player.load(std::memory_order_relaxed)));
    }

    std::atomic<uint32_t> g_lastScanTick{ 0 };

    std::atomic<bool> g_wantPlayer{ false };

    bool NeedPlayer()
    {
        return g_scaffold.load(std::memory_order_relaxed) ||
               g_auraOn.load(std::memory_order_relaxed) ||
               g_wantPlayer.load(std::memory_order_relaxed);
    }

    void KickScan()
    {
        if (g_player.load(std::memory_order_relaxed) != 0) return;
        if (!NeedPlayer()) return;

        const uint32_t now = ::GetTickCount();
        if (now - g_lastScanTick.load(std::memory_order_relaxed) < 15000) return;

        bool expected = false;
        if (!g_scanRun.compare_exchange_strong(expected, true)) return;
        g_lastScanTick.store(now, std::memory_order_relaxed);
        std::thread(ScanThread).detach();
    }

    template <typename Fn>
    void WalkPrivateMemory(Fn&& fn)
    {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        const uintptr_t base = Mem::Base();
        const size_t    isz  = Mem::ImageSize();
        const uintptr_t imgLo = base, imgHi = base + isz;

        constexpr size_t kChunk = 1u << 20;
        constexpr size_t kYield = 4u << 20;

        std::vector<uint8_t> buf(kChunk);
        size_t sinceYield = 0;
        HANDLE hProc = ::GetCurrentProcess();

        for (uintptr_t a = lo; a < hi; )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) break;

            const uintptr_t rs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t re = rs + mbi.RegionSize;
            const DWORD     pr = mbi.Protect & 0xFF;

            const bool usable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                pr == PAGE_READWRITE &&
                mbi.RegionSize >= 0x1000 &&
                mbi.RegionSize <= (256ull << 20) &&
                (mbi.Type == MEM_PRIVATE);

            if (usable && !(rs >= imgLo && rs < imgHi))
            {
                for (uintptr_t off = 0; off < mbi.RegionSize; off += kChunk)
                {
                    const size_t n = static_cast<size_t>(
                        (mbi.RegionSize - off) < kChunk ? (mbi.RegionSize - off) : kChunk);
                    const uintptr_t pa = rs + off;

                    SIZE_T got = 0;
                    if (::ReadProcessMemory(hProc, reinterpret_cast<LPCVOID>(pa),
                                            buf.data(), n, &got) && got >= 0x230)
                    {
                        if (!fn(pa, buf.data(), static_cast<size_t>(got))) return;
                        sinceYield += static_cast<size_t>(got);
                    }

                    if (sinceYield >= kYield)
                    {
                        sinceYield = 0;
                        ::Sleep(1);          
                    }
                    if (g_auraScanRun.load(std::memory_order_relaxed) == false) return;
                }
            }

            if (re <= a) break;
            a = re;
        }
    }

    int AuraScanOnce()
    {
        const uintptr_t base = Mem::Base();
        const size_t    isz  = Mem::ImageSize();
        if (base < 0x10000 || isz < 0x1000) return 0;

        const uintptr_t imgHi   = base + isz;
        const uintptr_t rdataLo = base + 0x08800000;      
        if (rdataLo >= imgHi) return 0;

        uintptr_t textLo = 0, textSz = 0;
        if (!Mem::SectionRange(".text", textLo, textSz)) return 0;
        const uintptr_t textHi = textLo + textSz;

        const uintptr_t self   = g_player.load(std::memory_order_relaxed);
        const uintptr_t selfVt = Plausible(self) ? Mem::ReadOr<uintptr_t>(self, 0) : 0;

        float sx = 0, sy = 0, sz = 0;
        bool  haveSelf = false;
        if (Plausible(self))
        {
            const uintptr_t aabb = Mem::ReadOr<uintptr_t>(self + OFF_ACTOR_AABB, 0);
            float a, b, c, d, e, f;
            if (ReadAABB(aabb, a, b, c, d, e, f) && PlausibleAABB(a, b, c, d, e, f))
            {
                sx = (a + d) * 0.5f; sy = (b + e) * 0.5f; sz = (c + f) * 0.5f;
                haveSelf = true;
            }
        }

        struct Hit { uintptr_t p; float x, y, z, w, h, d; bool player; };
        static std::vector<Hit> hits;
        hits.clear();
        if (hits.capacity() < 1024) hits.reserve(1024);

        const float r2 = kAuraScanRadius * kAuraScanRadius;
        HANDLE hProc = ::GetCurrentProcess();

        WalkPrivateMemory([&](uintptr_t pa, uint8_t* buf, size_t real) -> bool
        {
            const uintptr_t end = pa + real;
            for (uintptr_t q = pa; q + OFF_ACTOR_AABB + 8 <= end; q += 8)
            {
                const size_t i = static_cast<size_t>(q - pa);

                const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(buf + i);
                if (vt < rdataLo || vt >= imgHi) continue;

                uintptr_t vt8[8];
                SIZE_T g2 = 0;
                if (!::ReadProcessMemory(hProc, reinterpret_cast<LPCVOID>(vt), vt8, 64, &g2) || g2 < 64)
                    continue;
                bool vtOk = true;
                for (int k = 0; k < 8; ++k)
                    if (vt8[k] < textLo || vt8[k] >= textHi) { vtOk = false; break; }
                if (!vtOk) continue;

                const uintptr_t aabb = *reinterpret_cast<const uintptr_t*>(buf + i + OFF_ACTOR_AABB);
                if (aabb < 0x10000 || (aabb & 3)) continue;

                float mnx, mny, mnz, mxx, mxy, mxz;
                if (!ReadAABB(aabb, mnx, mny, mnz, mxx, mxy, mxz)) continue;
                if (!PlausibleAABB(mnx, mny, mnz, mxx, mxy, mxz)) continue;

                const float w = mxx - mnx, h = mxy - mny, d = mxz - mnz;
                if (w < 0.05f || w > 6.f) continue;
                if (d < 0.05f || d > 6.f) continue;
                if (h < 0.05f || h > 8.f) continue;

                const float cx = (mnx + mxx) * 0.5f;
                const float cy = (mny + mxy) * 0.5f;
                const float cz = (mnz + mxz) * 0.5f;

                if (haveSelf)
                {
                    const float dx = cx - sx, dy = cy - sy, dz = cz - sz;
                    if (dx * dx + dy * dy + dz * dz > r2) continue;
                }

                bool isPlayer = false;
                if (Plausible(selfVt) && vt == selfVt) isPlayer = true;
                else
                {
                    const uintptr_t gmv = Mem::ReadOr<uintptr_t>(q + OFF_PLAYER_GM, 0);
                    if (Plausible(gmv) &&
                        Mem::ReadOr<uintptr_t>(gmv + OFF_GM_PLAYER, 0) == q)
                        isPlayer = true;
                }

                if (hits.size() < 8192)
                    hits.push_back({ q, cx, cy, cz, w, h, d, isPlayer });
            }
            return true;
        });

        if (haveSelf)
        {
            const float rx = sx, ry = sy, rz = sz;
            std::sort(hits.begin(), hits.end(), [rx, ry, rz](const Hit& a, const Hit& b) {
                const float ax = a.x - rx, ay = a.y - ry, az = a.z - rz;
                const float bx = b.x - rx, by = b.y - ry, bz = b.z - rz;
                return ax * ax + ay * ay + az * az < bx * bx + by * by + bz * bz;
            });
        }

        const int n = static_cast<int>(hits.size() < kMaxAuraActors ? hits.size() : kMaxAuraActors);

        g_auraSeq.fetch_add(1, std::memory_order_acq_rel);          
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < n; ++i)
        {
            g_auraList[i].p        = hits[i].p;
            g_auraList[i].x        = hits[i].x;
            g_auraList[i].y        = hits[i].y;
            g_auraList[i].z        = hits[i].z;
            g_auraList[i].w        = hits[i].w;
            g_auraList[i].h        = hits[i].h;
            g_auraList[i].d        = hits[i].d;
            g_auraList[i].isPlayer = hits[i].player;
        }
        g_auraCount.store(n, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        g_auraSeq.fetch_add(1, std::memory_order_acq_rel);          

        return n;
    }

    void AuraScanThread()
    {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const int n = AuraScanOnce();
        const int total = static_cast<int>(g_auraScans.fetch_add(1, std::memory_order_relaxed)) + 1;
        if (total <= 3 || (total % 20) == 0)
            Log::info("[aura] scan #%d: %d actor(s) within %.0f blocks", total, n, kAuraScanRadius);
        g_auraScanRun.store(false, std::memory_order_relaxed);
    }

    void TickKillAura()
    {
        if (!g_auraOn.load(std::memory_order_relaxed)) return;

        g_auraEnter.fetch_add(1, std::memory_order_relaxed);

        const uint64_t now = ::GetTickCount64();

        {
            const bool running = g_auraScanRun.load(std::memory_order_relaxed);
            if (!running)
            {
                const uint32_t last = g_auraScanTick.load(std::memory_order_relaxed);
                const uint32_t gap  = (g_auraCount.load(std::memory_order_relaxed) > 0) ? 3000u : 1000u;
                if (last == 0 || static_cast<uint32_t>(now) - last >= gap)
                {
                    bool expected = false;
                    if (g_auraScanRun.compare_exchange_strong(expected, true))
                    {
                        g_auraScanTick.store(static_cast<uint32_t>(now), std::memory_order_relaxed);
                        std::thread(AuraScanThread).detach();
                    }
                }
            }
        }

        const uintptr_t gm   = g_gameMode.load(std::memory_order_relaxed);
        const uintptr_t self = g_player.load(std::memory_order_relaxed);
        if (!Plausible(gm) || !Plausible(self)) return;

        int delay = g_auraDelay.load(std::memory_order_relaxed);
        if (delay < 30) delay = 30;
        static uint64_t s_lastAtk = 0;
        if (s_lastAtk != 0 && now - s_lastAtk < static_cast<uint64_t>(delay)) return;

        AuraActor snap[kMaxAuraActors];
        int  cnt = 0;
        bool ok  = false;
        for (int t = 0; t < 16; ++t)
        {
            const uint32_t s0 = g_auraSeq.load(std::memory_order_acquire);
            if (s0 & 1u) { ::SwitchToThread(); continue; }

            cnt = g_auraCount.load(std::memory_order_relaxed);
            if (cnt < 0) cnt = 0;
            if (cnt > static_cast<int>(kMaxAuraActors)) cnt = static_cast<int>(kMaxAuraActors);
            for (int i = 0; i < cnt; ++i) snap[i] = g_auraList[i];

            if (g_auraSeq.load(std::memory_order_acquire) == s0) { ok = true; break; }
        }
        if (!ok || cnt <= 0) return;

        const uintptr_t myAabb = Mem::ReadOr<uintptr_t>(self + OFF_ACTOR_AABB, 0);
        float a, b, c, d, e, f;
        if (!ReadAABB(myAabb, a, b, c, d, e, f)) return;
        const float sx = (a + d) * 0.5f, sy = (b + e) * 0.5f, sz = (c + f) * 0.5f;

        const int   sel   = g_auraTargets.load(std::memory_order_relaxed);
        const float range = g_auraRange.load(std::memory_order_relaxed);
        const float r2    = range * range;
        const int   mode  = g_auraMode.load(std::memory_order_relaxed);

        int   bestIdx = -1;
        float bestD2  = 1.0e30f;
        for (int i = 0; i < cnt; ++i)
        {
            const AuraActor& A = snap[i];
            if (!Plausible(A.p)) continue;
            if (A.p == self) continue;                       

            if (sel == 0 && !A.isPlayer) continue;           
            if (sel == 1 &&  A.isPlayer) continue;           

            const float dx = A.x - sx, dy = A.y - sy, dz = A.z - sz;
            const float dd = dx * dx + dy * dy + dz * dz;
            if (dd > r2) continue;

            if (dd < bestD2) { bestD2 = dd; bestIdx = i; }
        }

        if (bestIdx < 0) return;

        if (g_regCtrl.load(std::memory_order_relaxed) == 0)
        {
            g_auraNoTls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        EnsureRegistryTls();

        using AtkFn = void(__fastcall*)(void*, void*, char);

        int attacked = 0;
        auto doAttack = [&](const AuraActor& A, float d2)
        {

            t_ourAutoCall = true;
            reinterpret_cast<AtkFn>(RvaToVa(RVA_ATTACK_IMPL))(
                reinterpret_cast<void*>(gm), reinterpret_cast<void*>(A.p), 1);
            t_ourAutoCall = false;

            g_auraLastX.store(static_cast<int32_t>(A.x), std::memory_order_relaxed);
            g_auraLastY.store(static_cast<int32_t>(A.y), std::memory_order_relaxed);
            g_auraLastZ.store(static_cast<int32_t>(A.z), std::memory_order_relaxed);
            g_auraLastKind.store(A.isPlayer ? 0 : 1, std::memory_order_relaxed);
            g_auraLastDist100.store(static_cast<int>(std::sqrt(d2) * 100.f),
                                    std::memory_order_relaxed);
            g_auraAttacks.fetch_add(1, std::memory_order_relaxed);
            ++attacked;
        };

        if (mode == 2)   
        {
            
            int order[kMaxAuraActors];
            int on = 0;
            for (int i = 0; i < cnt; ++i) order[on++] = i;
            std::sort(order, order + on, [&](int x, int y) {
                const AuraActor& A = snap[x]; const AuraActor& B = snap[y];
                const float ax = A.x - sx, ay = A.y - sy, az = A.z - sz;
                const float bx = B.x - sx, by = B.y - sy, bz = B.z - sz;
                return ax * ax + ay * ay + az * az < bx * bx + by * by + bz * bz;
            });

            for (int k = 0; k < on && attacked < 6; ++k)
            {
                const AuraActor& A = snap[order[k]];
                if (!Plausible(A.p) || A.p == self) continue;
                if (sel == 0 && !A.isPlayer) continue;
                if (sel == 1 &&  A.isPlayer) continue;
                const float dx = A.x - sx, dy = A.y - sy, dz = A.z - sz;
                const float dd = dx * dx + dy * dy + dz * dz;
                if (dd > r2) continue;
                doAttack(A, dd);
            }
        }
        else             
        {
            doAttack(snap[bestIdx], bestD2);
        }

        if (attacked > 0) s_lastAtk = now;
    }

    bool     g_csApplied  = false;
    uint8_t  g_csOrigByte = 0;
    bool     g_csVerified = false;   
    bool     g_csWarned   = false;

    void ApplyCameraShake()
    {
        const bool want = g_noCameraShake.load(std::memory_order_relaxed);
        if (want == g_csApplied) return;                 

        if (!g_csVerified)
        {
            if (want && !g_csWarned)
            {
                g_csWarned = true;
                Log::error("[game] NoCameraShake 不可用：地址未通过字节校验，拒绝打补丁");
            }
            g_csApplied = want;                          
            return;
        }

        const uintptr_t p = RvaToVa(RVA_CAMERASHAKE);
        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(p), 1, PAGE_EXECUTE_READWRITE, &old))
        {
            Log::error("[game] NoCameraShake: VirtualProtect failed");
            return;
        }
        if (want)
        {
            const uint8_t orig = *reinterpret_cast<uint8_t*>(p);
            if (orig != 0xC3) g_csOrigByte = orig;      
            *reinterpret_cast<uint8_t*>(p) = 0xC3;      
        }
        else if (g_csOrigByte != 0)
        {
            *reinterpret_cast<uint8_t*>(p) = g_csOrigByte;
        }
        DWORD tmp = 0;
        ::VirtualProtect(reinterpret_cast<void*>(p), 1, old, &tmp);
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(p), 1);
        g_csApplied = want;
        Log::info("[game] NoCameraShake = %d", static_cast<int>(want));
    }

    bool  g_taaApplied   = false;
    bool  g_taaOrigValid = false;
    float g_taaOrig      = 1.f;

    void ApplyTaa()
    {
        const bool      want = g_noTaa.load(std::memory_order_relaxed);
        const uintptr_t p    = RvaToVa(RVA_TAA_MODE);

        if (want)
        {
            if (!g_taaApplied)
            {
                
                float cur = 0.f;
                if (Mem::Read<float>(p, cur)) { g_taaOrig = cur; g_taaOrigValid = true; }
                Log::info("[game] NoTAA = 1 (upscalingMode %.2f -> 0.00 bilinear)",
                          static_cast<double>(g_taaOrigValid ? g_taaOrig : 0.f));
            }
            g_taaApplied = true;
            Mem::WriteProt<float>(p, 0.f);
        }
        else if (g_taaApplied)
        {
            g_taaApplied = false;
            const float back = g_taaOrigValid ? g_taaOrig : 1.f;
            Mem::WriteProt<float>(p, back);
            Log::info("[game] NoTAA = 0 (upscalingMode restored %.2f)", static_cast<double>(back));
        }
    }
}

namespace Game
{
    void Init()
    {
        const uintptr_t base = Mem::Base();
        Log::info("[game] module base = %p  size = 0x%zX",
                  reinterpret_cast<void*>(base), Mem::ImageSize());

        if (Verify(RvaToVa(RVA_CAMERASHAKE),
                   { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48 }))
        {
            g_csVerified = true;
            Log::info("[game] CameraShake lambda verified @ %p",
                      reinterpret_cast<void*>(RvaToVa(RVA_CAMERASHAKE)));
        }
        else Log::error("[game] CameraShake 字节不匹配 — NoCameraShake 将不可用");

        Log::info("[game] hooks deferred — 只有开启 Scaffold 时才安装");
    }

    void OnFrame()
    {
        ApplyCameraShake();
        ApplyTaa();

        if (g_scaffold.load(std::memory_order_relaxed) ||
            g_auraOn.load(std::memory_order_relaxed))
        {
            EnsureMainThreadPump();

            static uint64_t s_nextPost = 0;
            const uint64_t now = ::GetTickCount64();
            if (now >= s_nextPost)
            {
                s_nextPost = now + 30;     
                PostScaffoldTick();
            }
        }

        {
            static uint32_t s_seen = 0;
            static uint32_t s_seenEnter = 0;
            const uint32_t h = g_scafHits.load(std::memory_order_relaxed);
            const uint32_t e = g_scafEnter.load(std::memory_order_relaxed);

            if (e != s_seenEnter && e <= 8 && e > 0)
            {
                s_seenEnter = e;
                Log::info("[scaffold] hook entered #%u (hits=%u)", e, h);
            }

            if (h != s_seen)
            {
                s_seen = h;
                if (h <= 6 || (h % 100) == 0)
                    Log::info("[scaffold] #%u pos %d,%d,%d -> %d,%d,%d (feetY=%.2f, placed=%u, mode=%s)",
                              h,
                              g_scafFX.load(std::memory_order_relaxed),
                              g_scafFY.load(std::memory_order_relaxed),
                              g_scafFZ.load(std::memory_order_relaxed),
                              g_scafTX.load(std::memory_order_relaxed),
                              g_scafTY.load(std::memory_order_relaxed),
                              g_scafTZ.load(std::memory_order_relaxed),
                              g_scafFeet.load(std::memory_order_relaxed) / 100.f,
                              g_scafPlaced.load(std::memory_order_relaxed),
                              g_scaffoldRedirect.load(std::memory_order_relaxed) ? "redirect" : "native");
            }
        }

        {
            static uint32_t s_seenAuto = 0;
            const uint32_t a = g_autoPlaced.load(std::memory_order_relaxed);
            if (a != s_seenAuto && (a <= 5 || (a % 100) == 0))
            {
                s_seenAuto = a;
                Log::info("[scaffold] auto #%u placed at %d,%d,%d (blockId=%d flag=%d, tid: game=%u render=%u)",
                          a,
                          g_autoX.load(std::memory_order_relaxed),
                          g_autoY.load(std::memory_order_relaxed),
                          g_autoZ.load(std::memory_order_relaxed),
                          g_learnedBlock.load(std::memory_order_relaxed),
                          g_learnedFlag.load(std::memory_order_relaxed),
                          g_gameTid.load(std::memory_order_relaxed),
                          ::GetCurrentThreadId());
            }

            static uint32_t s_seenCap = 0, s_seenIns = 0, s_seenRL = 0, s_seenBB = 0;
            const uint32_t cap = g_regCap.load(std::memory_order_relaxed);
            const uint32_t ins = g_regIns.load(std::memory_order_relaxed);
            const uint32_t rl  = g_regLockEnter.load(std::memory_order_relaxed);
            const uint32_t bb  = g_bblRaw.load(std::memory_order_relaxed);
            const uint32_t bad = g_regLockBad.load(std::memory_order_relaxed);
            static uint32_t s_seenSkip = 0;
            const uint32_t sk = g_autoSkip.load(std::memory_order_relaxed);
            if (cap != s_seenCap || ins != s_seenIns || rl != s_seenRL || bb != s_seenBB ||
                (sk != s_seenSkip && (sk % 200) == 0))
            {
                s_seenCap = cap; s_seenIns = ins; s_seenRL = rl; s_seenBB = bb;
                s_seenSkip = sk;
                Log::info("[scaffold] regcap=%u regins=%u autoskip=%u | bblEnter=%u regLockEnter=%u regLockBad=%u gameTid=%u renderTid=%u",
                          cap, ins, sk, bb, rl, bad,
                          g_gameTid.load(std::memory_order_relaxed), ::GetCurrentThreadId());
            }
        }

        if (g_auraOn.load(std::memory_order_relaxed))
        {
            static uint32_t s_seenAtk = 0, s_seenNoTls = 0, s_seenScans = 0;
            const uint32_t atk = g_auraAttacks.load(std::memory_order_relaxed);
            const uint32_t nt  = g_auraNoTls.load(std::memory_order_relaxed);
            const uint32_t sc  = g_auraScans.load(std::memory_order_relaxed);
            const int      cnt = g_auraCount.load(std::memory_order_relaxed);

            if (atk != s_seenAtk)
            {
                s_seenAtk = atk;
                if (atk <= 6 || (atk % 50) == 0)
                    Log::info("[aura] hit #%u -> %s at %d,%d,%d (dist=%.2f, seen=%d, tid=%u)",
                              atk,
                              g_auraLastKind.load(std::memory_order_relaxed) == 0 ? "PLAYER" : "MOB",
                              g_auraLastX.load(std::memory_order_relaxed),
                              g_auraLastY.load(std::memory_order_relaxed),
                              g_auraLastZ.load(std::memory_order_relaxed),
                              g_auraLastDist100.load(std::memory_order_relaxed) / 100.f,
                              cnt, ::GetCurrentThreadId());
            }

            if (nt != s_seenNoTls && (nt <= 3 || (nt % 200) == 0))
            {
                s_seenNoTls = nt;
                Log::info("[aura] skipped: no ItemRegistryRef backup yet (autoskip=%u) — "
                          "正常跑几秒等 regcap>0 即可", nt);
            }

            if (sc != s_seenScans && sc <= 3)
            {
                s_seenScans = sc;
                Log::info("[aura] scan #%u done: %d actor(s) cached (range=%.1f, targets=%s)",
                          sc, cnt, static_cast<double>(g_auraRange.load(std::memory_order_relaxed)),
                          g_auraTargets.load(std::memory_order_relaxed) == 0 ? "Players" :
                          g_auraTargets.load(std::memory_order_relaxed) == 1 ? "Mobs" : "All");
            }
        }

        if (!NeedPlayer()) return;

        const uintptr_t p = g_player.load(std::memory_order_relaxed);
        if (p)
        {
            const uintptr_t aabb = Mem::ReadOr<uintptr_t>(p + OFF_ACTOR_AABB, 0);
            if (!aabb)
            {
                g_player.store(0, std::memory_order_relaxed);
                g_gameMode.store(0, std::memory_order_relaxed);
                Log::info("[game] player pointer lost, rescan");
                KickScan();
            }
        }
        else
        {
            KickScan();
        }
    }

    uintptr_t Player()   { return g_player.load(std::memory_order_relaxed); }
    uintptr_t GameMode() { return g_gameMode.load(std::memory_order_relaxed); }
    bool      InWorld()  { return g_player.load(std::memory_order_relaxed) != 0; }
    bool      HooksOk()  { return g_hooksOk; }

    bool PlayerPos(float& x, float& y, float& z)
    {
        const uintptr_t p = g_player.load(std::memory_order_relaxed);
        if (!Plausible(p)) return false;

        const uintptr_t aabb = Mem::ReadOr<uintptr_t>(p + OFF_ACTOR_AABB, 0);
        float mnx, mny, mnz, mxx, mxy, mxz;
        if (!ReadAABB(aabb, mnx, mny, mnz, mxx, mxy, mxz)) return false;
        
        if (!PlausibleAABB(mnx, mny, mnz, mxx, mxy, mxz)) return false;

        x = (mnx + mxx) * 0.5f;
        y = (mny + mxy) * 0.5f;
        z = (mnz + mxz) * 0.5f;
        return true;
    }

    void SetWantPlayer(bool on)
    {
        const bool was = g_wantPlayer.exchange(on, std::memory_order_relaxed);
        if (on && !was) KickScan();      
    }

    bool WantsPlayer() { return NeedPlayer(); }

    void SetScaffold(bool on)
    {
        const bool was = g_scaffold.exchange(on, std::memory_order_relaxed);

        if (on && !was)
        {
            EnsureHooks();
            EnsureMainThreadPump();   
            KickScan();
        }
    }

    void SetNoCameraShake(bool on) { g_noCameraShake.store(on, std::memory_order_relaxed); }
    void SetNoTaa(bool on)         { g_noTaa.store(on, std::memory_order_relaxed); }
    void SetScaffoldYOffset(int v) { g_scaffoldY.store(v, std::memory_order_relaxed); }

    void SetScaffoldDryRun(bool on)
    {
        const bool was = g_scaffoldDry.exchange(on, std::memory_order_relaxed);
        if (was != on)
            Log::info("[game] Scaffold Dry Run = %d（%s）", static_cast<int>(on),
                      on ? "只记录不写内存" : "正常放置");
    }
    bool ScaffoldDryRun() { return g_scaffoldDry.load(std::memory_order_relaxed); }

    void SetScaffoldRedirect(bool on)
    {
        const bool was = g_scaffoldRedirect.exchange(on, std::memory_order_relaxed);
        if (was != on)
            Log::info("[game] Scaffold method = %s", on ? "redirect（改写调用方 BlockPos）"
                                                        : "native（自己的 BlockPos，零改写）");
    }
    bool ScaffoldRedirect() { return g_scaffoldRedirect.load(std::memory_order_relaxed); }

    void SetScaffoldAuto(bool on)
    {
        const bool was = g_autoOn.exchange(on, std::memory_order_relaxed);
        if (was != on)
            Log::info("[game] Scaffold auto = %d（%s）", static_cast<int>(on),
                      on ? "常开，走到哪放到哪" : "只有右键时才放");
    }
    bool ScaffoldAuto() { return g_autoOn.load(std::memory_order_relaxed); }

    void SetScaffoldDelay(int ms)
    {
        if (ms < 10)   ms = 10;
        if (ms > 1000) ms = 1000;
        g_autoDelay.store(ms, std::memory_order_relaxed);
    }
    int ScaffoldDelay() { return g_autoDelay.load(std::memory_order_relaxed); }

    void SetKillAura(bool on)
    {
        const bool was = g_auraOn.exchange(on, std::memory_order_relaxed);

        if (on && !was)
        {

            EnsureRegLockHook();
            EnsureMainThreadPump();     
            KickScan();                 
            Log::info("[game] KillAura = 1 (targets=%s, mode=%s, range=%.1f, delay=%dms)",
                      kAuraTargetNames[g_auraTargets.load(std::memory_order_relaxed)],
                      kAuraModeNames[g_auraMode.load(std::memory_order_relaxed)],
                      static_cast<double>(g_auraRange.load(std::memory_order_relaxed)),
                      g_auraDelay.load(std::memory_order_relaxed));
        }
        else if (!on && was)
        {
            g_auraScanRun.store(false, std::memory_order_relaxed);   
            Log::info("[game] KillAura = 0");
        }
    }
    bool KillAuraOn() { return g_auraOn.load(std::memory_order_relaxed); }

    void SetKillAuraRange(float v)
    {
        if (v < 2.f) v = 2.f;
        if (v > 8.f) v = 8.f;
        g_auraRange.store(v, std::memory_order_relaxed);
    }

    void SetKillAuraTargets(int idx)
    {
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        const int was = g_auraTargets.exchange(idx, std::memory_order_relaxed);
        if (was != idx)
            Log::info("[game] KillAura targets = %s", kAuraTargetNames[idx]);
    }

    void SetKillAuraMode(int idx)
    {
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        const int was = g_auraMode.exchange(idx, std::memory_order_relaxed);
        if (was != idx)
            Log::info("[game] KillAura mode = %s", kAuraModeNames[idx]);
    }

    void SetKillAuraDelay(int ms)
    {
        if (ms < 30)   ms = 30;
        if (ms > 2000) ms = 2000;
        g_auraDelay.store(ms, std::memory_order_relaxed);
    }

    void SetKillAuraThroughWalls(bool on) { g_auraWalls.store(on, std::memory_order_relaxed); }

    uint32_t KillAuraAttacks()    { return g_auraAttacks.load(std::memory_order_relaxed); }
    int      KillAuraTargetsSeen(){ return g_auraCount.load(std::memory_order_relaxed); }

    bool TlsHookOk() { return g_trampRegLock != nullptr; }

    bool ScaffoldOn()      { return g_scaffold.load(std::memory_order_relaxed); }
    bool NoCameraShakeOn() { return g_noCameraShake.load(std::memory_order_relaxed); }
    bool NoTaaOn()         { return g_noTaa.load(std::memory_order_relaxed); }
}
