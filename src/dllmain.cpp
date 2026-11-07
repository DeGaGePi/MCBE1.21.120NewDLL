#include <Windows.h>

#include "core/Log.hpp"
#include "game/Game.hpp"
#include "hooks/DXGIHook.hpp"
#include "input/Input.hpp"
#include "ui/Module.hpp"

static HMODULE g_self = nullptr;

static DWORD WINAPI InitThread(LPVOID)
{
    Log::info("========================================");
    Log::info("NewDLL loaded (pid=%lu)  base=%p", GetCurrentProcessId(), (void*)g_self);
    Log::info("log file: %s", Log::LogPathAnsi().c_str());

    try { Modules::Init(); }
    catch (...) { Log::error("Modules::Init threw"); }

    try
    {
        Hooks::Init();
    }
    catch (const std::exception& e)
    {
        Log::error("Hooks::Init threw: %s", e.what());
    }
    catch (...)
    {
        Log::error("Hooks::Init threw (unknown)");
    }

    try
    {
        Game::Init();
    }
    catch (const std::exception& e)
    {
        Log::error("Game::Init threw: %s", e.what());
    }
    catch (...)
    {
        Log::error("Game::Init threw (unknown)");
    }

    Log::info("init done");
    return 0;
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        
        break;

    default:
        break;
    }
    return TRUE;
}
