#pragma once

#include <string>

namespace Hud
{
    
    void Tick();

    bool AnyEnabled();

    void Render();

    struct Item
    {
        int          id    = 0;      
        std::wstring name;           
        float        x = 0, y = 0;   
        float        w = 0, h = 0;
        bool         active = false; 
    };

    int  Items(Item* out, int maxItems);

    void SetItemPos(int id, float left, float top);

    void ResetPositions();
}
