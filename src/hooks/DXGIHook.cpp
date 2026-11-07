#include "hooks/DXGIHook.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>

#include "core/Log.hpp"
#include "game/Game.hpp"
#include "input/Input.hpp"
#include "render/Renderer.hpp"
#include "ui/ClickGui.hpp"
#include "ui/Hud.hpp"

namespace
{

    constexpr int kPresentIndex = 8;

    constexpr int kExecuteListsIndex = 10;

    using Present_t   = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ExecLists_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

    Present_t   oPresent   = nullptr;
    ExecLists_t oExecLists = nullptr;

    std::atomic<ID3D12CommandQueue*> g_lastQueue{ nullptr };

    void** VtSlot(void* obj, int index)
    {
        if (!obj) return nullptr;
        void** vt = *reinterpret_cast<void***>(obj);
        return vt ? &vt[index] : nullptr;
    }

    bool PatchSlot(void** slot, void* hook, void** original)
    {
        if (!slot || !hook) return false;

        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            return false;

        if (*slot == hook)                   
        {
            DWORD tmp = 0;
            VirtualProtect(slot, sizeof(void*), old, &tmp);

            return *original != nullptr;
        }

        *original = *slot;
        *slot     = hook;

        DWORD tmp = 0;
        VirtualProtect(slot, sizeof(void*), old, &tmp);
        return true;
    }

    LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        return DefWindowProcW(h, m, w, l);
    }

    HWND MakeDummyWindow()
    {
        static const wchar_t* kClass = L"NewDLL_DummyWnd";
        static bool reg = false;
        if (!reg)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc   = DummyProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            reg = true;
        }
        return CreateWindowExW(0, kClass, L"", WS_OVERLAPPEDWINDOW,
                               0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    void** FindPresentViaHwnd()
    {
        HWND hwnd = MakeDummyWindow();
        if (!hwnd)
        {
            Log::warn("[hook] dummy window creation failed (err=%lu)", GetLastError());
            return nullptr;
        }

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount        = 1;
        sd.BufferDesc.Width   = 8;
        sd.BufferDesc.Height  = 8;
        sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow       = hwnd;
        sd.SampleDesc.Count   = 1;
        sd.Windowed           = TRUE;
        sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain*       sc  = nullptr;
        ID3D11Device*         dev = nullptr;
        ID3D11DeviceContext*  ctx = nullptr;
        D3D_FEATURE_LEVEL     fl{};
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, _countof(levels), D3D11_SDK_VERSION,
            &sd, &sc, &dev, &fl, &ctx);

        void** slot = nullptr;
        if (SUCCEEDED(hr) && sc)
            slot = VtSlot(sc, kPresentIndex);
        else
            Log::warn("[hook] D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)", hr);

        if (ctx) ctx->Release();
        if (dev) dev->Release();
        if (sc)  sc->Release();
        DestroyWindow(hwnd);
        return slot;
    }

    void** FindPresentViaComposition()
    {
        ID3D11Device*        dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL    fl{};
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };

        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                     levels, _countof(levels), D3D11_SDK_VERSION,
                                     &dev, &fl, &ctx)) || !dev)
            return nullptr;

        void**         slot    = nullptr;
        IDXGIDevice*   dxgiDev = nullptr;
        IDXGIAdapter*  adapter = nullptr;
        IDXGIFactory2* fac     = nullptr;

        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter &&
            SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac))) && fac)
        {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width       = 8;
            sd.Height      = 8;
            sd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

            IDXGISwapChain1* sc = nullptr;
            if (SUCCEEDED(fac->CreateSwapChainForComposition(dev, &sd, nullptr, &sc)) && sc)
            {
                slot = VtSlot(sc, kPresentIndex);
                sc->Release();
            }
            else
            {
                Log::warn("[hook] CreateSwapChainForComposition failed");
            }
        }

        if (fac)     fac->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
        if (ctx)     ctx->Release();
        if (dev)     dev->Release();
        return slot;
    }

    void** FindExecListsD3D12()
    {
        ID3D12Device* dev = nullptr;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(&dev))) || !dev)
            return nullptr;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ID3D12CommandQueue* q    = nullptr;
        void**              slot = nullptr;
        if (SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                              reinterpret_cast<void**>(&q))) && q)
        {
            slot = VtSlot(q, kExecuteListsIndex);
            q->Release();
        }
        dev->Release();
        return slot;
    }

    void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* q, UINT num,
                                                 ID3D12CommandList* const* lists)
    {
        g_lastQueue.store(q, std::memory_order_relaxed);
        if (oExecLists) oExecLists(q, num, lists);
    }

    HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        
        if (!oPresent) return E_FAIL;

        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3)
        {

            if (!Renderer::IsReady())
            {
                static int s_frame = 0;
                ++s_frame;
                if ((s_frame % 30) == 1)
                {
                    ID3D12CommandQueue* q = g_lastQueue.load(std::memory_order_relaxed);
                    if (q)
                    {
                        
                        ID3D12Device* qDev = nullptr;
                        q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&qDev));

                        ID3D12Device* scDev = nullptr;
                        sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&scDev));

                        const bool same = (qDev && scDev && qDev == scDev);
                        if (qDev)  qDev->Release();
                        if (scDev) scDev->Release();

                        if (same)
                            Renderer::EnsureInit(sc3, q);
                    }
                }
            }

            Input::Poll();

            Game::OnFrame();

            const bool ready = Renderer::IsReady();

            if (Input::KeyPressed(VK_INSERT) && ready)
            {
                ClickGui::Toggle();
                Log::info("[ui] menu %s", ClickGui::IsOpen() ? "OPEN" : "closed");
            }

            if (ready)
            {

                ClickGui::Update();

                Hud::Tick();
            }

            const bool edFree = ready && ClickGui::EditorFreeRunning();
            Input::SetMenuOpen((ClickGui::IsOpen() || edFree) && ready);

            if (ready)
            {
                const bool wantGui = ClickGui::Visible();
                const bool wantHud = Hud::AnyEnabled();

                if (wantGui || wantHud || edFree)
                {
                    if (Renderer::BeginFrame(sc3))
                    {

                        if (wantHud)          Hud::Render();
                        if (wantGui)          ClickGui::Render();
                        else if (edFree)      ClickGui::RenderEditor();
                        Renderer::EndFrame();
                    }
                }
            }

            sc3->Release();
        }
        return oPresent(sc, sync, flags);
    }
}

