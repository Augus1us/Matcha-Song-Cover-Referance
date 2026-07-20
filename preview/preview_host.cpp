#include "preview_host.h"

#include "music_player_host.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <unordered_map>

namespace {

HWND g_window = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void DestroyRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

struct SpringState {
    float value = 0.0f;
    float velocity = 0.0f;
};

std::unordered_map<ImGuiID, float> g_animations;
std::unordered_map<ImGuiID, SpringState> g_springs;

} // namespace

namespace preview {

bool CreateDevice(HWND window) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selectedLevel{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &description, &g_swapChain, &g_device, &selectedLevel, &g_context);
    if (FAILED(result)) return false;

    CreateRenderTarget();
    return g_renderTarget != nullptr;
}

void DestroyDevice() {
    DestroyRenderTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

void ResizeSwapChain(UINT width, UINT height) {
    if (!g_swapChain || width == 0 || height == 0) return;
    DestroyRenderTarget();
    g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void BeginFrame() {
    ImGui_ImplDX11_NewFrame();
}

void EndFrame() {
    constexpr float clearColor[4] = { 0.035f, 0.039f, 0.050f, 1.0f };
    g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
    g_context->ClearRenderTargetView(g_renderTarget, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(1, 0);
}

void InitializeFonts() {
    ImGuiIO& io = ImGui::GetIO();
    g_regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    g_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 18.0f);
    if (!g_regular) g_regular = io.Fonts->AddFontDefault();
    if (!g_bold) g_bold = g_regular;
}

void SetWindow(HWND window) {
    g_window = window;
}

ID3D11Device* Device() { return g_device; }
ID3D11DeviceContext* Context() { return g_context; }

} // namespace preview

namespace music_host {

ImVec2 Measure(ImFont* font, float size, const char* text) {
    ImFont* selected = font ? font : ImGui::GetFont();
    return selected->CalcTextSizeA(size, FLT_MAX, 0.0f, text ? text : "");
}

void DrawText(ImDrawList* drawList, ImFont* font, float size,
              ImVec2 position, ImU32 color, const char* text) {
    drawList->AddText(font ? font : ImGui::GetFont(), size, position,
                      color, text ? text : "");
}

void DrawShadow(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding,
                int layers, float spread, float strength) {
    for (int index = layers; index >= 1; --index) {
        const float amount = static_cast<float>(index) / static_cast<float>(layers);
        const float grow = spread * amount;
        const int alpha = static_cast<int>(strength * 255.0f *
            (1.0f - amount) * (1.0f - amount) / layers * 3.0f);
        if (alpha <= 0) continue;
        drawList->AddRectFilled(
            ImVec2(min.x - grow, min.y - grow + 5.0f),
            ImVec2(max.x + grow, max.y + grow + 5.0f),
            IM_COL32(0, 0, 0, alpha), rounding + grow);
    }
}

namespace animation {

float Anim(ImGuiID id, bool enabled, float speed) {
    float delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    if (delta <= 0.0f) delta = 1.0f / 60.0f;
    const float target = enabled ? 1.0f : 0.0f;
    auto [entry, inserted] = g_animations.try_emplace(id, target);
    entry->second += (target - entry->second) * (1.0f - std::exp(-speed * delta));
    return entry->second;
}

float SpringF(ImGuiID id, float target, float speed, float damping) {
    float delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f);
    if (delta <= 0.0f) delta = 1.0f / 60.0f;
    speed = std::max(speed, 0.01f);
    damping = std::max(damping, 0.0f);

    auto [entry, inserted] = g_springs.try_emplace(id, SpringState{ target, 0.0f });
    SpringState& state = entry->second;
    if (inserted) return state.value;

    const float previous = state.value;
    const float omegaSquared = speed * speed;
    const float dampingTerm = 1.0f + 2.0f * delta * damping * speed;
    const float targetTerm = delta * delta * omegaSquared;
    const float inverse = 1.0f / (dampingTerm + targetTerm);
    state.value = (dampingTerm * previous + delta * state.velocity +
                   targetTerm * target) * inverse;
    state.velocity = (state.velocity + delta * omegaSquared *
                      (target - previous)) * inverse;
    if (std::abs(target - state.value) < 0.0005f &&
        std::abs(state.velocity) < 0.0005f) {
        state = { target, 0.0f };
    }
    return state.value;
}

void SetSpring(ImGuiID id, float value) {
    g_springs[id] = { value, 0.0f };
}

float ClickBounce(ImGuiID id, bool triggered) {
    static std::unordered_map<ImGuiID, float> elapsed;
    auto [entry, inserted] = elapsed.try_emplace(id, -1.0f);
    if (triggered) entry->second = 0.0f;
    if (entry->second < 0.0f) return 1.0f;

    float delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    if (delta <= 0.0f) delta = 1.0f / 60.0f;
    entry->second += delta;
    constexpr float duration = 0.28f;
    if (entry->second >= duration) {
        entry->second = -1.0f;
        return 1.0f;
    }
    const float progress = entry->second / duration;
    const float amplitude = 0.16f * std::exp(-5.0f * progress);
    return 1.0f - amplitude * std::cos(9.0f * progress);
}

} // namespace animation

namespace overlay {

HWND GetOverlayWindow() { return g_window; }
void* GetD3DDevice() { return preview::Device(); }
ImFont* GetFont(int index) {
    if (index == 0) return g_regular;
    if (index == 1) return g_bold;
    return nullptr;
}
ImFont* GetMusicRegularFont() { return g_regular; }
ImFont* GetMusicBoldFont() { return g_bold; }

} // namespace overlay
} // namespace music_host
