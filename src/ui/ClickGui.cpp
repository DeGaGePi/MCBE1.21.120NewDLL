#include "ui/ClickGui.hpp"
#include "ui/Module.hpp"
#include "ui/Hud.hpp"
#include "input/Input.hpp"
#include "render/Renderer.hpp"
#include "game/Game.hpp"
#include "core/Log.hpp"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using Renderer::TextAlign;

namespace
{
    
    D2D1_COLOR_F Accent()   { return Renderer::Color(139,  92, 246); }   
    D2D1_COLOR_F PanelBg()  { return Renderer::Color( 20,  20,  24); }
    D2D1_COLOR_F HeaderBg() { return Renderer::Color( 32,  32,  38); }
    D2D1_COLOR_F SubBg()    { return Renderer::Color( 13,  13,  17); }
    D2D1_COLOR_F TextOn()   { return Renderer::Color(238, 238, 242); }
    D2D1_COLOR_F TextOff()  { return Renderer::Color(150, 150, 160); }
    D2D1_COLOR_F TextDim()  { return Renderer::Color( 96,  96, 106); }
    D2D1_COLOR_F TrackOff() { return Renderer::Color( 62,  62,  70); }
    D2D1_COLOR_F TrackBg()  { return Renderer::Color( 44,  44,  52); }
    D2D1_COLOR_F OkDot()    { return Renderer::Color( 80, 220, 130); }

    inline D2D1_COLOR_F A(D2D1_COLOR_F c, float a) { c.a = a; return c; }
    inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    float g_catW = 220.f;
    inline float CatW() { return g_catW; }

    constexpr float kHeaderH = 32.f;
    constexpr float kModH    = 26.f;
    constexpr float kSetH    = 26.f;
    constexpr float kSldH    = 34.f;
    constexpr float kGap     = 14.f;
    constexpr float kRadius  = 6.f;
    constexpr float kIndent  = 14.f;
    constexpr float kPadR    = 12.f;
    inline float ContentX() { return g_catW - kPadR - 4.f; }   

    bool     g_open      = false;
    float    g_anim      = 0.f;
    float    g_dt        = 0.016f;
    uint64_t g_lastTick  = 0;
    bool     g_layoutOk  = false;
    int      g_cols      = 0;
    int      g_drag      = -1;
    bool     g_dragMoved = false;
    float    g_dragOffX  = 0.f, g_dragOffY = 0.f;
    float    g_pressX    = 0.f, g_pressY   = 0.f;
    int      g_tab       = 0;
    float    g_ux = 0.f, g_uw = 0.f;

    int      g_editDrag  = -1;
    float    g_editOffX  = 0.f, g_editOffY = 0.f;

    Setting* g_dropdown  = nullptr;   
    Setting* g_slider    = nullptr;   
    Setting* g_keyCap    = nullptr;   

    float g_dropX = 0.f, g_dropY = 0.f, g_dropW = 0.f, g_dropItemH = 20.f;
    int   g_dropCount = 0;

    float g_sldX = 0.f, g_sldW = 1.f;

    struct Xf
    {
        float cx = 0, cy = 0, s = 1.f;
        float X(float v) const { return cx + (v - cx) * s; }
        float Y(float v) const { return cy + (v - cy) * s; }
        float S(float v) const { return v * s; }
    } g_xf;

    void FRound(float x, float y, float w, float h, float r, D2D1_COLOR_F c)
    {
        Renderer::FillRoundRect(g_xf.X(x), g_xf.Y(y), g_xf.S(w), g_xf.S(h),
                                r * g_xf.s, r * g_xf.s, c);
    }
    void FRect(float x, float y, float w, float h, D2D1_COLOR_F c)
    {
        Renderer::FillRect(g_xf.X(x), g_xf.Y(y), g_xf.S(w), g_xf.S(h), c);
    }
    void FCircle(float x, float y, float d, D2D1_COLOR_F c)
    {
        FRound(x, y, d, d, d * 0.5f, c);
    }
    void FTextVC(const std::wstring& t, float x, float rowY, float rowH, float size,
                 D2D1_COLOR_F c, TextAlign al = TextAlign::Left)
    {
        Renderer::DrawText(t, g_xf.X(x), g_xf.Y(rowY + (rowH - size * 1.45f) * 0.5f),
                           g_xf.S(size), c, al);
    }

    bool Hit(float x, float y, float w, float h, float mx, float my)
    {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    }

    void MouseBuf(float& mx, float& my)
    {
        const auto& m = Input::Mouse();
        mx = m.x; my = m.y;
        float cw = 0, ch = 0;
        Input::ClientSize(cw, ch);
        const float bw = Renderer::Width(), bh = Renderer::Height();
        if (cw > 1.f && ch > 1.f && bw > 1.f && bh > 1.f)
        {
            mx *= bw / cw;
            my *= bh / ch;
        }
    }

    void MouseUI(float& mx, float& my)
    {
        MouseBuf(mx, my);
        if (g_xf.s > 0.01f)
        {
            mx = g_xf.cx + (mx - g_xf.cx) / g_xf.s;
            my = g_xf.cy + (my - g_xf.cy) / g_xf.s;
        }
    }

