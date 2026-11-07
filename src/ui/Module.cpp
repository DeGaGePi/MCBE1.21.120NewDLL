#include "ui/Module.hpp"
#include "game/Game.hpp"
#include "input/Input.hpp"

namespace
{
    
    Setting SB(const wchar_t* n, bool v)
    {
        Setting s; s.name = n; s.type = SettingType::Bool; s.b = v; return s;
    }
    Setting SF(const wchar_t* n, float v, float mn, float mx, float step)
    {
        Setting s; s.name = n; s.type = SettingType::Float;
        s.f = v; s.fmin = mn; s.fmax = mx; s.fstep = step; return s;
    }
    Setting SI(const wchar_t* n, int v, int mn, int mx)
    {
        Setting s; s.name = n; s.type = SettingType::Int;
        s.i = v; s.imin = mn; s.imax = mx; return s;
    }
    Setting SE(const wchar_t* n, std::initializer_list<const wchar_t*> opts, int idx)
    {
        Setting s; s.name = n; s.type = SettingType::Enum; s.idx = idx;
        for (auto* o : opts) s.options.emplace_back(o);
        return s;
    }
    Setting SK(const wchar_t* n, int vk)
    {
        Setting s; s.name = n; s.type = SettingType::Key; s.key = vk; return s;
    }

    Module Mod(const wchar_t* n)
    {
        Module m; m.name = n; return m;
    }

    std::vector<Category> g_cats;

