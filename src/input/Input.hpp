#pragma once

#include <Windows.h>

namespace Input
{
    struct MouseState
    {
        float x = 0.f, y = 0.f;      
        float dx = 0.f, dy = 0.f;
        bool  down[3]    = {};
        bool  pressed[3] = {};
        bool  released[3] = {};
        float wheel = 0.f;
        bool  valid = false;
    };

    void Install();
    void Poll();                     

    const MouseState& Mouse();
    bool KeyDown(int vk);
    bool KeyPressed(int vk);

    void SetMenuOpen(bool open);
    bool MenuOpen();

    void ClientSize(float& w, float& h);
}
