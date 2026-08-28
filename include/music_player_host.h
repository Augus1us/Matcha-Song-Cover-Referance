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
// Radial click ripple. Returns progress 0..1 over the ripple's lifetime after a
// click, or -1 while idle. Draw an expanding, fading ring/disc from it.
float ClickGlow(ImGuiID id, bool triggered);
// Eased press depression 0..1 (1 at the moment of press, decaying) for a subtle
// "push in" on click, separate from the springy ClickBounce.
float PressPulse(ImGuiID id, bool triggered);
}

namespace overlay {
HWND GetOverlayWindow();
void* GetD3DDevice();
// Toggle the host window between windowed and true (borderless) fullscreen --
// the whole window fills the monitor with no title bar. The player's fullscreen
// LAYOUT (the wide art+lyrics card) is a separate thing; this is the OS window
// itself. A host with no ownable window (e.g. a shared overlay) may no-op.
void ToggleFullscreenWindow();
bool IsFullscreenWindow();
ImFont* GetFont(int index);
ImFont* GetMusicRegularFont();
ImFont* GetMusicBoldFont();
}

} // namespace music_host