    void DrawCursor()
    {
        float mx = 0, my = 0;
        MouseBuf(mx, my);

        static const float P[7][2] = {
            {  0.0f,  0.0f }, {  0.0f, 17.0f }, {  4.5f, 13.1f }, {  7.2f, 18.7f },
            {  9.8f, 17.5f }, {  7.1f, 12.0f }, { 11.7f, 11.7f }
        };

        D2D1_POINT_2F outer[7], inner[7];
        for (int i = 0; i < 7; ++i)
        {
            outer[i] = D2D1_POINT_2F{ mx + P[i][0] * 1.20f + 0.5f, my + P[i][1] * 1.20f + 0.5f };
            inner[i] = D2D1_POINT_2F{ mx + P[i][0],                 my + P[i][1] };
        }
        Renderer::FillPolygon(outer, 7, Renderer::Color(8, 8, 12, 235));
        Renderer::FillPolygon(inner, 7, Renderer::Color(250, 250, 254));
    }

    float SettingH(const Setting& s)
    {
        return (s.type == SettingType::Float || s.type == SettingType::Int) ? kSldH : kSetH;
    }

    float SettingsBlockH(const Module& m)
    {
        float h = 5.f;
        for (const auto& s : m.settings) h += SettingH(s);
        return h + 3.f;
    }

    float ModuleH(const Module& m)
    {
        float h = kModH;
        if (m.anim > 0.001f)
        {
            h += SettingsBlockH(m) * m.anim;
        }
        return h;
    }

    float ColumnH(const Category& c)
    {
        if (!c.open) return kHeaderH;
        float h = kHeaderH + 4.f;
        for (const auto& m : c.mods) h += ModuleH(m);
        return h + 5.f;
    }

    void Layout()
    {
        const float sw = Renderer::Width();
        const float sh = Renderer::Height();
        auto& cats = Modules::All();
        const int n = static_cast<int>(cats.size());
        if (n < 1) { g_layoutOk = true; return; }
        if (sw < 2.f || sh < 2.f) return;

        const float top = 88.f;                       

        auto colOfName = [](const std::wstring& nm) -> int
        {
            if (nm == L"Combat" || nm == L"HUD") return 0;   
            if (nm == L"Visual")   return 1;
            if (nm == L"Movement") return 2;
            if (nm == L"Player")   return 3;
            if (nm == L"Misc")     return 4;
            return 5;                                        
        };

        std::vector<int> colOf(static_cast<size_t>(n), 0);
        int cols = 1;
        for (int i = 0; i < n; ++i)
        {
            int c = colOfName(cats[static_cast<size_t>(i)].name);
            if (c < 0) c = 0;
            if (c > 5) c = 5;
            colOf[static_cast<size_t>(i)] = c;
            if (c + 1 > cols) cols = c + 1;
        }
        if (cols < 1) cols = 1;

        float w = (sw - 20.f - (static_cast<float>(cols) - 1.f) * kGap) / static_cast<float>(cols);
        if (w > 220.f) w = 220.f;
        if (w < 128.f) w = 128.f;
        g_catW = w;

        const float total = static_cast<float>(cols) * g_catW +
                            (static_cast<float>(cols) - 1.f) * kGap;
        float x0 = (sw - total) * 0.5f;
        if (x0 < 8.f) x0 = 8.f;

        std::vector<float> cy(static_cast<size_t>(cols), top);
        for (int i = 0; i < n; ++i)
        {
            auto& c = cats[static_cast<size_t>(i)];
            const int col = colOf[static_cast<size_t>(i)];

            const float nx = x0 + static_cast<float>(col) * (g_catW + kGap);
            const float ny = cy[static_cast<size_t>(col)];
            cy[static_cast<size_t>(col)] = ny + ColumnH(c) + kGap;

            if (!c.moved) { c.x = nx; c.y = ny; }
        }

        g_cols     = cols;
        g_layoutOk = true;
    }

    std::wstring KeyName(int vk)
    {
        if (vk <= 0) return L"None";
        switch (vk)
        {
        case VK_LBUTTON:  return L"Mouse L";
        case VK_RBUTTON:  return L"Mouse R";
        case VK_MBUTTON:  return L"Mouse M";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        case VK_SHIFT:    return L"SHIFT";
        case VK_CONTROL:  return L"CTRL";
        case VK_MENU:     return L"ALT";
        case VK_SPACE:    return L"SPACE";
        case VK_TAB:      return L"TAB";
        case VK_RETURN:   return L"ENTER";
        case VK_ESCAPE:   return L"ESC";
        case VK_BACK:     return L"BACK";
        case VK_DELETE:   return L"DEL";
        case VK_INSERT:   return L"INSERT";
        case VK_HOME:     return L"HOME";
        case VK_END:      return L"END";
        case VK_PRIOR:    return L"PGUP";
        case VK_NEXT:     return L"PGDN";
        case VK_UP:       return L"UP";
        case VK_DOWN:     return L"DOWN";
        case VK_LEFT:     return L"LEFT";
        case VK_RIGHT:    return L"RIGHT";
        default: break;
        }
        wchar_t buf[48]{};
        if (vk >= VK_F1 && vk <= VK_F24)
        {
            swprintf_s(buf, L"F%d", vk - VK_F1 + 1);
            return buf;
        }
        const UINT sc = ::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        if (sc && ::GetKeyNameTextW(static_cast<LONG>(sc << 16), buf, 48) > 0 && buf[0])
            return buf;
        swprintf_s(buf, L"VK %02X", vk);
        return buf;
    }