namespace Hooks
{
    void Init()
    {

        static bool once = false;
        if (once) return;
        once = true;

        Input::Install();

        void** presentSlot = FindPresentViaHwnd();
        if (!presentSlot)
        {
            Log::warn("[hook] hwnd dummy swapchain unavailable, trying composition");
            presentSlot = FindPresentViaComposition();
        }

        if (presentSlot)
        {
            Log::info("[hook] Present slot=%p  original=%p", presentSlot, *presentSlot);
            if (PatchSlot(presentSlot, reinterpret_cast<void*>(&hkPresent),
                          reinterpret_cast<void**>(&oPresent)))
                Log::info("[hook] Present hooked -> oPresent=%p", oPresent);
            else
                Log::error("[hook] failed to patch Present slot");
        }
        else
        {
            Log::error("[hook] could not resolve Present slot");
        }

        void** eclSlot = FindExecListsD3D12();
        if (eclSlot)
        {
            Log::info("[hook] ExecuteCommandLists slot=%p  original=%p", eclSlot, *eclSlot);
            if (PatchSlot(eclSlot, reinterpret_cast<void*>(&hkExecuteCommandLists),
                          reinterpret_cast<void**>(&oExecLists)))
                Log::info("[hook] ExecuteCommandLists hooked -> oExec=%p", oExecLists);
            else
                Log::error("[hook] failed to patch ExecuteCommandLists slot");
        }
        else
        {
            Log::warn("[hook] D3D12 not available; queue capture disabled");
        }
    }
}
