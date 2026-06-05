#pragma once

#include <windows.h>

namespace sh4xe::render::display
{

enum class Mode
{
    Fullscreen,
    Windowed,
    Borderless,
};

// D3DPRESENT_PARAMETERS from D3D8. Kept local so the proxy does not need to link
// against or depend on the DirectX SDK headers.
struct PresentParameters
{
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

using ResetFn = HRESULT(__stdcall*)(void*, PresentParameters*);

const char* ModeName(Mode mode);

void CaptureCreateDevice(HWND focusWindow, const PresentParameters* params);
void CaptureReset(const PresentParameters* params);
void CaptureDeviceFallback(void* device);

bool RequestMode(Mode mode, char* out, size_t outSize);
bool RequestResolution(UINT width, UINT height, char* out, size_t outSize);
bool RequestModeAndResolution(Mode mode, UINT width, UINT height, char* out, size_t outSize);
void Describe(char* out, size_t outSize);

void ApplyPending(void* device, ResetFn reset);

} // namespace sh4xe::render::display
