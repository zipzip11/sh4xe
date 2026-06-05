#include "hooks/dinput8_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "render/d3d8_console.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cstdint>
#include <mutex>

namespace sh4xe::hooks::dinput8
{
namespace
{

// COM calling convention for DirectInput interface methods is __stdcall.
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, const IID&, void**, void*);
using CreateDeviceFn = HRESULT(__stdcall*)(void*, const GUID&, void**, void*);
using GetDeviceStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
using GetDeviceDataFn = HRESULT(__stdcall*)(void*, DWORD, void*, DWORD*, DWORD);
using ReleaseFn = ULONG(__stdcall*)(void*);

constexpr DWORD kDirectInput8Version = 0x0800;

// IDirectInput8 / IDirectInputDevice8 vtable indices.
constexpr size_t kVtblCreateDevice = 3;
constexpr size_t kVtblGetDeviceState = 9;
constexpr size_t kVtblGetDeviceData = 10;

// {BF798030-483A-4DA2-AA99-5D64ED369700} IID_IDirectInput8A
constexpr IID kIID_IDirectInput8A = {
    0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
// {6F1D2B61-D5A0-11CF-BFC7-444553540000} GUID_SysKeyboard
constexpr GUID kGUID_SysKeyboard = {
    0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct DeviceObjectDataPrefix
{
    DWORD ofs;
    DWORD data;
};

constexpr DWORD kDikQ = 0x10;
constexpr DWORD kDikE = 0x12;
constexpr DWORD kDikA = 0x1E;
constexpr DWORD kDikD = 0x20;
constexpr BYTE kDikDown = 0x80;

DirectInput8CreateFn g_originalDirectInput8Create = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
GetDeviceStateFn g_originalGetDeviceState = nullptr;
GetDeviceDataFn g_originalGetDeviceData = nullptr;

std::atomic<void*> g_keyboardDevice{nullptr};
std::mutex g_hookMutex;
bool g_createDeviceHooked = false;
bool g_deviceHooked = false;

bool GuidEquals(const GUID& a, const GUID& b)
{
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

bool LooksLikeKeyboardState(void* device, DWORD cbData)
{
    return device == g_keyboardDevice.load(std::memory_order_relaxed) || cbData == 256;
}

DWORD RemapMoveKey(DWORD key)
{
    if (key == kDikA)
        return kDikQ;
    if (key == kDikD)
        return kDikE;
    return key;
}

void RemapKeyboardState(void* data, DWORD cbData)
{
    if (!data || cbData <= kDikD)
        return;

    auto* keys = static_cast<BYTE*>(data);
    keys[kDikQ] = static_cast<BYTE>(keys[kDikQ] | (keys[kDikA] & kDikDown));
    keys[kDikE] = static_cast<BYTE>(keys[kDikE] | (keys[kDikD] & kDikDown));
    keys[kDikA] = 0;
    keys[kDikD] = 0;
}

void RemapKeyboardDataEvents(void* rgdod, DWORD cbObjectData, DWORD count)
{
    if (!rgdod || cbObjectData < sizeof(DeviceObjectDataPrefix))
        return;

    auto* bytes = static_cast<BYTE*>(rgdod);
    for (DWORD i = 0; i < count; ++i)
    {
        auto* event = reinterpret_cast<DeviceObjectDataPrefix*>(bytes + static_cast<size_t>(i) * cbObjectData);
        event->ofs = RemapMoveKey(event->ofs);
    }
}

HRESULT __stdcall GetDeviceStateDetour(void* device, DWORD cbData, void* lpvData)
{
    const HRESULT hr = g_originalGetDeviceState(device, cbData, lpvData);
    const bool consoleOpen = render::d3d8_console::IsOpen();

    if (!consoleOpen && SUCCEEDED(hr) && lpvData && LooksLikeKeyboardState(device, cbData))
        RemapKeyboardState(lpvData, cbData);

    if (consoleOpen && lpvData && cbData)
        std::memset(lpvData, 0, cbData); // zeroed state == no keys / centred axes
    return hr;
}

HRESULT __stdcall GetDeviceDataDetour(void* device, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD flags)
{
    const HRESULT hr = g_originalGetDeviceData(device, cbObjectData, rgdod, pdwInOut, flags);
    const bool consoleOpen = render::d3d8_console::IsOpen();

    if (!consoleOpen && SUCCEEDED(hr) && rgdod && pdwInOut &&
        device == g_keyboardDevice.load(std::memory_order_relaxed))
    {
        RemapKeyboardDataEvents(rgdod, cbObjectData, *pdwInOut);
    }

    if (consoleOpen && pdwInOut)
        *pdwInOut = 0; // report no buffered events
    return hr;
}

void HookDevice(void* device)
{
    if (!device)
        return;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_deviceHooked)
        return;

    void** vtbl = *reinterpret_cast<void***>(device);

    const bool state = CreateAndEnableHook("IDirectInputDevice8::GetDeviceState",
                                           vtbl[kVtblGetDeviceState],
                                           reinterpret_cast<void*>(&GetDeviceStateDetour),
                                           &g_originalGetDeviceState);
    const bool data = CreateAndEnableHook("IDirectInputDevice8::GetDeviceData",
                                          vtbl[kVtblGetDeviceData],
                                          reinterpret_cast<void*>(&GetDeviceDataDetour),
                                          &g_originalGetDeviceData);

    if (state && data)
    {
        g_deviceHooked = true;
        sh4xe::Log("IDirectInputDevice8 GetDeviceState/GetDeviceData hooked");
        render::d3d8_console::AddLog("ok dinput device input gating active");
    }
}

HRESULT __stdcall CreateDeviceDetour(void* self, const GUID& rguid, void** device, void* outer)
{
    const HRESULT hr = g_originalCreateDevice(self, rguid, device, outer);
    if (SUCCEEDED(hr) && device && *device)
    {
        if (GuidEquals(rguid, kGUID_SysKeyboard))
        {
            g_keyboardDevice.store(*device, std::memory_order_relaxed);
            sh4xe::Log("DirectInput keyboard device tracked for modern strafe remap (A/D -> Q/E)");
        }
        HookDevice(*device);
    }
    return hr;
}

void HookDirectInput8Interface(void* directInput)
{
    if (!directInput)
        return;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_createDeviceHooked)
        return;

    void** vtbl = *reinterpret_cast<void***>(directInput);
    if (CreateAndEnableHook("IDirectInput8::CreateDevice",
                            vtbl[kVtblCreateDevice],
                            reinterpret_cast<void*>(&CreateDeviceDetour),
                            &g_originalCreateDevice))
    {
        g_createDeviceHooked = true;
        sh4xe::Log("IDirectInput8::CreateDevice hooked");
    }
}

HRESULT WINAPI DirectInput8CreateDetour(HINSTANCE inst, DWORD version, const IID& riid, void** out, void* outer)
{
    const HRESULT hr = g_originalDirectInput8Create(inst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out)
        HookDirectInput8Interface(*out);
    return hr;
}

} // namespace

bool InstallInputHook()
{
    HMODULE dinput8 = LoadLibraryW(L"dinput8.dll");
    if (!dinput8)
    {
        sh4xe::Log("dinput8.dll could not be loaded; input gating disabled");
        return false;
    }

    void* create = reinterpret_cast<void*>(GetProcAddress(dinput8, "DirectInput8Create"));
    if (!create)
    {
        sh4xe::Log("DirectInput8Create export missing; input gating disabled");
        return false;
    }

    if (!CreateAndEnableHook("DirectInput8Create",
                             create,
                             reinterpret_cast<void*>(&DirectInput8CreateDetour),
                             &g_originalDirectInput8Create))
        return false;

    sh4xe::Log("DirectInput8Create hooked @ %p", create);

    // Build a throwaway interface + keyboard device so the shared vtables get
    // hooked now, regardless of when the game creates its own devices.
    void* probe = nullptr;
    const HRESULT hr = g_originalDirectInput8Create(
        GetModuleHandleW(nullptr), kDirectInput8Version, kIID_IDirectInput8A, &probe, nullptr);
    if (SUCCEEDED(hr) && probe)
    {
        HookDirectInput8Interface(probe);

        // Calling the (now hooked) CreateDevice installs the device-level hooks.
        void** diVtbl = *reinterpret_cast<void***>(probe);
        void* device = nullptr;
        const auto createDevice = reinterpret_cast<CreateDeviceFn>(diVtbl[kVtblCreateDevice]);
        if (SUCCEEDED(createDevice(probe, kGUID_SysKeyboard, &device, nullptr)) && device)
        {
            void** devVtbl = *reinterpret_cast<void***>(device);
            reinterpret_cast<ReleaseFn>(devVtbl[2])(device);
        }

        reinterpret_cast<ReleaseFn>(diVtbl[2])(probe);
    }
    else
    {
        sh4xe::Log("dinput probe DirectInput8Create failed hr=0x%08lX", static_cast<unsigned long>(hr));
    }

    return true;
}

} // namespace sh4xe::hooks::dinput8