    void Build()
    {

        {
            Category c; c.name = L"HUD";

            auto wm = Mod(L"Watermark");
            wm.implemented = true;
            wm.feat = Feature::HudWatermark;
            wm.enabled = true;
            wm.settings = { SB(L"Show Version", true) };
            c.mods.push_back(wm);

            auto al = Mod(L"ArrayList");
            al.implemented = true;
            al.feat = Feature::HudArrayList;
            al.enabled = true;
            al.settings = { SF(L"Scale", 1.0f, 0.6f, 1.6f, 0.05f) };
            c.mods.push_back(al);

            auto fps = Mod(L"FPS");
            fps.implemented = true;
            fps.feat = Feature::HudFps;
            fps.enabled = true;
            c.mods.push_back(fps);

            auto cps = Mod(L"CPS");
            cps.implemented = true;
            cps.feat = Feature::HudCps;
            cps.settings = { SE(L"Watch", { L"Both", L"Left Only" }, 0) };
            c.mods.push_back(cps);

            auto ks = Mod(L"Keystrokes");
            ks.implemented = true;
            ks.feat = Feature::HudKeystrokes;
            ks.settings = { SF(L"Scale", 1.0f, 0.6f, 1.6f, 0.05f) };
            c.mods.push_back(ks);

            auto co = Mod(L"Coordinates");
            co.implemented = true;
            co.feat = Feature::HudCoords;
            co.settings = { SB(L"Show Chunk", true) };
            c.mods.push_back(co);

            auto ed = Mod(L"HudEditor");
            ed.implemented = true;
            ed.feat = Feature::HudEditor;
            ed.settings = { SB(L"Show Labels", true) };
            c.mods.push_back(ed);

            g_cats.push_back(std::move(c));
        }

        {
            Category c; c.name = L"Combat";

            auto ka = Mod(L"Killaura");
            ka.implemented = true;
            ka.feat = Feature::KillAura;
            ka.settings = {
                
                SE(L"Targets", { L"Players Only", L"Mobs Only", L"All" }, 2),
                SF(L"Range", 4.5f, 2.f, 8.f, 0.1f),
                
                SE(L"Mode", { L"Single", L"Switch", L"Multi" }, 0),
                
                SI(L"Delay (ms)", 250, 30, 1000),
                
                SB(L"Through Walls", false),
                SK(L"Keybind", 0),
            };
            c.mods.push_back(ka);

            auto reach = Mod(L"Reach");
            reach.settings = { SF(L"Distance", 3.0f, 3.f, 8.f, 0.1f) };
            c.mods.push_back(reach);

            auto vel = Mod(L"Velocity");
            vel.settings = {
                SF(L"Horizontal", 0.f, 0.f, 100.f, 1.f),
                SF(L"Vertical", 0.f, 0.f, 100.f, 1.f),
            };
            c.mods.push_back(vel);

            auto ac = Mod(L"AutoClicker");
            ac.settings = { SI(L"CPS", 12, 1, 20), SB(L"Only Weapon", false) };
            c.mods.push_back(ac);

            c.mods.push_back(Mod(L"Criticals"));

            auto ak = Mod(L"AntiKnockback");
            ak.settings = { SB(L"Reduce Only", true) };
            c.mods.push_back(ak);

            g_cats.push_back(std::move(c));
        }

        {
            Category c; c.name = L"Visual";

            auto esp = Mod(L"ESP");
            esp.settings = {
                SE(L"Mode", { L"Box", L"Corner", L"2D Flat", L"3D" }, 0),
                SB(L"Outline", true),
                SF(L"Scale", 1.0f, 0.5f, 2.0f, 0.05f),
            };
            c.mods.push_back(esp);

            auto tr = Mod(L"Tracers");
            tr.settings = {
                SF(L"Thickness", 1.5f, 1.f, 4.f, 0.1f),
                SE(L"Origin", { L"Head", L"Feet", L"Center" }, 1),
            };
            c.mods.push_back(tr);

            auto nt = Mod(L"Nametags");
            nt.settings = { SF(L"Scale", 1.0f, 0.5f, 2.f, 0.05f), SB(L"Show Health", true) };
            c.mods.push_back(nt);

            auto fb = Mod(L"Fullbright");
            fb.settings = { SF(L"Gamma", 8.0f, 1.f, 16.f, 0.5f) };
            c.mods.push_back(fb);

            auto ncs = Mod(L"NoCameraShake");
            ncs.implemented = true;
            ncs.feat = Feature::NoCameraShake;
            c.mods.push_back(ncs);

            auto ntaa = Mod(L"NoTAA / ClearView");
            ntaa.implemented = true;
            ntaa.feat = Feature::NoTaa;
            ntaa.settings = { SB(L"Keep On", true) };
            c.mods.push_back(ntaa);

            g_cats.push_back(std::move(c));
        }

        {
            Category c; c.name = L"Movement";

            auto sp = Mod(L"Sprint");
            sp.settings = { SB(L"Omni", false) };
            c.mods.push_back(sp);

            c.mods.push_back(Mod(L"NoSlow"));

            auto speed = Mod(L"Speed");
            speed.settings = { SF(L"Multiplier", 1.3f, 1.f, 3.f, 0.05f), SB(L"Bhop", false) };
            c.mods.push_back(speed);

            auto fly = Mod(L"Fly");
            fly.settings = {
                SE(L"Mode", { L"Vanilla", L"Glide", L"Packet" }, 0),
                SF(L"Speed", 1.0f, 0.5f, 5.f, 0.1f),
            };
            c.mods.push_back(fly);

            auto step = Mod(L"Step");
            step.settings = { SF(L"Height", 1.0f, 0.5f, 2.5f, 0.1f) };
            c.mods.push_back(step);

            c.mods.push_back(Mod(L"Jesus"));

            g_cats.push_back(std::move(c));
        }

        {
            Category c; c.name = L"Player";

            auto sc = Mod(L"Scaffold");
            sc.implemented = true;
            sc.feat = Feature::Scaffold;
            sc.settings = {

                SE(L"Method", { L"Redirect (write pos)", L"Native (extra place)" }, 0),
                SI(L"Y Offset", -1, -3, 0),

                SI(L"Delay (ms)", 60, 10, 1000),

                SB(L"Dry Run (log only)", false),
                SK(L"Keybind", 0),
            };
            c.mods.push_back(sc);

            auto at = Mod(L"AutoTotem");
            at.settings = { SF(L"Health", 10.f, 1.f, 20.f, 0.5f) };
            c.mods.push_back(at);

            auto cs = Mod(L"ChestStealer");
            cs.settings = { SI(L"Delay", 100, 0, 500) };
            c.mods.push_back(cs);

            auto fp = Mod(L"FastPlace");
            fp.settings = { SI(L"Delay", 1, 0, 10) };
            c.mods.push_back(fp);

            auto nf = Mod(L"NoFall");
            nf.settings = { SE(L"Mode", { L"Packet", L"NoGround" }, 0) };
            c.mods.push_back(nf);

            c.mods.push_back(Mod(L"Blink"));

            g_cats.push_back(std::move(c));
        }

        {
            Category c; c.name = L"Misc";

            auto ag = Mod(L"AutoGG");
            ag.settings = { SE(L"Message", { L"gg", L"GG", L"Good Game" }, 0) };
            c.mods.push_back(ag);

            auto suf = Mod(L"ChatSuffix");
            suf.settings = { SE(L"Suffix", { L"NewDLL", L"| nDLL", L"on top" }, 0) };
            c.mods.push_back(suf);

            auto spam = Mod(L"Spammer");
            spam.settings = { SI(L"Delay", 1000, 100, 5000) };
            c.mods.push_back(spam);

            c.mods.push_back(Mod(L"Disabler"));
            c.mods.push_back(Mod(L"AntiVoid"));

            auto tmr = Mod(L"Timer");
            tmr.settings = { SF(L"Speed", 1.0f, 0.1f, 5.f, 0.1f) };
            c.mods.push_back(tmr);

            g_cats.push_back(std::move(c));
        }

        {
            static const wchar_t* kOrder[] = {
                L"Combat", L"HUD", L"Visual", L"Movement", L"Player", L"Misc"
            };
            std::vector<Category> out;
            out.reserve(g_cats.size());

            for (const wchar_t* nm : kOrder)
                for (auto& c : g_cats)
                    if (c.name == nm) { out.push_back(std::move(c)); c.name.clear(); }

            for (auto& c : g_cats)            
                if (!c.name.empty()) out.push_back(std::move(c));

            g_cats.swap(out);
        }
    }
}

