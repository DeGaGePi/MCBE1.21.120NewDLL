#include "ui/Hud.hpp"
#include "ui/Module.hpp"
#include "input/Input.hpp"
#include "render/Renderer.hpp"
#include "game/Game.hpp"

#include <Windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using Renderer::TextAlign;

namespace
{

    uint64_t g_frames  = 0;
    uint64_t g_fpsMark = 0;
    float    g_fps     = 0.f;

    int      g_clickL  = 0, g_clickR = 0;
    uint64_t g_cpsMark = 0;
    float    g_cpsL    = 0.f, g_cpsR = 0.f;

    constexpr float kPad = 14.f;

    enum ElemId { E_WM = 0, E_AL, E_FPS, E_CPS, E_KS, E_COORD, E_COUNT };

    enum Anchor { AN_TL = 0, AN_TR, AN_BL, AN_BR };

    struct Pos
    {
        int   anchor;
        float ox, oy;                
    };

    const Pos kDefault[E_COUNT] =
    {
        { AN_TL, kPad, kPad           },   
        { AN_TR, kPad, kPad           },   
        { AN_BR, kPad, kPad           },   
        { AN_BR, kPad, kPad + 21.f    },   
        { AN_BL, kPad, kPad           },   
        { AN_TL, kPad, kPad + 24.f    },   
    };

    Pos g_pos[E_COUNT];

    struct InitOnce { InitOnce() { for (int i = 0; i < E_COUNT; ++i) g_pos[i] = kDefault[i]; } } g_initOnce;

    const wchar_t* kLabels[E_COUNT] = {
        L"Watermark", L"ArrayList", L"FPS", L"CPS", L"Keystrokes", L"Coordinates"
    };

    void AnchorToLT(const Pos& p, float w, float h, float sw, float sh,
                    float& left, float& top)
    {
        switch (p.anchor)
        {
        case AN_TL: left = p.ox;             top = p.oy;             break;
        case AN_TR: left = sw - p.ox - w;    top = p.oy;             break;
        case AN_BL: left = p.ox;             top = sh - p.oy - h;    break;
        default:    left = sw - p.ox - w;    top = sh - p.oy - h;    break;   
        }
    }

    bool ModEnabled(Feature f)
    {
        for (auto& c : Modules::All())
            for (auto& m : c.mods)
                if (m.feat == f) return m.enabled;
        return false;
    }

    float SettingF(Feature f, const wchar_t* name, float def)
    {
        for (auto& c : Modules::All())
            for (auto& m : c.mods)
                if (m.feat == f)
                    for (auto& s : m.settings)
                        if (s.name == name) return s.f;
        return def;
    }

    int SettingI(Feature f, const wchar_t* name, int def)
    {
        for (auto& c : Modules::All())
            for (auto& m : c.mods)
                if (m.feat == f)
                    for (auto& s : m.settings)
                        if (s.name == name) return s.idx;
        return def;
    }

    bool SettingB(Feature f, const wchar_t* name, bool def)
    {
        for (auto& c : Modules::All())
            for (auto& m : c.mods)
                if (m.feat == f)
                    for (auto& s : m.settings)
                        if (s.name == name) return s.b;
        return def;
    }