    void ApplySliderFromX(Setting& s, float mx)
    {
        float t = (mx - g_sldX) / (g_sldW > 1.f ? g_sldW : 1.f);
        t = Clamp01(t);

        if (s.type == SettingType::Float)
        {
            float v = s.fmin + t * (s.fmax - s.fmin);
            if (s.fstep > 0.f) v = std::roundf(v / s.fstep) * s.fstep;
            if (v < s.fmin) v = s.fmin;
            if (v > s.fmax) v = s.fmax;
            s.f = v;
        }
        else
        {
            int v = s.imin + static_cast<int>(t * static_cast<float>(s.imax - s.imin) + 0.5f);
            if (v < s.imin) v = s.imin;
            if (v > s.imax) v = s.imax;
            s.i = v;
        }
    }

    void DrawSetting(const Category& c, Setting& s, float sy, float mx, float my, float a)
    {
        const float rowX = c.x + kIndent + 4.f;
        const float rowW = CatW() - kIndent - kPadR - 8.f;

        switch (s.type)
        {
        
        case SettingType::Bool:
        {
            FTextVC(s.name, rowX, sy, kSetH, 12.5f, A(s.b ? TextOn() : TextOff(), a));

            const float sw = 26.f, sh = 14.f;
            const float sx = c.x + ContentX() - sw;
            const float syy = sy + (kSetH - sh) * 0.5f;

            FRound(sx, syy, sw, sh, sh * 0.5f, A(s.b ? Accent() : TrackOff(), a));
            const float kd = sh - 5.f;
            const float kx = s.b ? (sx + sw - kd - 2.5f) : (sx + 2.5f);
            FCircle(kx, syy + 2.5f, kd, A(Renderer::Color(245, 245, 250), a));
            break;
        }

        case SettingType::Float:
        case SettingType::Int:
        {
            
            FTextVC(s.name, rowX, sy, 17.f, 12.5f, A(TextOff(), a));

            wchar_t buf[64]{};
            if (s.type == SettingType::Float) swprintf_s(buf, L"%.2f", s.f);
            else                              swprintf_s(buf, L"%d", s.i);
            Renderer::DrawText(buf, g_xf.X(c.x + ContentX()), g_xf.Y(sy + 1.f),
                               g_xf.S(12.5f), A(Accent(), a), TextAlign::Right);

            const float slX = rowX;
            const float slW = rowW;
            const float slY = sy + 17.f;
            const float slH = 7.f;

            FRound(slX, slY, slW, slH, slH * 0.5f, A(TrackBg(), a));

            float t = 0.f;
            if (s.type == SettingType::Float)
                t = (s.fmax - s.fmin) > 0.f ? (s.f - s.fmin) / (s.fmax - s.fmin) : 0.f;
            else
                t = (s.imax != s.imin) ? static_cast<float>(s.i - s.imin) / static_cast<float>(s.imax - s.imin) : 0.f;
            t = Clamp01(t);

            if (t > 0.001f)
                FRound(slX, slY, slW * t, slH, slH * 0.5f, A(Accent(), a));

            const float hd = 13.f;
            const float hx = slX + slW * t - hd * 0.5f;
            FCircle(hx, slY + slH * 0.5f - hd * 0.5f, hd, A(Renderer::Color(250, 250, 254), a));

            if (g_slider == &s)
            {
                g_sldX = g_xf.X(slX);
                g_sldW = g_xf.S(slW);
            }
            break;
        }

        case SettingType::Enum:
        {
            FTextVC(s.name, rowX, sy, kSetH, 12.5f, A(TextOff(), a));

            const float bw = 104.f, bh = 18.f;
            const float bx = c.x + ContentX() - bw;
            const float by = sy + (kSetH - bh) * 0.5f;
            const bool  open = (g_dropdown == &s);

            FRound(bx, by, bw, bh, 4.f,
                   A(open ? Renderer::Color(52, 52, 64) : TrackBg(), a));

            if (s.idx >= 0 && s.idx < static_cast<int>(s.options.size()))
            {
                Renderer::DrawText(s.options[s.idx], g_xf.X(bx + 8.f), g_xf.Y(by + 1.5f),
                                   g_xf.S(11.5f), A(TextOn(), a), TextAlign::Left);
            }
            
            FRound(bx + bw - 14.f, by + bh * 0.5f - 1.f, 7.f, 2.f, 1.f, A(TextOff(), a));

            if (open)
            {
                g_dropX     = g_xf.X(bx);
                g_dropY     = g_xf.Y(by + bh + 2.f);
                g_dropW     = g_xf.S(bw);
                g_dropItemH = 20.f;
                g_dropCount = static_cast<int>(s.options.size());
            }
            break;
        }

        case SettingType::Key:
        {
            FTextVC(s.name, rowX, sy, kSetH, 12.5f, A(TextOff(), a));

            const float bw = 104.f, bh = 18.f;
            const float bx = c.x + ContentX() - bw;
            const float by = sy + (kSetH - bh) * 0.5f;

            const bool cap = (g_keyCap == &s);

            FRound(bx, by, bw, bh, 4.f, A(cap ? Accent() : TrackBg(), a));
            Renderer::StrokeRoundRect(g_xf.X(bx), g_xf.Y(by), g_xf.S(bw), g_xf.S(bh),
                                      4.f * g_xf.s, 4.f * g_xf.s,
                                      A(Renderer::Color(255, 255, 255), 0.06f * a), 1.f);

            const std::wstring txt = cap ? L"[ press key ]" : KeyName(s.key);
            Renderer::DrawText(txt, g_xf.X(bx + bw * 0.5f), g_xf.Y(by + 1.5f),
                               g_xf.S(cap ? 10.5f : 11.5f),
                               A(cap ? Renderer::Color(255, 255, 255) : TextOn(), a),
                               TextAlign::Center);
            break;
        }
        }
    }

