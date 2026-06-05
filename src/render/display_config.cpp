#include "render/display_config.h"

#include "core/framework.h"
#include "render/d3d8_console.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sh4xe::render::display
{
namespace
{

struct PendingChange
{
    bool modeSet = false;
    Mode mode = Mode::Windowed;
    bool resolutionSet = false;
    UINT width = 0;
    UINT height = 0;
};

std::mutex g_mutex;
PresentParameters g_present = {};
bool g_havePresent = false;
HWND g_window = nullptr;
Mode g_mode = Mode::Windowed;
PendingChange g_pending = {};

DWORD g_savedStyle = 0;
DWORD g_savedExStyle = 0;
RECT g_savedRect = {};
bool g_haveSavedWindow = false;

bool HasPending(const PendingChange& pending)
{
    return pending.modeSet || pending.resolutionSet;
}

void Format(char* out, size_t outSize, const char* fmt, ...)
{
    if (!out || outSize == 0)
        return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(out, outSize, fmt, args);
    va_end(args);
    out[outSize - 1] = '\0';
}

HWND ResolveWindow(const PresentParameters& params)
{
    if (params.hDeviceWindow)
        return params.hDeviceWindow;
    if (g_window)
        return g_window;
    return GetActiveWindow();
}

RECT MonitorRectForWindow(HWND hwnd)
{
    RECT rect = {};
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info))
        return info.rcMonitor;

    rect.right = GetSystemMetrics(SM_CXSCREEN);
    rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    return rect;
}

void SaveWindowPlacement(HWND hwnd)
{
    if (!hwnd || g_haveSavedWindow)
        return;

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    if ((style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER)) == 0)
        return;

    g_savedStyle = style;
    g_savedExStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    GetWindowRect(hwnd, &g_savedRect);
    g_haveSavedWindow = true;
}

void SetWindowedStyle(HWND hwnd, const PresentParameters& params)
{
    if (!hwnd)
        return;

    SaveWindowPlacement(hwnd);

    const DWORD style = g_haveSavedWindow ? g_savedStyle : WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = g_haveSavedWindow ? g_savedExStyle : 0;
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle));

    RECT target = {};
    if (g_haveSavedWindow)
    {
        target = g_savedRect;
    }
    else
    {
        RECT monitor = MonitorRectForWindow(hwnd);
        const LONG width = static_cast<LONG>(params.BackBufferWidth ? params.BackBufferWidth : 640);
        const LONG height = static_cast<LONG>(params.BackBufferHeight ? params.BackBufferHeight : 480);
        target.right = width;
        target.bottom = height;
        AdjustWindowRectEx(&target, style, FALSE, exStyle);
        const LONG windowW = target.right - target.left;
        const LONG windowH = target.bottom - target.top;
        target.left = monitor.left + ((monitor.right - monitor.left) - windowW) / 2;
        target.top = monitor.top + ((monitor.bottom - monitor.top) - windowH) / 2;
        target.right = target.left + windowW;
        target.bottom = target.top + windowH;
    }

    SetWindowPos(hwnd,
                 nullptr,
                 target.left,
                 target.top,
                 target.right - target.left,
                 target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void SetBorderlessStyle(HWND hwnd)
{
    if (!hwnd)
        return;

    SaveWindowPlacement(hwnd);

    const RECT monitor = MonitorRectForWindow(hwnd);
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, 0);
    SetWindowPos(hwnd,
                 HWND_TOP,
                 monitor.left,
                 monitor.top,
                 monitor.right - monitor.left,
                 monitor.bottom - monitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void EnsureFullscreenSize(PresentParameters& params, HWND hwnd)
{
    if (params.BackBufferWidth && params.BackBufferHeight)
        return;

    const RECT monitor = MonitorRectForWindow(hwnd);
    params.BackBufferWidth = static_cast<UINT>(std::max<LONG>(1, monitor.right - monitor.left));
    params.BackBufferHeight = static_cast<UINT>(std::max<LONG>(1, monitor.bottom - monitor.top));
}

void CaptureLocked(const PresentParameters* params)
{
    if (!params)
        return;

    g_present = *params;
    g_havePresent = true;
    if (params->hDeviceWindow)
        g_window = params->hDeviceWindow;
    g_mode = params->Windowed ? g_mode : Mode::Fullscreen;
    if (params->Windowed && g_mode == Mode::Fullscreen)
        g_mode = Mode::Windowed;
}

void QueueLocked(const PendingChange& change)
{
    if (change.modeSet)
    {
        g_pending.modeSet = true;
        g_pending.mode = change.mode;
    }
    if (change.resolutionSet)
    {
        g_pending.resolutionSet = true;
        g_pending.width = change.width;
        g_pending.height = change.height;
    }
}

} // namespace

const char* ModeName(Mode mode)
{
    switch (mode)
    {
        case Mode::Fullscreen:
            return "fullscreen";
        case Mode::Borderless:
            return "borderless";
        case Mode::Windowed:
        default:
            return "windowed";
    }
}

void CaptureCreateDevice(HWND focusWindow, const PresentParameters* params)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (focusWindow)
        g_window = focusWindow;
    CaptureLocked(params);
}