    std::wstring Fmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        return std::wstring(buf);
    }

    void ShadowText(const std::wstring& t, float x, float y, float size,
                    D2D1_COLOR_F col, TextAlign al = TextAlign::Left)
    {
        Renderer::DrawText(t, x + 1.f, y + 1.f, size, Renderer::Color(0, 0, 0, 170), al);
        Renderer::DrawText(t, x, y, size, col, al);
    }

    enum Style { ST_TEXT = 0, ST_CHIP = 1, ST_KEYBOX = 2 };

    struct Cmd
    {
        int          id    = 0;
        int          style = ST_TEXT;
        std::wstring text;
        float        x = 0, y = 0;        
        float        size = 13.f;
        float        bw = 0, bh = 0;      
        bool         down = false;        
        D2D1_COLOR_F col = {};
        bool         underline = false;   
        
        float ex = 0, ey = 0, ew = 0, eh = 0;
    };

    void BuildCmds(std::vector<Cmd>& out)
    {
        const float sw = Renderer::Width();
        const float sh = Renderer::Height();
        if (sw < 2.f || sh < 2.f) return;

        const D2D1_COLOR_F white  = Renderer::Color(236, 236, 242, 235);
        const D2D1_COLOR_F dim    = Renderer::Color(170, 170, 180, 225);
        const D2D1_COLOR_F accent = Renderer::Color(139,  92, 246, 255);
        const D2D1_COLOR_F green  = Renderer::Color( 80, 220, 130, 235);
        const D2D1_COLOR_F track  = Renderer::Color( 18,  18,  22, 165);

        auto place = [&](int id, float w, float h, float& left, float& top)
        {
            AnchorToLT(g_pos[id], w, h, sw, sh, left, top);
        };

        if (ModEnabled(Feature::HudWatermark))
        {
            const bool showVer = SettingB(Feature::HudWatermark, L"Show Version", true);
            const std::wstring t = showVer ? L"NewDLL  v0.2" : L"NewDLL";
            const float size = 16.f;
            const float w = Renderer::MeasureText(t, size);
            const float h = size * 1.35f + 2.f;

            float left = 0, top = 0;
            place(E_WM, w, h, left, top);

            Cmd c;
            c.id = E_WM; c.style = ST_TEXT; c.text = t;
            c.x = left; c.y = top; c.size = size; c.col = white;
            c.underline = true;
            c.ex = left; c.ey = top; c.ew = w; c.eh = h;
            out.push_back(std::move(c));
        }

        if (ModEnabled(Feature::HudCoords))
        {
            float x = 0.f, y = 0.f, z = 0.f;
            const bool ok = Game::PlayerPos(x, y, z);

            std::wstring t;
            if (ok)
            {
                if (SettingB(Feature::HudCoords, L"Show Chunk", true))
                    t = Fmt(L"XYZ  %.2f  %.2f  %.2f    chunk %d %d",
                            static_cast<double>(x), static_cast<double>(y), static_cast<double>(z),
                            static_cast<int>(std::floor(x)) >> 4,
                            static_cast<int>(std::floor(z)) >> 4);
                else
                    t = Fmt(L"XYZ  %.2f  %.2f  %.2f",
                            static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
            }
            else
            {
                t = L"XYZ  --   locating player...";
            }

            const float size = 13.f;
            const float w = Renderer::MeasureText(t, size);
            const float h = size * 1.45f;

            float left = 0, top = 0;
            place(E_COORD, w, h, left, top);

            Cmd c;
            c.id = E_COORD; c.style = ST_TEXT; c.text = t;
            c.x = left; c.y = top; c.size = size;
            c.col = ok ? white : dim;
            c.ex = left; c.ey = top; c.ew = w; c.eh = h;
            out.push_back(std::move(c));
        }

        if (ModEnabled(Feature::HudArrayList))
        {
            const float scale = SettingF(Feature::HudArrayList, L"Scale", 1.f);
            const float size  = 13.5f * scale;

            std::vector<std::wstring> lines;
            for (auto& c : Modules::All())
                for (auto& m : c.mods)
                {
                    if (!m.enabled || !m.implemented) continue;
                    if (m.feat == Feature::HudArrayList ||
                        m.feat == Feature::HudWatermark ||
                        m.feat == Feature::HudEditor) continue;      
                    lines.push_back(m.name);
                }

            if (!lines.empty())
            {
                const float lineH = size * 1.42f;
                const float blkH  = lineH * static_cast<float>(lines.size());

                float maxW = 0.f;
                for (auto& l : lines)
                {
                    const float w = Renderer::MeasureText(l, size) + 12.f;
                    if (w > maxW) maxW = w;
                }

                float left = 0, top = 0;
                place(E_AL, maxW, blkH, left, top);

                const float right = left + maxW;
                for (size_t i = 0; i < lines.size(); ++i)
                {
                    const float w = Renderer::MeasureText(lines[i], size);
                    const float lx = right - w;
                    const float ly = top + lineH * static_cast<float>(i);

                    Cmd c;
                    c.id = E_AL; c.style = ST_CHIP; c.text = lines[i];
                    c.x = lx; c.y = ly; c.size = size; c.col = white;
                    c.ex = lx - 7.f; c.ey = ly - 1.f;
                    c.ew = w + 12.f; c.eh = size * 1.5f;
                    out.push_back(std::move(c));
                }
            }
        }

        {
            
            const float size = 14.f;
            const float lineH = size * 1.4f;

            int stack = 0;
            if (ModEnabled(Feature::HudCps)) ++stack;
            if (ModEnabled(Feature::HudFps)) ++stack;

            if (stack > 0)
            {
                
                float left = 0, top = 0;
                place(E_FPS, 90.f, lineH * stack, left, top);

                float y = top;
                if (ModEnabled(Feature::HudCps))
                {
                    const bool leftOnly = SettingI(Feature::HudCps, L"Watch", 0) == 1;
                    const float v = leftOnly ? g_cpsL : (g_cpsL + g_cpsR);
                    const std::wstring t = Fmt(L"CPS  %.0f", static_cast<double>(v));
                    const float w = Renderer::MeasureText(t, size);

                    Cmd c;
                    c.id = E_CPS; c.style = ST_TEXT; c.text = t;
                    c.x = left; c.y = y; c.size = size; c.col = white;
                    c.ex = left; c.ey = y; c.ew = w; c.eh = lineH;
                    y += lineH;
                    out.push_back(std::move(c));
                }
                if (ModEnabled(Feature::HudFps))
                {
                    const std::wstring t = Fmt(L"FPS  %.0f", static_cast<double>(g_fps));
                    const float w = Renderer::MeasureText(t, size);

                    Cmd c;
                    c.id = E_FPS; c.style = ST_TEXT; c.text = t;
                    c.x = left; c.y = y; c.size = size;
                    c.col = (g_fps >= 55.f) ? green : white;
                    c.ex = left; c.ey = y; c.ew = w; c.eh = lineH;
                    out.push_back(std::move(c));
                }
            }
        }

        if (ModEnabled(Feature::HudKeystrokes))
        {
            const float scale = SettingF(Feature::HudKeystrokes, L"Scale", 1.f);
            const float bw  = 30.f * scale;
            const float bh  = 30.f * scale;
            const float gap =  5.f * scale;

            const float wS  = 48.f * scale;
            const float wSp = 62.f * scale;
            const float wM  = 42.f * scale;

            const float rowW = bw * 3.f + gap * 2.f;
            const float row3W = wS + wSp + wM * 2.f + gap * 3.f;
            const float blkW = (row3W > rowW) ? row3W : rowW;
            const float blkH = bh * 3.f + gap * 2.f;

            float left = 0, top = 0;
            place(E_KS, blkW, blkH, left, top);

            const auto& M = Input::Mouse();
            const bool shift = Input::KeyDown(VK_SHIFT) ||
                               Input::KeyDown(VK_LSHIFT) ||
                               Input::KeyDown(VK_RSHIFT);

            auto keyBox = [&](float x, float y, float w, const wchar_t* label, bool down)
            {
                Cmd c;
                c.id = E_KS; c.style = ST_KEYBOX; c.text = label;
                c.x = x; c.y = y; c.size = 13.f * scale;
                c.bw = w; c.bh = bh; c.down = down;
                c.ex = x; c.ey = y; c.ew = w; c.eh = bh;
                out.push_back(std::move(c));
            };

            keyBox(left + bw + gap, top, bw, L"W", Input::KeyDown('W'));

            const float y2 = top + bh + gap;
            keyBox(left,                  y2, bw, L"A", Input::KeyDown('A'));
            keyBox(left + bw + gap,       y2, bw, L"S", Input::KeyDown('S'));
            keyBox(left + bw * 2.f + gap * 2.f, y2, bw, L"D", Input::KeyDown('D'));

            const float y3 = y2 + bh + gap;
            float x3 = left;
            keyBox(x3, y3, wS,  L"SHIFT", shift);                     x3 += wS + gap;
            keyBox(x3, y3, wSp, L"SPACE", Input::KeyDown(VK_SPACE));  x3 += wSp + gap;
            keyBox(x3, y3, wM,  L"LMB",   M.down[0]);                 x3 += wM + gap;
            keyBox(x3, y3, wM,  L"RMB",   M.down[1]);
        }
    }

    inline D2D1_COLOR_F AccentC() { return Renderer::Color(139, 92, 246, 255); }
}