    void DrawModule(const Category& c, Module& m, float ry, float mx, float my, float anim)
    {
        const float rowX = c.x + 4.f;
        const float rowW = CatW() - 8.f;
        const bool  hover = Hit(c.x, ry, CatW(), kModH, mx, my);

        if (hover)
            FRound(rowX, ry + 1.f, rowW, kModH - 2.f, 4.f, A(Renderer::Color(255, 255, 255), 0.06f * anim));

        D2D1_COLOR_F nameCol = m.enabled ? TextOn() : TextOff();
        if (!m.implemented) nameCol = TextDim();
        FTextVC(m.name, c.x + 15.f, ry, kModH, 13.f, A(nameCol, anim));

        {
            const float nameW = Renderer::MeasureText(m.name, 13.f) / (g_xf.s > 0.01f ? g_xf.s : 1.f);
            float mx0 = c.x + 17.f + nameW + 6.f;

            const float cap = c.x + ContentX() - 46.f;
            if (mx0 > cap) mx0 = cap;

            if (m.implemented)
            {
                const float d = 5.f;
                FCircle(mx0, ry + (kModH - d) * 0.5f, d, A(OkDot(), 0.92f * anim));
            }
            else
            {

                FTextVC(L"N/A", mx0 - 2.f, ry, kModH, 10.f,
                        A(Renderer::Color(150, 90, 90), 0.85f * anim));
            }
        }

        if (!m.settings.empty())
        {
            const float ax = c.x + ContentX() - 40.f;
            const float ay = ry + kModH * 0.5f;
            const float s = 4.f;
            D2D1_POINT_2F tri[3];
            if (m.expanded) { tri[0] = { ax - s, ay + s * 0.6f }; tri[1] = { ax + s, ay + s * 0.6f }; tri[2] = { ax, ay - s * 0.8f }; }
            else            { tri[0] = { ax - s, ay - s * 0.6f }; tri[1] = { ax + s, ay - s * 0.6f }; tri[2] = { ax, ay + s * 0.8f }; }
            for (int i = 0; i < 3; ++i) { tri[i].x = g_xf.X(tri[i].x); tri[i].y = g_xf.Y(tri[i].y); }
            Renderer::FillPolygon(tri, 3, A(TextOff(), anim));
        }

        const float tw = 30.f, th = 15.f;
        const float tx = c.x + ContentX() - tw;
        const float ty = ry + (kModH - th) * 0.5f;
        const D2D1_COLOR_F trk = m.enabled ? Accent()
                                              : (m.implemented ? TrackOff()
                                                               : Renderer::Color(33, 33, 39));
        FRound(tx, ty, tw, th, th * 0.5f, A(trk, anim));
        const float kd = th - 5.f;
        const float kx = m.enabled ? (tx + tw - kd - 2.5f) : (tx + 2.5f);
        FCircle(kx, ty + 2.5f, kd, A(Renderer::Color(245, 245, 250), anim));

        if (m.anim > 0.01f && !m.settings.empty())
        {
            const float blockH = SettingsBlockH(m);
            const float sy0 = ry + kModH;

            FRound(c.x + 6.f, sy0, CatW() - 12.f, blockH * m.anim, 4.f,
                   A(SubBg(), 0.85f * m.anim * anim));

            float sy = sy0 + 5.f;
            for (auto& s : m.settings)
            {
                DrawSetting(c, s, sy, mx, my, anim * m.anim);
                sy += SettingH(s);
            }
        }
    }

