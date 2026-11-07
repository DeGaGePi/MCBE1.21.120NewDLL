#include "input/Input.hpp"

#include <atomic>
#include <mutex>
#include <cstring>

#include "core/Log.hpp"

namespace
{
    
    Input::MouseState g_pending;
    bool g_pKDown[256] = {};
    bool g_pKPress[256] = {};
    bool g_pKRelease[256] = {};

    Input::MouseState g_frame;
    bool g_fKDown[256] = {};
    bool g_fKPress[256] = {};

    std::mutex        g_mtx;
    std::atomic<bool> g_menuOpen{ false };
    std::atomic<bool> g_prevOpen{ false };

    std::atomic<float> g_vx{ 0.f };
    std::atomic<float> g_vy{ 0.f };
    std::atomic<bool>  g_vValid{ false };
    std::atomic<bool>  g_center{ false };
    std::atomic<bool>  g_rawOk{ false };

    float g_cliW = 0.f, g_cliH = 0.f;
    float g_prevX = 0.f, g_prevY = 0.f;

    bool g_hidCursor = false;

    std::atomic<HWND>     g_gameWnd{ nullptr };
    std::atomic<uint32_t> g_winTryTick{ 0 };
    HWND g_rawWnd = nullptr;

    bool CursorShowing()
    {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        return ::GetCursorInfo(&ci) != FALSE && (ci.flags & CURSOR_SHOWING) != 0;
    }

    BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;

