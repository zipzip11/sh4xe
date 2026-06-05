#pragma once

namespace sh4xe::render::tweaks
{

// Texture filtering override applied to every stage the game configures.
// Game   -> pass the game's own filter choice through untouched.
// Point  -> force nearest-neighbour (sharp, pixelated).
// Linear -> force bilinear (+ trilinear mips).
// Aniso  -> force anisotropic (+ trilinear mips, max anisotropy).
enum class FilterMode
{
    Game,
    Point,
    Linear,
    Anisotropic,
};

void SetFilterMode(FilterMode mode);
FilterMode GetFilterMode();
const char* FilterModeName(FilterMode mode);

// Global texture quality controls. The default modern profile forces
// anisotropic/trilinear sampling and a modest negative mip bias; `filter game`
// returns texture sampling to the game's own choices.
void ResetGraphicsQualityTweak();

void SetTextureLodBias(float bias);
float TextureLodBias();

// When true, every D3DRS_FOGENABLE the game sets is forced off. Turning it
// back on simply stops overriding; the game restores its own fog on the next
// state set.
void SetFogDisabled(bool disabled);
bool FogDisabled();

void SetFogDistanceScale(float scale);
float FogDistanceScale();

void SetFogDensityScale(float scale);
float FogDensityScale();

void SetDitherEnabled(bool enabled);
bool DitherEnabled();

// Incremented whenever a render-quality setting changes so the D3D8 hook can
// lazily re-seed persistent render states on the live device.
unsigned int GraphicsQualityRevision();

// While set, the SetTextureStageState filter override is bypassed so the overlay
// can choose its own font filtering regardless of the active FilterMode. The
// console raises this only around its own draw, on the render thread.
void SetSuppressFilterOverride(bool suppress);
bool SuppressFilterOverride();

// Lightweight DX8 lighting enhancement. It combines fixed-function light/material
// boosting with a shaderless end-of-scene grade pass.
void ResetLightingTweak();
void SetLightingEnabled(bool enabled);
bool LightingEnabled();

void SetLightingLightBoost(float boost);
float LightingLightBoost();

void SetLightingAmbientBoost(float boost);
float LightingAmbientBoost();

void SetLightingAmbientLift(float lift);
float LightingAmbientLift();

void SetLightingSpecularBoost(float boost);
float LightingSpecularBoost();

void SetLightingRangeBoost(float boost);
float LightingRangeBoost();

void SetLightingScreenWarmth(float warmth);
float LightingScreenWarmth();

void SetLightingVignette(float vignette);
float LightingVignette();

} // namespace sh4xe::render::tweaks
