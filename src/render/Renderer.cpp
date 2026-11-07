#include "render/Renderer.hpp"

#include <d2d1helper.h>
#include <wrl/client.h>
#include <unordered_map>

#include "core/Log.hpp"

using Microsoft::WRL::ComPtr;

namespace
{
    ID2D1Factory1*        g_factory   = nullptr;
    ID2D1Device*          g_d2dDevice = nullptr;
    ID2D1DeviceContext*   g_d2dCtx    = nullptr;
    IDWriteFactory*       g_dwFactory = nullptr;
    ID2D1SolidColorBrush* g_brush     = nullptr;

    ComPtr<ID3D11Device>          g_dev11;
    ComPtr<ID3D11DeviceContext>   g_ctx11;
    ComPtr<ID3D11On12Device>      g_on12;
    ComPtr<ID3D12CommandQueue>    g_queue;
    bool                          g_isD3D12 = false;

    ComPtr<ID3D12Resource>     g_bb12;
    ComPtr<ID3D11Texture2D>    g_bb11;
    ComPtr<ID3D11Resource>     g_wrapped;
    ComPtr<IDXGISurface>       g_surface;
    ComPtr<ID2D1Bitmap1>       g_target;
    bool                       g_frame = false;

    float g_w = 0.f, g_h = 0.f;

    std::unordered_map<int, IDWriteTextFormat*> g_formats;

    IDWriteTextFormat* Format(float size)
    {
        const int key = static_cast<int>(size * 10.f + 0.5f);
        auto it = g_formats.find(key);
        if (it != g_formats.end()) return it->second;
        if (!g_dwFactory) return nullptr;

        static const wchar_t* kFamilies[] = {
            L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei", L"Segoe UI", L"Arial"
        };

        IDWriteTextFormat* f = nullptr;
        for (const wchar_t* fam : kFamilies)
        {
            if (SUCCEEDED(g_dwFactory->CreateTextFormat(
                    fam, nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    size, L"zh-cn", &f)) && f)
                break;
            f = nullptr;
        }
        if (!f)
        {
            static bool logged = false;
            if (!logged) { logged = true; Log::error("[render] CreateTextFormat 全部回退字体都失败"); }
            g_formats[key] = nullptr;
            return nullptr;
        }

        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        g_formats[key] = f;
        return f;
    }

    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c)
    {
        if (!g_d2dCtx) return nullptr;
        if (!g_brush)
        {
            if (FAILED(g_d2dCtx->CreateSolidColorBrush(c, &g_brush)) || !g_brush)
                return nullptr;
        }
        g_brush->SetColor(c);
        return g_brush;
    }
}

namespace Renderer
{
    bool IsReady() { return g_d2dCtx != nullptr; }
    float Width()  { return g_w; }
    float Height() { return g_h; }

    bool EnsureInit(IDXGISwapChain3* sc, ID3D12CommandQueue* gameQueue)
    {
        if (g_d2dCtx) return true;
        if (!sc) return false;

        HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(g_dev11.GetAddressOf()));
        if (SUCCEEDED(hr) && g_dev11)
        {
            g_isD3D12 = false;
            g_dev11->GetImmediateContext(g_ctx11.GetAddressOf());
            Log::info("[render] swapchain is D3D11");
        }
        else
        {
            ComPtr<ID3D12Device> dev12;
            hr = sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(dev12.GetAddressOf()));
            if (FAILED(hr) || !dev12)
            {
                Log::error("[render] neither D3D11 nor D3D12 device on swapchain (hr=0x%08X)", hr);
                return false;
            }
            if (!gameQueue)
            {
                Log::error("[render] D3D12 game but command queue not captured yet");
                return false;
            }