namespace Hud
{
    bool AnyEnabled()
    {
        return ModEnabled(Feature::HudWatermark) ||
               ModEnabled(Feature::HudArrayList) ||
               ModEnabled(Feature::HudFps) ||
               ModEnabled(Feature::HudCps) ||
               ModEnabled(Feature::HudKeystrokes) ||
               ModEnabled(Feature::HudCoords);
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();

        ++g_frames;
        if (g_fpsMark == 0) g_fpsMark = now;
        if (now - g_fpsMark >= 500)
        {
            g_fps     = static_cast<float>(g_frames) * 1000.f /
                        static_cast<float>(now - g_fpsMark);
            g_frames  = 0;
            g_fpsMark = now;
        }

        if (g_cpsMark == 0) g_cpsMark = now;
        const auto& M = Input::Mouse();
        if (M.pressed[0]) ++g_clickL;
        if (M.pressed[1]) ++g_clickR;
        if (now - g_cpsMark >= 1000)
        {
            const float span = static_cast<float>(now - g_cpsMark);
            g_cpsL = static_cast<float>(g_clickL) * 1000.f / span;
            g_cpsR = static_cast<float>(g_clickR) * 1000.f / span;
            g_clickL = g_clickR = 0;
            g_cpsMark = now;
        }
    }

