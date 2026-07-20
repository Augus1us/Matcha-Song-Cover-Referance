#pragma once

#include <Windows.h>
#include <d3d11.h>

namespace preview {

bool CreateDevice(HWND window);
void DestroyDevice();
void ResizeSwapChain(UINT width, UINT height);
void BeginFrame();
void EndFrame();
void InitializeFonts();
void SetWindow(HWND window);

ID3D11Device* Device();
ID3D11DeviceContext* Context();

} // namespace preview
