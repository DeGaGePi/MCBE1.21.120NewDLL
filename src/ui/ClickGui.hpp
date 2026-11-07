#pragma once

namespace ClickGui
{
    void Toggle();
    bool IsOpen();
    bool Visible();       
    void Update();        
    void Render();        
    void RenderEditor();  

    bool EditorActive();

    bool EditorFreeRunning();

    void Relayout();
}
