#pragma once

#include <cstdint>

namespace Game
{
    
    void Init();

    void OnFrame();

    uintptr_t Player();
    uintptr_t GameMode();
    bool      InWorld();

    bool PlayerPos(float& x, float& y, float& z);

    bool HooksOk();

    void SetScaffold(bool on);
    void SetNoCameraShake(bool on);
    void SetNoTaa(bool on);
    void SetScaffoldYOffset(int v);   

    void SetScaffoldDryRun(bool on);
    bool ScaffoldDryRun();

    void SetScaffoldRedirect(bool on);
    bool ScaffoldRedirect();

    void SetScaffoldAuto(bool on);
    bool ScaffoldAuto();

    void SetScaffoldDelay(int ms);
    int  ScaffoldDelay();

    void SetWantPlayer(bool on);

    bool WantsPlayer();

    bool ScaffoldOn();
    bool NoCameraShakeOn();
    bool NoTaaOn();

    enum class AuraTarget { Players = 0, Mobs = 1, All = 2 };

    void SetKillAura(bool on);
    bool KillAuraOn();

    void SetKillAuraRange(float blocks);       
    void SetKillAuraTargets(int idx);          
    void SetKillAuraMode(int idx);             
    void SetKillAuraDelay(int ms);             
    void SetKillAuraThroughWalls(bool on);     

    uint32_t KillAuraAttacks();
    int      KillAuraTargetsSeen();

    bool TlsHookOk();
}
