#include "hooks/fov_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "sh4/addresses.h"

#include <atomic>

namespace sh4xe::hooks::fov
{
namespace
{

// int __cdecl Camera_SetFovRadians(Camera* camera, float vfov)
using SetFovFn = int(__cdecl*)(int, float);

SetFovFn g_originalSetFov = nullptr;
bool g_installed = false;

std::atomic<float> g_scale{1.0f};
std::atomic<float> g_lastBaseFov{0.0f};

int __cdecl SetFovDetour(int camera, float vfov)
{
    g_lastBaseFov.store(vfov, std::memory_order_relaxed);

    const float scale = g_scale.load(std::memory_order_relaxed);
    // The game clamps the angle itself, but keep our own product finite/sane
    // before handing it over.
    float scaled = vfov * scale;
    if (!(scaled > 0.0f))
        scaled = vfov;

    return g_originalSetFov(camera, scaled);
}

} // namespace

bool InstallFovHook()
{
    if (g_installed)
        return true;

    if (!CreateAndEnableHook("Camera_SetFovRadians",
                             sh4::addr::kCameraSetFovRadians,
                             reinterpret_cast<void*>(&SetFovDetour),
                             &g_originalSetFov))
        return false;

    g_installed = true;
    sh4xe::Log("Camera_SetFovRadians hooked @ %p", reinterpret_cast<void*>(sh4::addr::kCameraSetFovRadians));
    return true;
}

void SetFovScale(float scale)
{
    g_scale.store(scale, std::memory_order_relaxed);
}

float GetFovScale()
{
    return g_scale.load(std::memory_order_relaxed);
}

float LastBaseFovRadians()
{
    return g_lastBaseFov.load(std::memory_order_relaxed);
}

} // namespace sh4xe::hooks::fov