    void DrawColumn(Category& c)
    {
        const float anim = g_anim;
        const float h = ColumnH(c);

        FRound(c.x, c.y, CatW(), h, kRadius, A(PanelBg(), 0.97f * anim));
        Renderer::StrokeRoundRect(g_xf.X(c.x), g_xf.Y(c.y), g_xf.S(CatW()), g_xf.S(h),
                                  kRadius * g_xf.s, kRadius * g_xf.s,
                                  A(Renderer::Color(255, 255, 255), 0.05f * anim), 1.f);

        FRound(c.x, c.y, CatW(), kHeaderH, kRadius, A(HeaderBg(), anim));
        FRect(c.x, c.y + kHeaderH - kRadius, CatW(), kRadius, A(HeaderBg(), anim));
        FRound(c.x + 9.f, c.y + kHeaderH * 0.5f - 7.f, 3.f, 14.f, 1.5f, A(Accent(), anim));
        FTextVC(c.name, c.x + 19.f, c.y, kHeaderH, 15.f, A(TextOn(), anim));
        FRound(c.x + CatW() - 22.f, c.y + kHeaderH * 0.5f - 1.5f, 10.f, 3.f, 1.5f, A(TextOff(), anim));

        if (!c.open) return;

        float mx = 0, my = 0;
        MouseBuf(mx, my);

        float ry = c.y + kHeaderH + 3.f;
        for (auto& m : c.mods)
        {
            DrawModule(c, m, ry, mx, my, anim);
            ry += ModuleH(m);
        }
    }

    void DrawDropdownLayer()
    {
        if (!g_dropdown || g_dropCount <= 0) return;

        const float itemH = g_dropItemH;
        const int   n     = g_dropCount;
        const float W = g_dropW, X = g_dropX, Y = g_dropY;
        const float H = itemH * static_cast<float>(n) + 5.f;

        Renderer::FillRoundRect(X, Y, W, H, 5.f, 5.f, Renderer::Color(28, 28, 34, 248));
        Renderer::StrokeRoundRect(X, Y, W, H, 5.f, 5.f,
                                  Renderer::Color(255, 255, 255, 22), 1.f);

        float mx = 0, my = 0;
        MouseBuf(mx, my);

        for (int i = 0; i < n; ++i)
        {
            const float iy = Y + 2.5f + itemH * static_cast<float>(i);
            const bool  sel = (i == g_dropdown->idx);
            const bool  hov = Hit(X, iy, W, itemH, mx, my);

            if (hov)
                Renderer::FillRect(X + 3.f, iy, W - 6.f, itemH, Renderer::Color(255, 255, 255, 20));

            if (i < static_cast<int>(g_dropdown->options.size()))
            {
                Renderer::DrawText(g_dropdown->options[i], X + 9.f, iy + 2.5f, 11.5f,
                                   sel ? Accent() : TextOn(), TextAlign::Left);
            }
        }
    }

    const wchar_t* kTabNames[3] = { L"ClickGui", L"HudEditor", L"Scripting" };

    void DrawTabs(float anim)
    {
        const float fs = 17.f, pad = 26.f;
        const float sw = Renderer::Width();

        float totalW = 0.f;
        for (auto* n : kTabNames) totalW += Renderer::MeasureText(n, fs) + pad;
        totalW -= pad;

        float x = (sw - totalW) * 0.5f;
        const float y = 16.f;
        const float bandH = fs * 1.6f;

        FRound(x - 14.f, y - 6.f, totalW + 28.f, bandH + 12.f, 9.f,
               A(Renderer::Color(22, 22, 26), 0.74f * anim));

        float activeX = x, activeW = 0.f;
        for (int i = 0; i < 3; ++i)
        {
            const float w = Renderer::MeasureText(kTabNames[i], fs) + pad;
            const bool active = (i == g_tab);
            FTextVC(kTabNames[i], x + pad * 0.5f, y, bandH, fs,
                    A(active ? TextOn() : TextOff(), anim));
            if (active) { activeX = x; activeW = w; }
            x += w;
        }

        const float t = 1.f - expf(-g_dt * 12.f);
        g_ux += (activeX - g_ux) * t;
        g_uw += (activeW - g_uw) * t;
        FRound(g_ux, y + bandH - 1.f, g_uw, 2.f, 1.f, A(Accent(), anim));
    }