        RECT r{};
        GetClientRect(hwnd, &r);
        if ((r.right - r.left) >= 320 && (r.bottom - r.top) >= 240)
        {
            *reinterpret_cast<HWND*>(lp) = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    HWND FindGameWindow()
    {
        HWND cur = g_gameWnd.load(std::memory_order_relaxed);
        if (cur && ::IsWindow(cur)) return cur;

        const uint32_t now  = ::GetTickCount();
        uint32_t       prev = g_winTryTick.load(std::memory_order_relaxed);
        if (now - prev < 2000) return nullptr;
        if (!g_winTryTick.compare_exchange_strong(prev, now)) return nullptr;

        HWND found = nullptr;
        ::EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&found));
        if (found && found != cur)
            Log::info("[input] game window = %p", reinterpret_cast<void*>(found));
        g_gameWnd.store(found, std::memory_order_relaxed);
        return found;
    }

    void RefreshClientSize()
    {
        HWND w = FindGameWindow();
        if (!w) return;
        RECT r{};
        if (GetClientRect(w, &r))
        {
            g_cliW = static_cast<float>(r.right - r.left);
            g_cliH = static_cast<float>(r.bottom - r.top);
        }
    }

    void MoveCursor(float dx, float dy)
    {
        if (g_cliW < 2.f || g_cliH < 2.f) RefreshClientSize();

        float x = g_vx.load(std::memory_order_relaxed) + dx;
        float y = g_vy.load(std::memory_order_relaxed) + dy;

        if (x < 0.f) x = 0.f;
        if (y < 0.f) y = 0.f;
        if (g_cliW > 2.f && x > g_cliW - 1.f) x = g_cliW - 1.f;
        if (g_cliH > 2.f && y > g_cliH - 1.f) y = g_cliH - 1.f;

        g_vx.store(x, std::memory_order_relaxed);
        g_vy.store(y, std::memory_order_relaxed);
        g_vValid.store(true, std::memory_order_relaxed);
    }

    void ButtonEvent(int idx, bool downNow)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (downNow)
        {
            if (!g_pending.down[idx]) g_pending.pressed[idx] = true;
            g_pending.down[idx] = true;
        }
        else
        {
            g_pending.down[idx] = false;
            g_pending.released[idx] = true;
        }
    }

    LRESULT CALLBACK RawProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        if (m == WM_INPUT)
        {
            UINT sz = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &sz,
                            sizeof(RAWINPUTHEADER));
            if (sz > 0 && sz <= 256)
            {
                BYTE buf[256]{};
                UINT got = sz;
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf, &got,
                                    sizeof(RAWINPUTHEADER)) == sz)
                {
                    auto* ri = reinterpret_cast<RAWINPUT*>(buf);

                    if (ri->header.dwType == RIM_TYPEMOUSE)
                    {
                        const RAWMOUSE& rm = ri->data.mouse;

                        if (rm.usFlags & MOUSE_MOVE_ABSOLUTE)
                        {
                            
                            if (g_cliW < 2.f || g_cliH < 2.f) RefreshClientSize();
                            const float nx = (static_cast<float>(rm.lLastX) / 65535.f) * g_cliW;
                            const float ny = (static_cast<float>(rm.lLastY) / 65535.f) * g_cliH;
                            MoveCursor(nx - g_vx.load(std::memory_order_relaxed),
                                       ny - g_vy.load(std::memory_order_relaxed));
                        }
                        else
                        {
                            const float dx = static_cast<float>(static_cast<short>(rm.lLastX));
                            const float dy = static_cast<float>(static_cast<short>(rm.lLastY));
                            if (dx != 0.f || dy != 0.f) MoveCursor(dx, dy);
                        }

                        if (rm.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)   ButtonEvent(0, true);
                        if (rm.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)     ButtonEvent(0, false);
                        if (rm.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)  ButtonEvent(1, true);
                        if (rm.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)    ButtonEvent(1, false);
                        if (rm.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) ButtonEvent(2, true);
                        if (rm.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)   ButtonEvent(2, false);
                        if (rm.usButtonFlags & RI_MOUSE_WHEEL)
                        {
                            std::lock_guard<std::mutex> lk(g_mtx);
                            g_pending.wheel += static_cast<float>(static_cast<short>(rm.usButtonData))
                                               / static_cast<float>(WHEEL_DELTA);
                        }
                    }
                    else if (ri->header.dwType == RIM_TYPEKEYBOARD)
                    {
                        const RAWKEYBOARD& rk = ri->data.keyboard;
                        const int vk = static_cast<int>(rk.VKey);

                        if (vk > 0 && vk < 256)
                        {
                            const bool down = (rk.Flags & RI_KEY_BREAK) == 0;
                            std::lock_guard<std::mutex> lk(g_mtx);
                            if (down)
                            {
                                if (!g_pKDown[vk]) g_pKPress[vk] = true;
                                g_pKDown[vk] = true;
                            }
                            else
                            {
                                g_pKDown[vk] = false;
                                g_pKRelease[vk] = true;
                            }
                        }
                    }
                }
            }
            return 0;   
        }
        return DefWindowProcW(h, m, wp, lp);
    }

    HWND MakeRawWindow()
    {
        static const wchar_t* kCls = L"NewDLL_RawWnd";
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc   = RawProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCls;
        RegisterClassExW(&wc);   
        return CreateWindowExW(0, kCls, L"", WS_POPUP, 0, 0, 0, 0,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    DWORD WINAPI HookThread(LPVOID)
    {
        g_rawWnd = MakeRawWindow();
        if (g_rawWnd)
        {
            RAWINPUTDEVICE rid[2]{};

            rid[0].usUsagePage = 0x01;              
            rid[0].usUsage     = 0x02;              
            rid[0].dwFlags     = RIDEV_INPUTSINK;   
            rid[0].hwndTarget  = g_rawWnd;

            rid[1].usUsagePage = 0x01;
            rid[1].usUsage     = 0x06;              
            rid[1].dwFlags     = RIDEV_INPUTSINK;
            rid[1].hwndTarget  = g_rawWnd;

            const BOOL ok = RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
            g_rawOk.store(ok == TRUE, std::memory_order_relaxed);
            Log::info("[input] raw input (mouse+kb) win=%p register=%d err=%lu",
                      reinterpret_cast<void*>(g_rawWnd), ok ? 1 : 0,
                      ok ? 0ul : GetLastError());
        }
        else
        {
            Log::error("[input] raw input window creation failed err=%lu", GetLastError());
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
}

namespace Input
{
    void Install()
    {
        static bool once = false;
        if (once) return;
        once = true;
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    }

    void Poll()
    {
        std::lock_guard<std::mutex> lk(g_mtx);

        if (g_center.exchange(false, std::memory_order_relaxed))
        {
            RefreshClientSize();
            if (g_cliW > 2.f) g_vx.store(g_cliW * 0.5f, std::memory_order_relaxed);
            if (g_cliH > 2.f) g_vy.store(g_cliH * 0.5f, std::memory_order_relaxed);
            g_vValid.store(true, std::memory_order_relaxed);
        }

        if (!g_rawOk.load(std::memory_order_relaxed))
        {
            POINT p{};
            if (::GetCursorPos(&p))
            {
                HWND w = FindGameWindow();
                if (w) ScreenToClient(w, &p);
                g_vx.store(static_cast<float>(p.x), std::memory_order_relaxed);
                g_vy.store(static_cast<float>(p.y), std::memory_order_relaxed);
                g_vValid.store(true, std::memory_order_relaxed);
            }
        }

        g_frame.x = g_vx.load(std::memory_order_relaxed);
        g_frame.y = g_vy.load(std::memory_order_relaxed);
        g_frame.dx = g_frame.x - g_prevX;
        g_frame.dy = g_frame.y - g_prevY;
        g_prevX = g_frame.x;
        g_prevY = g_frame.y;
        g_frame.valid = g_vValid.load(std::memory_order_relaxed);

        for (int i = 0; i < 3; ++i)
        {
            g_frame.down[i]      = g_pending.down[i];
            g_frame.pressed[i]   = g_pending.pressed[i];
            g_frame.released[i]  = g_pending.released[i];
            g_pending.pressed[i]  = false;
            g_pending.released[i] = false;
        }
        g_frame.wheel = g_pending.wheel;
        g_pending.wheel = 0.f;

        memcpy(g_fKDown,  g_pKDown,  sizeof(g_fKDown));
        memcpy(g_fKPress, g_pKPress, sizeof(g_fKPress));
        memset(g_pKPress,   0, sizeof(g_pKPress));
        memset(g_pKRelease, 0, sizeof(g_pKRelease));

        if (g_menuOpen.load(std::memory_order_relaxed))
        {
            ::ClipCursor(nullptr);      
            if (!g_hidCursor && CursorShowing())
            {
                ::ShowCursor(FALSE);
                g_hidCursor = true;
            }
        }
        else if (g_hidCursor)
        {
            if (!CursorShowing()) ::ShowCursor(TRUE);
            g_hidCursor = false;
        }
    }

    const MouseState& Mouse() { return g_frame; }

    bool KeyDown(int vk)
    {
        if (vk < 0 || vk >= 256) return false;
        return g_fKDown[vk];
    }

    bool KeyPressed(int vk)
    {
        if (vk < 0 || vk >= 256) return false;
        return g_fKPress[vk];
    }

    void SetMenuOpen(bool open)
    {
        g_menuOpen.store(open, std::memory_order_relaxed);
        if (open && !g_prevOpen.exchange(true, std::memory_order_relaxed))
            g_center.store(true, std::memory_order_relaxed);   
        else if (!open)
            g_prevOpen.store(false, std::memory_order_relaxed);
    }

    bool MenuOpen() { return g_menuOpen.load(std::memory_order_relaxed); }

    void ClientSize(float& w, float& h)
    {
        w = 0.f; h = 0.f;
        HWND wnd = FindGameWindow();
        if (!wnd) return;
        RECT r{};
        if (GetClientRect(wnd, &r))
        {
            w = static_cast<float>(r.right - r.left);
            h = static_cast<float>(r.bottom - r.top);
        }
    }
}
