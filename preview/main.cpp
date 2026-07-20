#include "preview_host.h"

#include "media.h"
#include "music_player_host.h"
#include "music_player_ui.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <Windows.h>
#include <dwmapi.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace {

LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                 WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return true;

    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            preview::ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void DrawBackdrop() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 min = viewport->Pos;
    const ImVec2 max(min.x + viewport->Size.x, min.y + viewport->Size.y);
    drawList->AddRectFilledMultiColor(
        min, max,
        IM_COL32(11, 13, 19, 255), IM_COL32(15, 20, 29, 255),
        IM_COL32(7, 9, 14, 255), IM_COL32(12, 14, 21, 255));

    drawList->AddCircleFilled(
        ImVec2(min.x + viewport->Size.x * 0.18f,
               min.y + viewport->Size.y * 0.24f),
        viewport->Size.y * 0.34f, IM_COL32(24, 58, 76, 36), 96);
    drawList->AddCircleFilled(
        ImVec2(min.x + viewport->Size.x * 0.82f,
               min.y + viewport->Size.y * 0.74f),
        viewport->Size.y * 0.38f, IM_COL32(52, 29, 73, 30), 96);

    ImFont* font = music_host::overlay::GetMusicRegularFont();
    drawList->AddText(font, 13.0f, ImVec2(min.x + 20.0f, min.y + 18.0f),
                      IM_COL32(255, 255, 255, 105),
                      "Native Music Player Preview");
    drawList->AddText(font, 11.0f, ImVec2(min.x + 20.0f, min.y + 39.0f),
                      IM_COL32(255, 255, 255, 55),
                      "Start playback in Spotify or another Windows media app");
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    WNDCLASSEXW windowClass{
        sizeof(WNDCLASSEXW), CS_CLASSDC, WindowProcedure, 0, 0,
        instance, nullptr, nullptr, nullptr, nullptr,
        L"NativeMusicPlayerPreview", nullptr
    };
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowW(
        windowClass.lpszClassName, L"Native Music Player Preview",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 700,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    preview::SetWindow(window);
    if (!preview::CreateDevice(window)) {
        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        return 2;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
    style.WindowBorderSize = 0.0f;
    preview::InitializeFonts();

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(preview::Device(), preview::Context());

    native_music_player::g_playerOptions.visible = true;
    native_music_player::g_playerOptions.showLyrics = true;
    native_music_player::g_playerOptions.x = 72.0f;
    native_music_player::g_playerOptions.y = 105.0f;
    media::Init();

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;

        preview::BeginFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawBackdrop();
        native_music_player::DrawMusicPlayer();
        ImGui::Render();
        preview::EndFrame();
    }

    native_music_player::ShutdownMusicPlayer();
    media::Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    preview::DestroyDevice();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    return 0;
}