    bool ClickSetting(Category& c, Setting& s, float sy, float ux, float uy, float screenX)
    {
        const float rowX = c.x + kIndent + 4.f;
        const float rowW = CatW() - kIndent - kPadR - 8.f;

        switch (s.type)
        {
        case SettingType::Bool:
        {
            const float sw = 26.f, sh = 14.f;
            const float sx = c.x + ContentX() - sw;
            const float syy = sy + (kSetH - sh) * 0.5f;
            if (Hit(sx - 4.f, syy - 6.f, sw + 8.f, sh + 12.f, ux, uy))
            {
                s.b = !s.b;
                return true;
            }
            return false;
        }

        case SettingType::Float:
        case SettingType::Int:
        {
            const float slX = rowX;
            const float slW = rowW;
            const float slY = sy + 17.f;
            const float slH = 7.f;

            if (Hit(slX - 3.f, slY - 7.f, slW + 6.f, slH + 14.f, ux, uy))
            {
                g_slider = &s;
                g_sldX = g_xf.X(slX);
                g_sldW = g_xf.S(slW);
                ApplySliderFromX(s, screenX);
                return true;
            }
            return false;
        }

        case SettingType::Enum:
        {
            const float bw = 104.f, bh = 18.f;
            const float bx = c.x + ContentX() - bw;
            const float by = sy + (kSetH - bh) * 0.5f;
            if (Hit(bx - 4.f, by - 4.f, bw + 8.f, bh + 8.f, ux, uy))
            {
                g_dropdown  = &s;
                g_dropX     = g_xf.X(bx);
                g_dropY     = g_xf.Y(by + bh + 2.f);
                g_dropW     = g_xf.S(bw);
                g_dropItemH = 20.f;
                g_dropCount = static_cast<int>(s.options.size());
                return true;
            }
            return false;
        }

        case SettingType::Key:
        {
            const float bw = 104.f, bh = 18.f;
            const float bx = c.x + ContentX() - bw;
            const float by = sy + (kSetH - bh) * 0.5f;
            if (Hit(bx - 4.f, by - 4.f, bw + 8.f, bh + 8.f, ux, uy))
            {
                g_keyCap = &s;
                return true;
            }
            return false;
        }
        }
        return false;
    }

    bool HudEditorOn()
    {
        for (auto& c : Modules::All())
            for (auto& m : c.mods)
                if (m.feat == Feature::HudEditor) return m.enabled;
        return false;
    }

    bool HudEditorLabels()
    {
        for (auto& c : Modules::All())
            for (auto& m : c.mods)
                if (m.feat == Feature::HudEditor)
                    for (auto& s : m.settings)
                        if (s.name == L"Show Labels") return s.b;
        return true;
    }

    void DrawHudEditorOverlay(float anim, bool withHint)
    {
        Hud::Item items[16];
        const int n = Hud::Items(items, 16);

        float mx = 0, my = 0;
        MouseBuf(mx, my);

        for (int i = 0; i < n; ++i)
        {
            const auto& it = items[i];
            const bool hot = (g_editDrag == it.id) || Hit(it.x, it.y, it.w, it.h, mx, my);

            Renderer::FillRect(it.x, it.y, it.w, it.h,
                               A(Renderer::Color(139, 92, 246), (hot ? 0.20f : 0.07f) * anim));

            const D2D1_COLOR_F line = A(Renderer::Color(190, 150, 255), (hot ? 0.95f : 0.5f) * anim);
            Renderer::StrokeRoundRect(it.x, it.y, it.w, it.h, 3.f, 3.f, line, hot ? 1.7f : 1.f);

            if (HudEditorLabels())
                Renderer::DrawText(it.name, it.x + 2.f, it.y - 17.f, 12.f,
                                   A(Renderer::Color(226, 226, 240), 0.92f * anim), TextAlign::Left);
        }

        if (withHint)
        {
            const float sw = Renderer::Width(), sh = Renderer::Height();
            Renderer::DrawText(L"HudEditor  ·  drag to move  ·  right-click empty space to reset",
                               sw * 0.5f, sh - 42.f, 13.f,
                               A(Renderer::Color(200, 200, 212), 0.85f * anim), TextAlign::Center);
        }
    }

    void UpdateEditor()
    {
        const bool pageOn = (g_open && g_tab == 1);
        if (!HudEditorOn() && !pageOn) { g_editDrag = -1; return; }
        if (g_open && g_tab != 1)      { g_editDrag = -1; return; }

        float mx = 0, my = 0;
        MouseBuf(mx, my);
        const auto& M = Input::Mouse();

        Hud::Item items[16];
        const int n = Hud::Items(items, 16);

        if (g_editDrag < 0)
        {
            if (M.pressed[0])
            {
                for (int i = 0; i < n; ++i)
                {
                    if (Hit(items[i].x, items[i].y, items[i].w, items[i].h, mx, my))
                    {
                        g_editDrag = items[i].id;
                        g_editOffX = mx - items[i].x;
                        g_editOffY = my - items[i].y;
                        Log::info("[hud] drag \"%ls\"", items[i].name.c_str());
                        break;
                    }
                }
            }
            else if (M.pressed[1])
            {
                bool onItem = false;
                for (int i = 0; i < n; ++i)
                    if (Hit(items[i].x, items[i].y, items[i].w, items[i].h, mx, my))
                    { onItem = true; break; }

                if (!onItem)                    
                {
                    Hud::ResetPositions();
                    Log::info("[hud] positions reset to default");
                }
            }
        }
        else
        {
            if (M.down[0])
            {
                for (int i = 0; i < n; ++i)
                    if (items[i].id == g_editDrag)
                    {
                        Hud::SetItemPos(g_editDrag, mx - g_editOffX, my - g_editOffY);
                        break;
                    }
            }
            else
            {
                g_editDrag = -1;
            }
        }
    }
}

namespace ClickGui
{
    void Toggle()  { g_open = !g_open; }
    bool IsOpen()  { return g_open; }
    bool Visible() { return g_anim > 0.002f; }

    bool EditorActive()
    {
        return HudEditorOn() || (g_open && g_tab == 1);
    }