            g_isD3D12 = true;
            D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
            };
            IUnknown* queues[] = { gameQueue };
            D3D_FEATURE_LEVEL chosen{};
            hr = D3D11On12CreateDevice(
                dev12.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, _countof(levels),
                queues, 1, 0,
                g_dev11.GetAddressOf(), g_ctx11.GetAddressOf(), &chosen);
            if (FAILED(hr) || !g_dev11)
            {
                Log::error("[render] D3D11On12CreateDevice failed (hr=0x%08X)", hr);
                return false;
            }
            g_dev11.As(&g_on12);
            g_queue = gameQueue;
            Log::info("[render] swapchain is D3D12 -> D3D11On12 bridge ok (fl=0x%X)", chosen);
        }

        D2D1_FACTORY_OPTIONS fo{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                               &fo, reinterpret_cast<void**>(&g_factory));
        if (FAILED(hr) || !g_factory) { Log::error("[render] D2D1CreateFactory 0x%08X", hr); return false; }

        ComPtr<IDXGIDevice> dxgiDev;
        g_dev11.As(&dxgiDev);
        hr = g_factory->CreateDevice(dxgiDev.Get(), &g_d2dDevice);
        if (FAILED(hr) || !g_d2dDevice) { Log::error("[render] D2D CreateDevice 0x%08X", hr); return false; }

        hr = g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2dCtx);
        if (FAILED(hr) || !g_d2dCtx) { Log::error("[render] D2D CreateDeviceContext 0x%08X", hr); return false; }

        g_d2dCtx->SetDpi(96.f, 96.f);            
        g_d2dCtx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_d2dCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&g_dwFactory));
        if (FAILED(hr) || !g_dwFactory) { Log::error("[render] DWriteCreateFactory 0x%08X", hr); return false; }

        Log::info("[render] renderer ready");
        return true;
    }

    void FailLog(const char* why, HRESULT hr = S_OK)
    {
        static int n = 0;
        if (n >= 60) return;
        ++n;
        Log::error("[render] BeginFrame failed: %s (hr=0x%08X)", why, static_cast<unsigned>(hr));
    }

    bool BeginFrame(IDXGISwapChain3* sc)
    {
        if (!g_d2dCtx || !sc) { FailLog("renderer not ready"); return false; }

        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT hr = sc->GetDesc(&desc);
        if (FAILED(hr)) { FailLog("GetDesc", hr); return false; }

        g_w = static_cast<float>(desc.BufferDesc.Width);
        g_h = static_cast<float>(desc.BufferDesc.Height);
        if (g_w <= 1.f || g_h <= 1.f) { FailLog("zero/one size"); return false; }

        auto AbortAcquired = [](const char* why, HRESULT e) -> bool
        {
            if (g_isD3D12 && g_on12 && g_wrapped)
            {
                ID3D11Resource* r = g_wrapped.Get();
                g_on12->ReleaseWrappedResources(&r, 1);
                if (g_ctx11) g_ctx11->Flush();
            }
            g_wrapped.Reset(); g_surface.Reset(); g_target.Reset();
            g_bb12.Reset();    g_bb11.Reset();
            FailLog(why, e);
            return false;
        };

        g_surface.Reset();
        g_target.Reset();

        const UINT bbIndex = sc->GetCurrentBackBufferIndex();

        if (g_isD3D12)
        {
            if (!g_on12) { FailLog("no ID3D11On12Device"); return false; }
            g_bb12.Reset(); g_wrapped.Reset();

            hr = sc->GetBuffer(bbIndex, __uuidof(ID3D12Resource),
                               reinterpret_cast<void**>(g_bb12.GetAddressOf()));
            if (FAILED(hr) || !g_bb12) { FailLog("GetBuffer(D3D12Resource)", hr); return false; }

            D3D11_RESOURCE_FLAGS rf{};
            rf.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = g_on12->CreateWrappedResource(
                    g_bb12.Get(), &rf,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
                    __uuidof(ID3D11Resource),
                    reinterpret_cast<void**>(g_wrapped.GetAddressOf()));
            if (FAILED(hr) || !g_wrapped) { FailLog("CreateWrappedResource", hr); return false; }

            ID3D11Resource* res = g_wrapped.Get();
            g_on12->AcquireWrappedResources(&res, 1);

            hr = g_wrapped.As(&g_surface);
            if (FAILED(hr) || !g_surface) return AbortAcquired("QI IDXGISurface (D3D12 path)", hr);
        }
        else
        {
            g_bb11.Reset();
            hr = sc->GetBuffer(bbIndex, __uuidof(ID3D11Texture2D),
                               reinterpret_cast<void**>(g_bb11.GetAddressOf()));
            if (FAILED(hr) || !g_bb11) { FailLog("GetBuffer(D3D11Texture2D)", hr); return false; }

            hr = g_bb11.As(&g_surface);
            if (FAILED(hr) || !g_surface) { FailLog("QI IDXGISurface (D3D11 path)", hr); return false; }
        }

        DXGI_FORMAT fmt = desc.BufferDesc.Format;
        if (fmt == DXGI_FORMAT_UNKNOWN) fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt, D2D1_ALPHA_MODE_IGNORE),
            96.f, 96.f);

        hr = g_d2dCtx->CreateBitmapFromDxgiSurface(g_surface.Get(), &bp, &g_target);
        if (FAILED(hr) || !g_target)
        {
            
            D2D1_BITMAP_PROPERTIES1 bp2 = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.f, 96.f);
            hr = g_d2dCtx->CreateBitmapFromDxgiSurface(g_surface.Get(), &bp2, &g_target);
            if (FAILED(hr) || !g_target)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    Log::error("[render] CreateBitmapFromDxgiSurface failed 0x%08X (fmt=%d)",
                               static_cast<unsigned>(hr), static_cast<int>(fmt));
                }
                return AbortAcquired("CreateBitmapFromDxgiSurface", hr);
            }
        }

        g_d2dCtx->SetTarget(g_target.Get());
        g_d2dCtx->SetTransform(D2D1::Matrix3x2F::Identity());
        g_d2dCtx->BeginDraw();
        g_frame = true;
        return true;
    }

    void EndFrame()
    {
        if (!g_frame) return;
        g_frame = false;

        static bool s_first = false;
        if (!s_first)
        {
            s_first = true;
            Log::info("[render] first overlay frame drawn (%gx%g, d3d12=%d)",
                      static_cast<double>(g_w), static_cast<double>(g_h), g_isD3D12 ? 1 : 0);
        }

        const HRESULT endHr = g_d2dCtx->EndDraw();
        if (FAILED(endHr))
        {
            static int n = 0;
            if (n < 20)
            {
                ++n;
                Log::error("[render] EndDraw failed 0x%08X", static_cast<unsigned>(endHr));
            }
            
            if (endHr == D2DERR_RECREATE_TARGET && g_brush)
            {
                g_brush->Release();
                g_brush = nullptr;
            }
        }
        g_d2dCtx->SetTarget(nullptr);
        g_target.Reset();

        if (g_isD3D12)
        {
            if (g_on12 && g_wrapped)
            {
                ID3D11Resource* res = g_wrapped.Get();
                g_on12->ReleaseWrappedResources(&res, 1);
                g_ctx11->Flush();          
            }
            g_wrapped.Reset();
            g_bb12.Reset();
        }
        else
        {
            g_bb11.Reset();
        }
        g_surface.Reset();
    }

    void Shutdown()
    {
        for (auto& kv : g_formats) if (kv.second) kv.second->Release();
        g_formats.clear();
        if (g_brush)     { g_brush->Release();     g_brush = nullptr; }
        if (g_d2dCtx)    { g_d2dCtx->Release();    g_d2dCtx = nullptr; }
        if (g_d2dDevice) { g_d2dDevice->Release(); g_d2dDevice = nullptr; }
        if (g_dwFactory) { g_dwFactory->Release(); g_dwFactory = nullptr; }
        if (g_factory)   { g_factory->Release();   g_factory = nullptr; }
        g_target.Reset(); g_surface.Reset(); g_wrapped.Reset();
        g_bb12.Reset(); g_bb11.Reset(); g_on12.Reset(); g_queue.Reset();
        g_ctx11.Reset(); g_dev11.Reset();
    }

    void FillRect(float x, float y, float w, float h, D2D1_COLOR_F c)
    {
        if (!g_frame) return;
        auto* b = Brush(c);
        if (b) g_d2dCtx->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
    }

    void FillRoundRect(float x, float y, float w, float h, float rx, float ry, D2D1_COLOR_F c)
    {
        if (!g_frame) return;
        auto* b = Brush(c);
        if (b) g_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), b);
    }

    void StrokeRoundRect(float x, float y, float w, float h, float rx, float ry,
                         D2D1_COLOR_F c, float thickness)
    {
        if (!g_frame) return;
        auto* b = Brush(c);
        if (b) g_d2dCtx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), b, thickness);
    }

    void FillRectGradientV(float x, float y, float w, float h, D2D1_COLOR_F top, D2D1_COLOR_F bottom)
    {
        if (!g_frame) return;
        D2D1_GRADIENT_STOP stops[2];
        stops[0].position = 0.f; stops[0].color = top;
        stops[1].position = 1.f; stops[1].color = bottom;

        ID2D1GradientStopCollection* coll = nullptr;
        if (FAILED(g_d2dCtx->CreateGradientStopCollection(stops, 2, &coll)) || !coll) return;

        ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(g_d2dCtx->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, y), D2D1::Point2F(x, y + h)),
                coll, &lb)) && lb)
        {
            g_d2dCtx->FillRectangle(D2D1::RectF(x, y, x + w, y + h), lb);
            lb->Release();
        }
        coll->Release();
    }

    void DrawLine(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float thickness)
    {
        if (!g_frame) return;
        auto* b = Brush(c);
        if (b) g_d2dCtx->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, thickness);
    }

    void FillPolygon(const D2D1_POINT_2F* points, int count, D2D1_COLOR_F c)
    {
        if (!g_frame || !g_factory || !points || count < 3) return;
        auto* b = Brush(c);
        if (!b) return;

        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(g_factory->CreatePathGeometry(&geo)) || !geo) return;

        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geo->Open(&sink)) || !sink) { geo->Release(); return; }

        sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(points + 1, static_cast<UINT32>(count - 1));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();

        g_d2dCtx->FillGeometry(geo, b);
        geo->Release();
    }

    float MeasureText(const std::wstring& text, float fontSize)
    {
        if (!g_dwFactory || text.empty()) return 0.f;
        IDWriteTextFormat* f = Format(fontSize);
        if (!f) return 0.f;

        IDWriteTextLayout* layout = nullptr;
        if (FAILED(g_dwFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                 f, 10000.f, fontSize * 2.f, &layout)) || !layout)
            return 0.f;

        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return m.widthIncludingTrailingWhitespace;
    }

    void DrawText(const std::wstring& text, float x, float y, float fontSize,
                  D2D1_COLOR_F color, TextAlign align)
    {
        if (!g_frame || text.empty()) return;
        IDWriteTextFormat* f = Format(fontSize);
        if (!f) return;
        auto* b = Brush(color);
        if (!b) return;

        const float w = MeasureText(text, fontSize);
        const float h = fontSize * 1.5f;

        float left = x;
        if (align == TextAlign::Center)      left = x - w * 0.5f;
        else if (align == TextAlign::Right)  left = x - w;

        const D2D1_RECT_F r = D2D1::RectF(left - 1.f, y, left + w + 3.f, y + h);
        g_d2dCtx->DrawText(text.c_str(), static_cast<UINT32>(text.size()), f, r, b,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}
