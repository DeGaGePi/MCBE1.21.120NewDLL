#pragma once

#include <Windows.h>

#ifdef DrawText
#undef DrawText
#endif
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
namespace Renderer
{

    bool EnsureInit(IDXGISwapChain3* swapChain, ID3D12CommandQueue* gameQueue);

    bool BeginFrame(IDXGISwapChain3* swapChain);

    void EndFrame();

    void Shutdown();

    bool  IsReady();
    float Width();
    float Height();

    enum class TextAlign { Left, Center, Right };

    void FillRect(float x, float y, float w, float h, D2D1_COLOR_F color);
    void FillRoundRect(float x, float y, float w, float h, float rx, float ry, D2D1_COLOR_F color);
    void StrokeRoundRect(float x, float y, float w, float h, float rx, float ry,
                         D2D1_COLOR_F color, float thickness);
    void FillRectGradientV(float x, float y, float w, float h,
                           D2D1_COLOR_F top, D2D1_COLOR_F bottom);
    void DrawLine(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness);

    void FillPolygon(const D2D1_POINT_2F* points, int count, D2D1_COLOR_F color);

    void DrawText(const std::wstring& text, float x, float y, float fontSize,
                  D2D1_COLOR_F color, TextAlign align = TextAlign::Left);
    float MeasureText(const std::wstring& text, float fontSize);

    inline D2D1_COLOR_F Color(int r, int g, int b, int a = 255)
    {
        return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
    }
}