    bool EditorFreeRunning()
    {
        return HudEditorOn() && !Visible();
    }

    void RenderEditor()
    {
        DrawHudEditorOverlay(1.f, true);
    }

    void Relayout() { Layout(); }

    void Update()
    {
        const uint64_t now = GetTickCount64();
        g_dt = (g_lastTick == 0) ? 0.016f : static_cast<float>(now - g_lastTick) / 1000.f;
        g_lastTick = now;
        if (g_dt > 0.1f) g_dt = 0.1f;

        const float target = g_open ? 1.f : 0.f;
        g_anim += (target - g_anim) * (1.f - expf(-g_dt * 14.f));
        if (!g_open && g_anim < 0.002f) g_anim = 0.f;

        for (auto& c : Modules::All())
            for (auto& m : c.mods)
            {
                const float t = m.expanded ? 1.f : 0.f;
                m.anim += (t - m.anim) * (1.f - expf(-g_dt * 17.f));
                if (!m.expanded && m.anim < 0.004f) m.anim = 0.f;
            }

        Modules::Sync();

        if (Renderer::Width() > 1.f) Layout();
        if (g_anim < 0.55f)
        {
            g_drag = -1; g_slider = nullptr; g_dropdown = nullptr; g_keyCap = nullptr;
            return;
        }

        float mx = 0, my = 0;          
        float ux = 0, uy = 0;          
        MouseBuf(mx, my);
        MouseUI(ux, uy);
        const auto& M = Input::Mouse();
        auto& cats = Modules::All();

        if (g_keyCap)
        {
            for (int vk = 1; vk < 256; ++vk)
            {
                if (Input::KeyPressed(vk))
                {
                    g_keyCap->key = vk;
                    g_keyCap = nullptr;
                    Log::info("[ui] keybind set to %d", vk);
                    return;
                }
            }
            if (M.pressed[1] || M.pressed[0])
                g_keyCap = nullptr;                        
            return;
        }

        if (g_dropdown)
        {
            if (M.pressed[1]) { g_dropdown = nullptr; return; }
            if (M.pressed[0])
            {
                bool consumed = false;
                if (Hit(g_dropX, g_dropY, g_dropW, g_dropItemH * static_cast<float>(g_dropCount), mx, my))
                {
                    const int i = static_cast<int>((my - g_dropY) / g_dropItemH);
                    if (i >= 0 && i < g_dropCount) g_dropdown->idx = i;
                    consumed = true;
                }
                g_dropdown = nullptr;
                if (consumed) return;
            }
        }

        if (g_slider)
        {
            if (M.down[0])
            {
                ApplySliderFromX(*g_slider, mx);
                return;
            }
            g_slider = nullptr;
        }

        if (M.pressed[0] && uy < 62.f)
        {
            const float fs = 17.f, pad = 26.f, sw = Renderer::Width();
            float totalW = 0.f;
            for (auto* n : kTabNames) totalW += Renderer::MeasureText(n, fs) + pad;
            totalW -= pad;
            float x = (sw - totalW) * 0.5f;
            for (int i = 0; i < 3; ++i)
            {
                const float w = Renderer::MeasureText(kTabNames[i], fs) + pad;
                if (Hit(x, 10.f, w, fs * 1.6f, ux, uy)) { g_tab = i; break; }
                x += w;
            }
            return;
        }

        if (M.pressed[1] && g_tab == 0)
        {
            for (auto& c : cats)
            {
                if (!c.open) continue;
                float ry = c.y + kHeaderH + 3.f;
                for (auto& m : c.mods)
                {
                    if (Hit(c.x, ry, CatW(), kModH, ux, uy))
                    {
                        if (!m.settings.empty()) m.expanded = !m.expanded;
                        return;
                    }
                    ry += ModuleH(m);
                }
            }
        }

        if (M.pressed[0] && g_tab == 0)
        {
            g_pressX = ux; g_pressY = uy;

            for (size_t ci = 0; ci < cats.size(); ++ci)
            {
                auto& c = cats[ci];

                if (Hit(c.x, c.y, CatW(), kHeaderH, ux, uy))
                {
                    g_drag = static_cast<int>(ci);
                    g_dragMoved = false;
                    g_dragOffX = ux - c.x;
                    g_dragOffY = uy - c.y;
                    return;
                }

                if (!c.open || g_tab != 0) continue;

                float ry = c.y + kHeaderH + 3.f;
                bool  handled = false;

                for (auto& m : c.mods)
                {
                    
                    if (Hit(c.x, ry, CatW(), kModH, ux, uy))
                    {
                        const float tw = 30.f, th = 15.f;
                        const float tx = c.x + ContentX() - tw;
                        const float ty = ry + (kModH - th) * 0.5f;

                        if (Hit(tx - 3.f, ty - 5.f, tw + 6.f, th + 10.f, ux, uy))
                        {
                            if (m.implemented)
                            {
                                m.enabled = !m.enabled;
                                Log::info("[ui] %ls = %s", m.name.c_str(), m.enabled ? "ON" : "off");
                            }
                            else
                            {

                                Log::warn("[ui] %ls 没有后端实现，拒绝开启（需要地址）",
                                          m.name.c_str());
                            }
                        }
                        else if (!m.settings.empty())
                        {
                            m.expanded = !m.expanded;
                        }
                        handled = true;
                        break;
                    }

                    if (m.anim > 0.5f && !m.settings.empty())
                    {
                        float sy = ry + kModH + 5.f;
                        for (auto& s : m.settings)
                        {
                            if (Hit(c.x, sy, CatW(), SettingH(s), ux, uy))
                            {
                                handled = ClickSetting(c, s, sy, ux, uy, mx);
                                break;
                            }
                            sy += SettingH(s);
                        }
                        if (handled) break;
                    }

                    ry += ModuleH(m);
                }

                if (handled) return;
            }
        }

        if (g_drag >= 0 && M.down[0])
        {
            if (fabsf(ux - g_pressX) + fabsf(uy - g_pressY) > 4.f) g_dragMoved = true;
            if (g_dragMoved && g_drag < static_cast<int>(cats.size()))
            {
                cats[g_drag].x = ux - g_dragOffX;
                cats[g_drag].y = uy - g_dragOffY;

                cats[g_drag].moved = true;
            }
        }

        if (M.released[0] && g_drag >= 0)
        {
            if (!g_dragMoved && g_drag < static_cast<int>(cats.size()))
                cats[g_drag].open = !cats[g_drag].open;
            g_drag = -1;
            g_dragMoved = false;
        }

        UpdateEditor();
    }