namespace Modules
{
    std::vector<Category>& All()
    {
        return g_cats;
    }

    void Init()
    {
        if (!g_cats.empty()) return;
        Build();
    }

    void Sync()
    {
        for (auto& c : g_cats)
        {
            for (auto& m : c.mods)
            {

                for (auto& s : m.settings)
                {
                    if (s.type == SettingType::Key && s.key > 0 && Input::KeyPressed(s.key))
                        m.enabled = !m.enabled;
                }

                switch (m.feat)
                {
                case Feature::Scaffold:
                    Game::SetScaffold(m.enabled);
                    for (const auto& s : m.settings)
                    {
                        if (s.type == SettingType::Int && s.name == L"Y Offset")
                            Game::SetScaffoldYOffset(s.i);
                        else if (s.type == SettingType::Int && s.name == L"Delay (ms)")
                            Game::SetScaffoldDelay(s.i);
                        else if (s.type == SettingType::Bool && s.name == L"Dry Run (log only)")
                            Game::SetScaffoldDryRun(s.b);
                        else if (s.type == SettingType::Enum && s.name == L"Method")
                            Game::SetScaffoldRedirect(s.idx == 0);
                    }
                    break;
                case Feature::NoCameraShake: Game::SetNoCameraShake(m.enabled); break;
                case Feature::NoTaa:         Game::SetNoTaa(m.enabled);         break;

                case Feature::KillAura:
                    Game::SetKillAura(m.enabled);
                    for (const auto& s : m.settings)
                    {
                        if (s.type == SettingType::Float && s.name == L"Range")
                            Game::SetKillAuraRange(s.f);
                        else if (s.type == SettingType::Int && s.name == L"Delay (ms)")
                            Game::SetKillAuraDelay(s.i);
                        else if (s.type == SettingType::Bool && s.name == L"Through Walls")
                            Game::SetKillAuraThroughWalls(s.b);
                        else if (s.type == SettingType::Enum && s.name == L"Targets")
                            Game::SetKillAuraTargets(s.idx);
                        else if (s.type == SettingType::Enum && s.name == L"Mode")
                            Game::SetKillAuraMode(s.idx);
                    }
                    break;

                case Feature::HudCoords:     Game::SetWantPlayer(m.enabled);    break;

                default: break;   
                }
            }
        }
    }
}