    void Render()
    {
        std::vector<Cmd> cmds;
        BuildCmds(cmds);

        for (const auto& c : cmds)
        {
            if (c.style == ST_KEYBOX)
            {
                const float r = c.bh * 0.1667f;
                Renderer::FillRoundRect(c.x, c.y, c.bw, c.bh, r, r,
                                        c.down ? Renderer::Color(139, 92, 246, 215)
                                               : Renderer::Color(18, 18, 22, 165));
                ShadowText(c.text, c.x + c.bw * 0.5f, c.y + (c.bh - c.size * 1.15f) * 0.5f,
                           c.size,
                           c.down ? Renderer::Color(255, 255, 255, 245)
                                  : Renderer::Color(170, 170, 180, 225),
                           TextAlign::Center);
                continue;
            }

            const float w = Renderer::MeasureText(c.text, c.size);

            if (c.style == ST_CHIP)
            {
                Renderer::FillRect(c.x - 7.f, c.y - 1.f, w + 12.f, c.size * 1.5f,
                                   Renderer::Color(0, 0, 0, 105));
                Renderer::FillRect(c.x + w + 2.f, c.y - 1.f, 2.f, c.size * 1.5f, AccentC());
            }

            ShadowText(c.text, c.x, c.y, c.size, c.col);

            if (c.underline)
                Renderer::FillRect(c.x, c.y + c.size * 1.35f, w, 2.f, AccentC());
        }
    }

    int Items(Item* out, int maxItems)
    {
        if (!out || maxItems <= 0) return 0;

        std::vector<Cmd> cmds;
        BuildCmds(cmds);

        const float sw = Renderer::Width();
        const float sh = Renderer::Height();

        int n = 0;
        for (int id = 0; id < E_COUNT && n < maxItems; ++id)
        {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            bool  any = false;

            for (const auto& c : cmds)
            {
                if (c.id != id) continue;
                if (!any)
                {
                    x0 = c.ex; y0 = c.ey;
                    x1 = c.ex + c.ew; y1 = c.ey + c.eh;
                    any = true;
                }
                else
                {
                    if (c.ex < x0) x0 = c.ex;
                    if (c.ey < y0) y0 = c.ey;
                    if (c.ex + c.ew > x1) x1 = c.ex + c.ew;
                    if (c.ey + c.eh > y1) y1 = c.ey + c.eh;
                }
            }

            float w = 0.f, h = 0.f;
            if (any)
            {
                w = x1 - x0;
                h = y1 - y0;
            }
            else
            {
                
                w = Renderer::MeasureText(kLabels[id], 12.f) + 16.f;
                h = 20.f;
                AnchorToLT(g_pos[id], w, h, sw, sh, x0, y0);
            }

            out[n].id     = id;
            out[n].name   = kLabels[id];
            out[n].x      = x0;
            out[n].y      = y0;
            out[n].w      = w;
            out[n].h      = h;
            out[n].active = any;
            ++n;
        }
        return n;
    }

    void SetItemPos(int id, float left, float top)
    {
        if (id < 0 || id >= E_COUNT) return;

        const float sw = Renderer::Width();
        const float sh = Renderer::Height();
        if (sw < 2.f || sh < 2.f) return;

        float w = 90.f, h = 20.f;
        {
            Item all[E_COUNT];
            const int n = Items(all, E_COUNT);
            for (int i = 0; i < n; ++i)
                if (all[i].id == id) { w = all[i].w; h = all[i].h; }
        }

        if (left < 0.f) left = 0.f;
        if (top  < 0.f) top  = 0.f;
        if (left > sw - 24.f) left = sw - 24.f;
        if (top  > sh - 16.f) top  = sh - 16.f;

        Pos& p = g_pos[id];
        switch (p.anchor)
        {
        case AN_TL: p.ox = left;             p.oy = top;             break;
        case AN_TR: p.ox = sw - left - w;    p.oy = top;             break;
        case AN_BL: p.ox = left;             p.oy = sh - top - h;    break;
        default:    p.ox = sw - left - w;    p.oy = sh - top - h;    break;
        }

        if (p.ox < 0.f) p.ox = 0.f;
        if (p.oy < 0.f) p.oy = 0.f;
        if (p.ox > sw)  p.ox = sw;
        if (p.oy > sh)  p.oy = sh;
    }

    void ResetPositions()
    {
        for (int i = 0; i < E_COUNT; ++i) g_pos[i] = kDefault[i];
    }
}
