#pragma once

#include <string>
#include <vector>

enum class SettingType
{
    Bool,     
    Float,    
    Int,      
    Enum,     
    Key       
};

struct Setting
{
    std::wstring name;
    SettingType  type = SettingType::Bool;

    bool b = false;

    float f = 0.f, fmin = 0.f, fmax = 1.f, fstep = 0.01f;

    int i = 0, imin = 0, imax = 100;

    std::vector<std::wstring> options;
    int idx = 0;

    int key = 0;                        

    bool dragging  = false;             
    bool capturing = false;             
};

enum class Feature
{
    None,

    Scaffold,
    KillAura,
    NoCameraShake,
    NoTaa,

    HudWatermark,
    HudArrayList,
    HudFps,
    HudCps,
    HudKeystrokes,
    HudCoords,

    HudEditor
};

struct Module
{
    std::wstring name;
    bool  enabled     = false;
    bool  implemented = false;          
    Feature feat      = Feature::None;

    bool  expanded = false;
    float anim     = 0.f;               

    std::vector<Setting> settings;
};

struct Category
{
    std::wstring name;
    std::vector<Module> mods;
    float x = 0.f, y = 0.f;
    bool  open = true;

    bool  moved = false;
};

namespace Modules
{
    std::vector<Category>& All();
    void Init();
    void Sync();                        
}
