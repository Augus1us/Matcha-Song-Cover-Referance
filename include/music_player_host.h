#pragma once

#include <Windows.h>
#include "imgui.h"

namespace music_host {

ImVec2 Measure(ImFont* font, float size, const char* text);
void DrawText(ImDrawList* drawList, ImFont* font, float size,
              ImVec2 position, ImU32 color, const char* text);
void DrawShadow(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding,
                int layers = 12, float spread = 14.0f, float strength = 0.42f);

namespace animation {
float Anim(ImGuiID id, bool enabled, float speed = 16.0f);
float SpringF(ImGuiID id, float target, float speed = 18.0f,
              float damping = 1.0f);
void SetSpring(ImGuiID id, float value);
float ClickBounce(ImGuiID id, bool triggered);
}

namespace overlay {
HWND GetOverlayWindow();
void* GetD3DDevice();
ImFont* GetFont(int index);
ImFont* GetMusicRegularFont();
ImFont* GetMusicBoldFont();
}

} // namespace music_host
