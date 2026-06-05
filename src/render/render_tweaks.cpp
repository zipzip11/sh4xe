#include "render/render_tweaks.h"

#include <atomic>

namespace sh4xe::render::tweaks
{
namespace
{

constexpr FilterMode kDefaultFilterMode = FilterMode::Anisotropic;
constexpr float kDefaultTextureLodBias = -0.35f;
constexpr bool kDefaultDitherEnabled = true;
constexpr float kDefaultFogDistanceScale = 1.30f;
constexpr float kDefaultFogDensityScale = 0.72f;

std::atomic<FilterMode> g_filterMode{kDefaultFilterMode};
std::atomic<float> g_textureLodBias{kDefaultTextureLodBias};
std::atomic<bool> g_fogDisabled{false};
std::atomic<float> g_fogDistanceScale{kDefaultFogDistanceScale};
std::atomic<float> g_fogDensityScale{kDefaultFogDensityScale};
std::atomic<bool> g_ditherEnabled{kDefaultDitherEnabled};
std::atomic<bool> g_suppressFilterOverride{false};
std::atomic<unsigned int> g_graphicsQualityRevision{1};

constexpr bool kDefaultLightingEnabled = true;
constexpr float kDefaultLightingLightBoost = 1.22f;
constexpr float kDefaultLightingAmbientBoost = 1.12f;
constexpr float kDefaultLightingAmbientLift = 0.035f;
constexpr float kDefaultLightingSpecularBoost = 1.35f;
constexpr float kDefaultLightingRangeBoost = 1.12f;
constexpr float kDefaultLightingScreenWarmth = 0.0f;
constexpr float kDefaultLightingVignette = 0.0f;

std::atomic<bool> g_lightingEnabled{kDefaultLightingEnabled};
std::atomic<float> g_lightingLightBoost{kDefaultLightingLightBoost};
std::atomic<float> g_lightingAmbientBoost{kDefaultLightingAmbientBoost};
std::atomic<float> g_lightingAmbientLift{kDefaultLightingAmbientLift};
std::atomic<float> g_lightingSpecularBoost{kDefaultLightingSpecularBoost};
std::atomic<float> g_lightingRangeBoost{kDefaultLightingRangeBoost};
std::atomic<float> g_lightingScreenWarmth{kDefaultLightingScreenWarmth};
std::atomic<float> g_lightingVignette{kDefaultLightingVignette};

float ClampFloat(float value, float lo, float hi)
{
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

void MarkGraphicsQualityChanged()
{
    g_graphicsQualityRevision.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void SetFilterMode(FilterMode mode)
{
    g_filterMode.store(mode, std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

FilterMode GetFilterMode()
{
    return g_filterMode.load(std::memory_order_relaxed);
}

const char* FilterModeName(FilterMode mode)
{
    switch (mode)
    {
        case FilterMode::Point:
            return "point";
        case FilterMode::Linear:
            return "linear";
        case FilterMode::Anisotropic:
            return "aniso";
        case FilterMode::Game:
        default:
            return "game";
    }
}

void ResetGraphicsQualityTweak()
{
    g_filterMode.store(kDefaultFilterMode, std::memory_order_relaxed);
    g_textureLodBias.store(kDefaultTextureLodBias, std::memory_order_relaxed);
    g_fogDisabled.store(false, std::memory_order_relaxed);
    g_fogDistanceScale.store(kDefaultFogDistanceScale, std::memory_order_relaxed);
    g_fogDensityScale.store(kDefaultFogDensityScale, std::memory_order_relaxed);
    g_ditherEnabled.store(kDefaultDitherEnabled, std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

void SetTextureLodBias(float bias)
{
    g_textureLodBias.store(ClampFloat(bias, -2.0f, 2.0f), std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

float TextureLodBias()
{
    return g_textureLodBias.load(std::memory_order_relaxed);
}

void SetFogDisabled(bool disabled)
{
    g_fogDisabled.store(disabled, std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

bool FogDisabled()
{
    return g_fogDisabled.load(std::memory_order_relaxed);
}

void SetFogDistanceScale(float scale)
{
    g_fogDistanceScale.store(ClampFloat(scale, 0.25f, 4.0f), std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

float FogDistanceScale()
{
    return g_fogDistanceScale.load(std::memory_order_relaxed);
}

void SetFogDensityScale(float scale)
{
    g_fogDensityScale.store(ClampFloat(scale, 0.0f, 4.0f), std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

float FogDensityScale()
{
    return g_fogDensityScale.load(std::memory_order_relaxed);
}

void SetDitherEnabled(bool enabled)
{
    g_ditherEnabled.store(enabled, std::memory_order_relaxed);
    MarkGraphicsQualityChanged();
}

bool DitherEnabled()
{
    return g_ditherEnabled.load(std::memory_order_relaxed);
}

unsigned int GraphicsQualityRevision()
{
    return g_graphicsQualityRevision.load(std::memory_order_relaxed);
}

void SetSuppressFilterOverride(bool suppress)
{
    g_suppressFilterOverride.store(suppress, std::memory_order_relaxed);
}

bool SuppressFilterOverride()
{
    return g_suppressFilterOverride.load(std::memory_order_relaxed);
}

void ResetLightingTweak()
{
    g_lightingEnabled.store(kDefaultLightingEnabled, std::memory_order_relaxed);
    g_lightingLightBoost.store(kDefaultLightingLightBoost, std::memory_order_relaxed);
    g_lightingAmbientBoost.store(kDefaultLightingAmbientBoost, std::memory_order_relaxed);
    g_lightingAmbientLift.store(kDefaultLightingAmbientLift, std::memory_order_relaxed);
    g_lightingSpecularBoost.store(kDefaultLightingSpecularBoost, std::memory_order_relaxed);
    g_lightingRangeBoost.store(kDefaultLightingRangeBoost, std::memory_order_relaxed);
    g_lightingScreenWarmth.store(kDefaultLightingScreenWarmth, std::memory_order_relaxed);
    g_lightingVignette.store(kDefaultLightingVignette, std::memory_order_relaxed);
}

void SetLightingEnabled(bool enabled)
{
    g_lightingEnabled.store(enabled, std::memory_order_relaxed);
}

bool LightingEnabled()
{
    return g_lightingEnabled.load(std::memory_order_relaxed);
}

void SetLightingLightBoost(float boost)
{
    g_lightingLightBoost.store(ClampFloat(boost, 0.0f, 3.0f), std::memory_order_relaxed);
}

float LightingLightBoost()
{
    return g_lightingLightBoost.load(std::memory_order_relaxed);
}

void SetLightingAmbientBoost(float boost)
{
    g_lightingAmbientBoost.store(ClampFloat(boost, 0.0f, 3.0f), std::memory_order_relaxed);
}

float LightingAmbientBoost()
{
    return g_lightingAmbientBoost.load(std::memory_order_relaxed);
}

void SetLightingAmbientLift(float lift)
{
    g_lightingAmbientLift.store(ClampFloat(lift, 0.0f, 0.25f), std::memory_order_relaxed);
}

float LightingAmbientLift()
{
    return g_lightingAmbientLift.load(std::memory_order_relaxed);
}

void SetLightingSpecularBoost(float boost)
{
    g_lightingSpecularBoost.store(ClampFloat(boost, 0.0f, 4.0f), std::memory_order_relaxed);
}

float LightingSpecularBoost()
{
    return g_lightingSpecularBoost.load(std::memory_order_relaxed);
}

void SetLightingRangeBoost(float boost)
{
    g_lightingRangeBoost.store(ClampFloat(boost, 0.25f, 3.0f), std::memory_order_relaxed);
}

float LightingRangeBoost()
{
    return g_lightingRangeBoost.load(std::memory_order_relaxed);
}

void SetLightingScreenWarmth(float warmth)
{
    g_lightingScreenWarmth.store(ClampFloat(warmth, 0.0f, 1.0f), std::memory_order_relaxed);
}

float LightingScreenWarmth()
{
    return g_lightingScreenWarmth.load(std::memory_order_relaxed);
}

void SetLightingVignette(float vignette)
{
    g_lightingVignette.store(ClampFloat(vignette, 0.0f, 1.0f), std::memory_order_relaxed);
}

float LightingVignette()
{
    return g_lightingVignette.load(std::memory_order_relaxed);
}

} // namespace sh4xe::render::tweaks