    void Render()
    {
        const float sw = Renderer::Width(), sh = Renderer::Height();
        const float anim = g_anim;
        if (sw < 2.f || sh < 2.f) return;

        g_xf.cx = sw * 0.5f;
        g_xf.cy = sh * 0.5f;
        g_xf.s  = 0.965f + 0.035f * anim;

        Renderer::FillRect(0, 0, sw, sh, A(Renderer::Color(0, 0, 0), 0.42f * anim));

        DrawTabs(anim);

        {
            const bool showEd = (g_tab == 1) || HudEditorOn();
            if (showEd) DrawHudEditorOverlay(anim, g_tab == 1);
        }

        if (g_tab == 2)
        {
            Renderer::DrawText(L"Scripting  ·  not implemented yet",
                               sw * 0.5f, sh * 0.5f - 20.f, 24.f,
                               A(Renderer::Color(255, 120, 120), anim), TextAlign::Center);
        }
        else if (g_tab == 1)
        {
            
        }
        else
        {
            for (auto& c : Modules::All()) DrawColumn(c);
            DrawDropdownLayer();          

            const float cx = sw * 0.5f;

            Renderer::DrawText(L"NewDLL  v0.2", cx, sh - 42.f, 14.f,
                               A(Renderer::Color(200, 200, 210), 0.75f * anim), TextAlign::Center);

            {
                int impl = 0, total = 0;
                for (auto& c : Modules::All())
                    for (auto& m : c.mods) { ++total; if (m.implemented) ++impl; }

                wchar_t buf[128];
                swprintf_s(buf, L"backends: %d / %d available      N/A = 未实现（需要逆向地址）",
                           impl, total);
                Renderer::DrawText(buf, cx, sh - 78.f, 12.f,
                                   A(Renderer::Color(150, 150, 162), 0.8f * anim), TextAlign::Center);
            }

            if (Game::ScaffoldOn())
            {
                if (Game::HooksOk())
                    Renderer::DrawText(L"scaffold: hook installed", cx, sh - 60.f, 12.f,
                                       A(Renderer::Color(80, 220, 130), 0.85f * anim), TextAlign::Center);
                else
                    Renderer::DrawText(L"scaffold: HOOK FAILED (byte mismatch)", cx, sh - 60.f, 12.f,
                                       A(Renderer::Color(255, 90, 90), 0.95f * anim), TextAlign::Center);
            }
            else if (Game::KillAuraOn())
            {

                if (Game::TlsHookOk())
                    Renderer::DrawText(L"killaura: tls hook installed", cx, sh - 60.f, 12.f,
                                       A(Renderer::Color(80, 220, 130), 0.85f * anim), TextAlign::Center);
                else
                    Renderer::DrawText(L"killaura: HOOK FAILED (byte mismatch)", cx, sh - 60.f, 12.f,
                                       A(Renderer::Color(255, 90, 90), 0.95f * anim), TextAlign::Center);
            }

            if (Game::Player())
                Renderer::DrawText(L"player linked", cx, sh - 24.f, 12.f,
                                   A(Renderer::Color(80, 220, 130), 0.75f * anim), TextAlign::Center);
            else if (Game::WantsPlayer())
                Renderer::DrawText(L"player: scanning...", cx, sh - 24.f, 12.f,
                                   A(Renderer::Color(190, 170, 90), 0.85f * anim), TextAlign::Center);
            else
                Renderer::DrawText(L"player: idle (no feature needs it)", cx, sh - 24.f, 12.f,
                                   A(Renderer::Color(120, 120, 132), 0.7f * anim), TextAlign::Center);
        }

        DrawCursor();   
    }
}
