#include <windows.h>

#include <mutex>

namespace
{

HMODULE g_realDsound = nullptr;
std::once_flag g_loadOnce;

void LoadRealDsound()
{
    wchar_t systemDir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return;

    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%ls\\dsound.dll", systemDir);
    g_realDsound = LoadLibraryW(path);
}

FARPROC RealProc(const char* name)
{
    std::call_once(g_loadOnce, LoadRealDsound);
    return g_realDsound ? GetProcAddress(g_realDsound, name) : nullptr;
}

HRESULT MissingExport()
{
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

} // namespace

extern "C" HRESULT WINAPI Proxy_DirectSoundCreate(LPCGUID deviceGuid, void** directSound, void* outer)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, void**, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundCreate")))
        return fn(deviceGuid, directSound, outer);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundEnumerateA(void* callback, void* context)
{
    using Fn = HRESULT(WINAPI*)(void*, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundEnumerateA")))
        return fn(callback, context);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundEnumerateW(void* callback, void* context)
{
    using Fn = HRESULT(WINAPI*)(void*, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundEnumerateW")))
        return fn(callback, context);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    using Fn = HRESULT(WINAPI*)();
    if (auto fn = reinterpret_cast<Fn>(RealProc("DllCanUnloadNow")))
        return fn();
    return S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID iid, void** object)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DllGetClassObject")))
        return fn(clsid, iid, object);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCaptureCreate(LPCGUID deviceGuid, void** capture, void* outer)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, void**, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundCaptureCreate")))
        return fn(deviceGuid, capture, outer);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateA(void* callback, void* context)
{
    using Fn = HRESULT(WINAPI*)(void*, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundCaptureEnumerateA")))
        return fn(callback, context);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateW(void* callback, void* context)
{
    using Fn = HRESULT(WINAPI*)(void*, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundCaptureEnumerateW")))
        return fn(callback, context);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_GetDeviceID(LPCGUID source, LPGUID destination)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, LPGUID);
    if (auto fn = reinterpret_cast<Fn>(RealProc("GetDeviceID")))
        return fn(source, destination);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundFullDuplexCreate(LPCGUID captureDeviceGuid,
                                                            LPCGUID renderDeviceGuid,
                                                            const void* captureBufferDesc,
                                                            const void* soundBufferDesc,
                                                            HWND hwnd,
                                                            DWORD level,
                                                            void** fullDuplex,
                                                            void** captureBuffer8,
                                                            void** soundBuffer8,
                                                            void* outer)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, LPCGUID, const void*, const void*, HWND, DWORD, void**, void**, void**, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundFullDuplexCreate")))
    {
        return fn(captureDeviceGuid,
                  renderDeviceGuid,
                  captureBufferDesc,
                  soundBufferDesc,
                  hwnd,
                  level,
                  fullDuplex,
                  captureBuffer8,
                  soundBuffer8,
                  outer);
    }
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCreate8(LPCGUID deviceGuid, void** directSound8, void* outer)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, void**, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundCreate8")))
        return fn(deviceGuid, directSound8, outer);
    return MissingExport();
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCaptureCreate8(LPCGUID deviceGuid, void** capture8, void* outer)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, void**, void*);
    if (auto fn = reinterpret_cast<Fn>(RealProc("DirectSoundCaptureCreate8")))
        return fn(deviceGuid, capture8, outer);
    return MissingExport();
}