void CaptureReset(const PresentParameters* params)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CaptureLocked(params);
}

bool RequestMode(Mode mode, char* out, size_t outSize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    PendingChange change = {};
    change.modeSet = true;
    change.mode = mode;
    QueueLocked(change);
    Format(out, outSize, "ok display = %s queued", ModeName(mode));
    return true;
}

bool RequestResolution(UINT width, UINT height, char* out, size_t outSize)
{
    if (width < 320 || height < 200 || width > 7680 || height > 4320)
    {
        Format(out, outSize, "err display resolution: use 320x200 through 7680x4320");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    PendingChange change = {};
    change.resolutionSet = true;
    change.width = width;
    change.height = height;
    QueueLocked(change);
    Format(out, outSize, "ok internal resolution = %ux%u queued", width, height);
    return true;
}

bool RequestModeAndResolution(Mode mode, UINT width, UINT height, char* out, size_t outSize)
{
    if (width < 320 || height < 200 || width > 7680 || height > 4320)
    {
        Format(out, outSize, "err display resolution: use 320x200 through 7680x4320");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    PendingChange change = {};
    change.modeSet = true;
    change.mode = mode;
    change.resolutionSet = true;
    change.width = width;
    change.height = height;
    QueueLocked(change);
    Format(out, outSize, "ok display = %s %ux%u queued", ModeName(mode), width, height);
    return true;
}

void Describe(char* out, size_t outSize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_havePresent)
    {
        Format(out, outSize, "ok display: waiting for D3D presentation parameters");
        return;
    }

    Format(out,
           outSize,
           "ok display = %s, internal resolution = %ux%u%s",
           ModeName(g_mode),
           g_present.BackBufferWidth,
           g_present.BackBufferHeight,
           HasPending(g_pending) ? " (change queued)" : "");
}

void ApplyPending(void* device, ResetFn reset)
{
    if (!device || !reset)
        return;

    PresentParameters params = {};
    PendingChange pending = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!HasPending(g_pending))
            return;

        if (!g_havePresent)
        {
            g_pending = {};
            params = {};
            pending = {};
        }
        else
        {
            params = g_present;
            pending = g_pending;
            g_pending = {};
        }
    }

    if (!HasPending(pending))
    {
        sh4xe::Log("display reset skipped: D3D presentation parameters unavailable");
        d3d8_console::AddLog("err display: D3D presentation parameters unavailable");
        d3d8_console::AddLog("err display: restart the game if this was queued before CreateDevice was captured");
        return;
    }

    if (pending.resolutionSet)
    {
        params.BackBufferWidth = pending.width;
        params.BackBufferHeight = pending.height;
    }

    const Mode targetMode = pending.modeSet ? pending.mode : (params.Windowed ? g_mode : Mode::Fullscreen);
    HWND hwnd = ResolveWindow(params);
    if (!hwnd)
    {
        sh4xe::Log("display reset skipped: game window unavailable");
        d3d8_console::AddLog("err display: game window unavailable");
        return;
    }

    if (targetMode == Mode::Fullscreen)
    {
        SaveWindowPlacement(hwnd);
        params.Windowed = FALSE;
        EnsureFullscreenSize(params, hwnd);
    }
    else
    {
        params.Windowed = TRUE;
        params.FullScreen_RefreshRateInHz = 0;
        if (targetMode == Mode::Borderless)
            SetBorderlessStyle(hwnd);
        else
            SetWindowedStyle(hwnd, params);
    }

    sh4xe::Log("display reset requested mode=%s resolution=%ux%u",
               ModeName(targetMode),
               params.BackBufferWidth,
               params.BackBufferHeight);

    const HRESULT hr = reset(device, &params);
    sh4xe::Log("display reset returned hr=0x%08lX", static_cast<unsigned long>(hr));
    if (FAILED(hr))
    {
        sh4xe::Log("display reset failed mode=%s resolution=%ux%u",
                   ModeName(targetMode),
                   params.BackBufferWidth,
                   params.BackBufferHeight);
        d3d8_console::AddLog("err display: Reset failed hr=0x%08lX", static_cast<unsigned long>(hr));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_present = params;
        g_havePresent = true;
        g_window = hwnd;
        g_mode = targetMode;
    }

    if (targetMode == Mode::Borderless)
        SetBorderlessStyle(hwnd);
    else if (targetMode == Mode::Windowed)
        SetWindowedStyle(hwnd, params);

    sh4xe::Log("display reset applied mode=%s resolution=%ux%u",
               ModeName(targetMode),
               params.BackBufferWidth,
               params.BackBufferHeight);
    d3d8_console::AddLog("ok display = %s, internal resolution = %ux%u",
                         ModeName(targetMode),
                         params.BackBufferWidth,
                         params.BackBufferHeight);
}

} // namespace sh4xe::render::display
