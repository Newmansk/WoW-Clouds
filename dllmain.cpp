// wow_clouds.cpp
// Procedural Cloud Layer ("Breath of the Wild" style) for WoW 1.12.x / 1.18.1 (32 bits, Direct3D 9) client.
// Loads with VanillaFixes (dlls.txt). Doesnt need external libraries: patches the vtables of D3D9 directly.
//
// Idea: just before the game starts rendering the UI (the initial switch to orthographic projection 
// after rendering the world in perspective), we draw a full-screen quad at the far plane (z = 1.0) 
// using a pixel shader. With ZFUNC set to LESSEQUAL and depth writing disabled, pixels are drawn only 
// where the world hasn't rendered anything (i.e., the sky). Afterward, the full device state is restored.

#include "pch.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------------------------
// Extra cloud layers (optional, off by default -- see [CloudsLayer2]/[CloudsLayer3] in the ini).
// Layer 1 (the original/base layer) keeps using the top-level [Clouds] fields in Config below,
// unchanged, so existing ini files keep working exactly as before.
// ---------------------------------------------------------------------------------------------
struct CloudLayerConfig {
    int   enabled = 0;
    float scale = 1.2f;
    float coverage = 0.55f;
    float softness = 0.06f;
    float opacity = 0.95f;
    float windX = 0.030f, windY = 0.012f;
    float horizonFade = 0.20f;
    float dome = 0.30f;
    float evolve = 0.08f;
    float detail = 1.5f;
    float bandSoft = 0.07f;
    float timeScale = 1.0f;
    // Offsets this layer's noise field so it doesn't look like an exact copy of the other layers
    // at a different scale. 0 = original/unshifted noise (this is what layer 1 always uses).
    float noiseSeed = 0.0f;
    // Coverage/opacity this layer moves to while weather (rain/snow/dust) is active, replacing the
    // global RainCoverage/RainOpacity for this layer only. Negative = not set: the layer then
    // inherits the global weather shift (same absolute change as layer 1), as before this existed.
    float rainCoverage = -1.0f;
    float rainOpacity = -1.0f;
};

// ---------------------------------------------------------------------------------------------
// Configuration (wow_clouds.ini, with DLL; loads after saving)
// ---------------------------------------------------------------------------------------------
struct Config {
    int   enabled = 1;
    int   testMode = 0;         // 1 = paints the sky magenta (to verify that the drawing appears where it should)
    int   debug = 0;            // 1 = log detailed in wow_clouds.log
    int   upAxis = 2;           // axis "up" from the view matrix input space: 0=X 1=Y 2=Z
    float upSign = 1.0f;
    int   useFogTint = 0;       // sample the game's fog tone (the captured value is unreliable in this client)
    int   writeAlpha = 0;       // 1 = the clouds also write to the render target's alpha channel
    int   skyDump = 0;          // 1 = dumps the sky phase drawing calls into the log
    // Set to 1, save the ini, do whatever in-game thing you want to capture (sheath/unsheath a
    // weapon, etc.), and every sky-phase draw call for the next ~3 seconds gets dumped to the log
    // -- unlike SkyDump above, which only fires on fixed, unpredictable frames. Resets itself back
    // to 0 in the ini right after triggering, so it won't keep dumping on every later hot-reload.
    int   dumpNow = 0;
    // 1 = exposes Lua functions to in-game addons (see the Lua bridge section above H_Present).
    // Off by default: it calls engine functions by address, so it's opt-in until verified.
    int   luaBridge = 0;
    int   hideOrig = 0;         // 1 = It does not draw the calls listed in CloudDraws (original clouds)
    int   layer = 1;            // 1 = draw the clouds at the original position in the sky (always behind everything); 0 = at the end of the world with a depth test
    int   cloudPrims = 372;     // original cloud dome triangles (identifies the specific one even if the order changes)
    int   skyColorPrims = 298;  // sky gradient triangles (without texture), whose vertex color is the sky color
    int   colorTrace = 0;       // 1 = write down each one ~0.5 s the vertex color of the first 3 sky draw calls
    int   skyColorDraw = 1;     // call of the sky, featuring vertices colored like the sky (in a gradient)
    int   colorSource = 5;      // 0 fixed | 1 sky survey | 2 vertex color | 3 TEXTUREFACTOR | 4 material (of CloudDraws)
    int   skySampleEvery = 12;
    float tintMix = 0.40f, colorGain = 1.30f, skyLumNight = 0.08f, skyLumDay = 0.35f;
    unsigned long long cloudMask = 0;
    float minLit = 0.10f;       // glossy floor the color of clouds (avoid black clouds at night)
    float evolve = 0.08f;       // speed at which the shape of the clouds changes (0 = they simply move)
    float detail = 1.5f;        // weight of the fine noise octaves: higher value = more detail y more defined edges
    float bandSoft = 0.07f;     // softness of the shading bands (lower = sharper)
    float timeScale = 1.0f;     // global speed multiplier (wind + evolution)
    float dome = 0.30f;         // 0 = infinite plane (highly stretched clouds), higher value = more dome-like/less stretching
    float scale = 1.2f;
    float coverage = 0.55f;      // 0..1
    float softness = 0.06f;
    float opacity = 0.95f;
    float thrFloor = -0.20f;     // Shader threshold when effective coverage = 1.0 (see DrawClouds). 
    // Must be below the actual minimum of the fbm5 noise (~0 in theory,
    // but in practice it rarely drops below ~0.2–0.3) to avoid
    // clear-sky gaps when coverage=1. A more negative value results in a
    // "flatter" sky (less density variation) when fully overcast.

    //-- Rain transition parameters --
    float rainCoverage = 1.0f;    // cloud cover with active weather (rain/snow/dust)
    float rainOpacity = 1.00f;    // cloud opacity with active weather
    float rainDarken = 0.55f;     // 1 = no change, lower = darker
    float rainBlendSpeed = 0.6f;  // how fast the transition occurs (per second)

    // Fade speed when entering/exiting a [Preset:...] by AreaId (units/sec; same
    // mechanism as RainBlendSpeed ​​but applied directly to each value, rather than a single 0–1 factor—
    // this works well even when transitioning directly from one preset to another without passing through a "base" zone).
    float zoneBlendSpeed = 0.6f;
    // Optional per-field overrides (defaulting to ZoneBlendSpeed) — useful because
    // CoverageAdd typically has a much shorter travel distance than Darken/SpeedMult;
    // at the same speed (units/sec), it reaches its target sooner, making it feel "faster"
    // even though it isn't really.
    float zoneDarkenBlendSpeed = 0.6f;
    float zoneSpeedBlendSpeed = 0.6f;
    float zoneCoverageBlendSpeed = 0.6f;
    float zoneTintMixBlendSpeed = 0.6f;
    float zoneWeatherBlendSpeedMultBlendSpeed = 0.6f;

    // -- Extra daytime transparency, only in clear weather --
    // Uses the same actual sky color (g_skyVColor) and the same SkyLumNight/SkyLumDay 
    // thresholds already used for cloud night-darkening (ComputeColors), so no new memory 
    // addresses or unverified elements are required.
    int   dayTransparencyEnabled = 1;
    float dayTransparencyAmount = 0.60f;  // 0.60 = 60% more transparent during the day

    // -- Weather detection based on sky color --
    float wxBaselineTau = 90.0f;   // seconds it takes for the "baseline" to follow the actual color
    float wxDevEnter = 0.07f;      // distance (obscuration) at which to start considering it to be raining
    float wxDevExit = 0.03f;       // distance below which it is considered clear again
    float wxConfirmSec = 1.0f;     // seconds the distance must be maintained before confirming the change
    // Speed ​​(in luminance per second) above which a frame-to-frame change is considered an external jump 
    // (e.g., a mod abruptly changing the time) rather than actual weather. Measured rainfall changes by at 
    // most ~0.02/s; we allow plenty of headroom above that.
    float wxJumpRate = 0.15f;
    float wxJumpWindow = 0.25f;   // minimum seconds between comparisons for a sudden jump (avoids single-frame noise)
    int   wxLog = 0;              // 1 = writes the baseline, actual color, and status to the log every second
    // Main detector: CMapWeather+0x20 sends a pulse (0=clear, 1=rain, 2=snow, 3=dust) for a few seconds 
    // when the weather changes, and remains at -1 the rest of the time. We "hold" this value by storing the last observed pulse. 
    // 1 = use this detector; 0 = use only the color-based one. Main detector: CMapWeather has an effect "slot" for each 
    // weather type (confirmed via Cheat Engine): +0x28 = rain effect pointer (0 if none), +0x2c = snow, +0x30 = dust. 
    // Unlike +0x20 (which only pulses briefly and does not always trigger when transitioning out of "clear" weather), 
    // these remain active while the effect is ongoing and revert to 0 when it ends. 1 = use this detector.
    int   wxUseSlots = 1;
    // Backup: the +0x20 pulse (failure during the transition from clear to new weather; see notes).
    // It is used only if the slot-based detector cannot read the manager (object not yet ready).
    int   wxUsePulse = 1;
    // 0x00B71EA8: AreaId, confirmed by the user (see note next to ReadClientAreaId). 1 = log
    // every second. Used to drive the zone presets below (darken/speedMult/coverageAdd).
    int   areaLog = 0;
    // 1 = logs the active preset and the smoothed values ​​every second (see g_zoneDarken, etc.)
    int   presetLog = 0;

    float windX = 0.030f, windY = 0.012f;
    float horizonFade = 0.20f;
    float sunAz = 45.0f, sunEl = 50.0f;   // degrees
    float quadDepth = 1.0f;
    float nightBrightness = 0.18f;
    float sunColor[3] = { 1.00f, 0.97f, 0.90f };
    float shadowColor[3] = { 0.52f, 0.60f, 0.78f };

    // Optional extra layers for depth (farther/bigger -> closer/smaller). Both start disabled;
    // see LoadConfig for their suggested non-zero defaults, and [CloudsLayer2]/[CloudsLayer3] in
    // the ini to enable and tweak them.
    CloudLayerConfig layer2;
    CloudLayerConfig layer3;

    // 1.0 = full resolution (unchanged behavior). Below 1.0, all 3 layers are rendered into a
    // smaller offscreen buffer (this fraction of the real target's width/height) and the result is
    // stretched back onto the screen, to cut the GPU cost of the noise shader. Only applies to the
    // normal sky-phase draw (Layer=1 path); the rare end-of-frame fallback always renders full-res.
    float renderScale = 1.0f;
};

static Config g_cfg;
static HMODULE g_self;
static char g_iniPath[MAX_PATH], g_logPath[MAX_PATH];
static FILETIME g_iniTime;
static DWORD g_lastIniCheck;

static double NowSec() {
    static LARGE_INTEGER f, t0;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

static bool   g_dumpWindowActive = false;   // DumpNow: a manual, on-demand dump window (see LoadConfig)
static double g_dumpWindowUntil = 0.0;      // NowSec() timestamp at which that window closes

// -- Weather: detected by the rate of change of the sky color (g_skyVColor) --
enum { WX_UNKNOWN = -1, WX_CLEAR = 0, WX_RAIN = 1, WX_SNOW = 2, WX_SAND = 3 };
static volatile int g_weather = WX_CLEAR;
static float g_rainBlend = 0.0f;   // 0 = normal weather, 1 = full rain
static double g_lastBlendTime = -1.0;   // to measure the actual time between frames (independent of FPS)

// Accumulated effective time (replaces NowSec()*TimeScale for cloud evolution/wind).
// It is incremented once per frame in H_Present by the actual dt, multiplied by TimeScale and
// by the g_zoneSpeedMult (already updated for that frame)—this ensures the zone preset
// deceleration is truly gradual, rather than recalculating "time * speed" each time
// (which would cause stuttering, since the speed changes incrementally during the transition).
static double g_animTime = 0.0;
// Same idea as g_animTime, but WITHOUT TimeScale baked in (only dt * g_zoneSpeedMult), so layers 2
// and 3 can each apply their own TimeScale at draw time without touching layer 1's accumulator
// (which keeps it pixel-identical to before this feature existed).
static double g_animTimeRaw = 0.0;

// Zone preset values, already smoothed (see the blend block in H_Present). They start at
// neutral values ​​(with no active preset).
static float g_zoneDarken = 1.0f;
static float g_zoneSpeedMult = 1.0f;
static float g_zoneCoverageAdd = 0.0f;
static float g_zoneTintMix = 0.40f;             // matches Config's default TintMix; converges to the real one on first LoadConfig
static float g_zoneWeatherBlendSpeedMult = 1.0f;

static bool   g_wxBaselineInit = false;
static float  g_wxBaseline = 0.0f;     // baseline: follows the actual color slowly
static float  g_wxLastLum = 0.0f;      // color from the last time the jump was evaluated (not necessarily the previous frame)
static double g_wxLastJumpCheckTime = -1.0;
static double g_lastWxTime = -1.0;
static double g_wxCandidateSince = -1.0;  // time when the candidate state began to sustain itself
static int    g_wxCandidate = WX_CLEAR;

// -- Main detector: last pulse seen at CMapWeather+0x20 --
static int    g_wxPulseHeld = WX_CLEAR;   // last valid pulse (0-3) we saw, sustained until the next one
static bool   g_wxPulseEverSeen = false;  // false until the first valid pulse (prevents starting in an arbitrary state)

// -- Daytime transparency: reproduces the true color of the sky, just like the nighttime darkening. --
static float  g_dayness = 0.0f;     // 0 = pitch black, 1 = broad daylight

static void LogV(const char* fmt, va_list ap) {
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
}
static void LogAlways(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(fmt, ap); va_end(ap); }
static void LogDbg(const char* fmt, ...) { if (!g_cfg.debug) return; va_list ap; va_start(ap, fmt); LogV(fmt, ap); va_end(ap); }

static float ReadF(const char* key, float def) {
    char buf[64], dbuf[64];
    snprintf(dbuf, sizeof dbuf, "%g", def);
    GetPrivateProfileStringA("Clouds", key, dbuf, buf, sizeof buf, g_iniPath);
    return (float)atof(buf);
}
// Same as ReadF but from an arbitrary section (for [Preset:Name]).
static float ReadFSection(const char* section, const char* key, float def) {
    char buf[64], dbuf[64];
    snprintf(dbuf, sizeof dbuf, "%g", def);
    GetPrivateProfileStringA(section, key, dbuf, buf, sizeof buf, g_iniPath);
    return (float)atof(buf);
}

// Loads one optional extra cloud layer from its own ini section ([CloudsLayer2]/[CloudsLayer3]).
// lc is passed already filled with that layer's suggested defaults (see LoadConfig); any key
// missing from the ini keeps the suggested default instead of falling back to layer 1's values.
static void LoadExtraCloudLayer(const char* section, CloudLayerConfig& lc) {
    lc.enabled = (int)ReadFSection(section, "Enabled", (float)lc.enabled);
    lc.scale = ReadFSection(section, "Scale", lc.scale);
    lc.coverage = ReadFSection(section, "Coverage", lc.coverage);
    lc.softness = ReadFSection(section, "Softness", lc.softness);
    lc.opacity = ReadFSection(section, "Opacity", lc.opacity);
    lc.windX = ReadFSection(section, "WindX", lc.windX);
    lc.windY = ReadFSection(section, "WindY", lc.windY);
    lc.horizonFade = ReadFSection(section, "HorizonFade", lc.horizonFade);
    lc.dome = ReadFSection(section, "Dome", lc.dome);
    lc.evolve = ReadFSection(section, "Evolve", lc.evolve);
    lc.detail = ReadFSection(section, "Detail", lc.detail);
    lc.bandSoft = ReadFSection(section, "BandSoftness", lc.bandSoft);
    lc.timeScale = ReadFSection(section, "TimeScale", lc.timeScale);
    lc.noiseSeed = ReadFSection(section, "NoiseSeed", lc.noiseSeed);
    lc.rainCoverage = ReadFSection(section, "RainCoverage", lc.rainCoverage);
    lc.rainOpacity = ReadFSection(section, "RainOpacity", lc.rainOpacity);
}

// -- Zone presets by AreaId --------------------------------------------------------------
// Each preset is a [Preset:Name] section within the same .ini file. They are auto-discovered
// (no master list is required): any section starting with "Preset:" loads automatically.
// AreaIds=46,51          -> List of AreaIds (0x00B71EA8, confirmed) that activate this preset.
// Darken=0.6             -> 1.0 = no change; uses the same units as RainDarken (lower value = darker).
// SpeedMult=0.5          -> 1.0 = no change; multiplies TimeScale.
// CoverageAdd=0.1        -> 0.0 = no change; added to Coverage.
// If an AreaId appears in more than one preset, the first one loaded (based on .ini file order)
// takes precedence and a warning is logged; the others simply ignore that AreaId.
#define MAX_ZONE_PRESETS 16
#define MAX_IDS_PER_PRESET 32

struct ZonePreset {
    char  name[48];
    int   areaIds[MAX_IDS_PER_PRESET];
    int   areaCount;
    float darken;
    float speedMult;
    float coverageAdd;
    float tintMix;               // per-zone override of TintMix (color tint strength); defaults to the global TintMix
    float weatherBlendSpeedMult; // multiplies RainBlendSpeed in this zone; 1.0 = no change
};

static ZonePreset g_zonePresets[MAX_ZONE_PRESETS];
static int        g_zonePresetCount = 0;

// true if areaId is already assigned to a preset loaded before presetIdxExclusive (to
// detect duplicates during loading). Returns the index of the owning preset, or -1 if none.
static int FindPresetOwningArea(int areaId, int presetIdxExclusive) {
    for (int p = 0; p < presetIdxExclusive; p++) {
        for (int i = 0; i < g_zonePresets[p].areaCount; i++) {
            if (g_zonePresets[p].areaIds[i] == areaId) return p;
        }
    }
    return -1;
}

static void LoadZonePresets() {
    g_zonePresetCount = 0;

    char sectionNames[8192];
    DWORD len = GetPrivateProfileSectionNamesA(sectionNames, sizeof(sectionNames), g_iniPath);
    if (len == 0) return;

    for (char* name = sectionNames; *name; name += strlen(name) + 1) {
        if (strncmp(name, "Preset:", 7) != 0) continue;
        if (g_zonePresetCount >= MAX_ZONE_PRESETS) {
            LogAlways("[PRESET] Maximum of %d presets reached; ignoring \"%s\"", MAX_ZONE_PRESETS, name);
            continue;
        }

        ZonePreset& pr = g_zonePresets[g_zonePresetCount];
        strncpy(pr.name, name + 7, sizeof(pr.name) - 1);
        pr.name[sizeof(pr.name) - 1] = 0;
        pr.areaCount = 0;
        pr.darken = ReadFSection(name, "Darken", 1.0f);
        pr.speedMult = ReadFSection(name, "SpeedMult", 1.0f);
        pr.coverageAdd = ReadFSection(name, "CoverageAdd", 0.0f);
        // Neutral default for TintMix is the global TintMix (g_cfg is already loaded at this point,
        // see LoadConfig), so a preset that doesn't set it just inherits the global tint strength.
        pr.tintMix = ReadFSection(name, "TintMix", g_cfg.tintMix);
        pr.weatherBlendSpeedMult = ReadFSection(name, "WeatherBlendSpeedMult", 1.0f);

        char idsBuf[512];
        GetPrivateProfileStringA(name, "AreaIds", "", idsBuf, sizeof idsBuf, g_iniPath);
        for (const char* p = idsBuf; *p;) {
            while (*p && (*p < '0' || *p > '9')) p++;
            if (!*p) break;
            int id = atoi(p);
            while (*p >= '0' && *p <= '9') p++;

            int owner = FindPresetOwningArea(id, g_zonePresetCount);
            if (owner >= 0) {
                LogAlways("[PRESET] AreaId %d already belongs to \"%s\"; ignored in \"%s\"",
                    id, g_zonePresets[owner].name, pr.name);
                continue;
            }
            if (pr.areaCount < MAX_IDS_PER_PRESET) {
                pr.areaIds[pr.areaCount++] = id;
            }
            else {
                LogAlways("[PRESET] \"%s\" reached the maximum of %d AreaIds, ignoring %d", pr.name, MAX_IDS_PER_PRESET, id);
            }
        }

        if (g_cfg.presetLog) {
            LogAlways("[PRESET] loaded \"%s\": %d zone(s), Darken=%.2f SpeedMult=%.2f CoverageAdd=%.2f TintMix=%.2f WeatherBlendSpeedMult=%.2f",
                pr.name, pr.areaCount, pr.darken, pr.speedMult, pr.coverageAdd, pr.tintMix, pr.weatherBlendSpeedMult);
        }
        g_zonePresetCount++;
    }
}

// Searches for the preset containing areaId. Returns the index, or -1 if none is found ("base" zone).
static int FindPresetForArea(int areaId) {
    for (int p = 0; p < g_zonePresetCount; p++) {
        for (int i = 0; i < g_zonePresets[p].areaCount; i++) {
            if (g_zonePresets[p].areaIds[i] == areaId) return p;
        }
    }
    return -1;
}
static void ReadColor(const char* key, float out[3]) {
    char buf[96];
    GetPrivateProfileStringA("Clouds", key, "", buf, sizeof buf, g_iniPath);
    float r, g, b;
    if (sscanf(buf, "%f , %f , %f", &r, &g, &b) == 3) { out[0] = r; out[1] = g; out[2] = b; }
}

static void TunablesClearDirty();   // defined with the Lua bridge below

static void LoadConfig() {
    Config c;  // default values
    c.enabled = (int)ReadF("Enabled", 1);
    c.testMode = (int)ReadF("TestMode", 0);
    c.debug = (int)ReadF("Debug", 0);
    c.upAxis = (int)ReadF("UpAxis", 2);
    if (c.upAxis < 0 || c.upAxis > 2) c.upAxis = 2;
    c.upSign = ReadF("UpSign", 1) < 0 ? -1.0f : 1.0f;
    c.useFogTint = (int)ReadF("UseFogTint", 0);
    c.writeAlpha = (int)ReadF("WriteAlpha", 0);
    c.dome = ReadF("Dome", c.dome);
    c.evolve = ReadF("Evolve", c.evolve);
    c.detail = ReadF("Detail", c.detail);
    c.bandSoft = ReadF("BandSoftness", c.bandSoft);
    c.timeScale = ReadF("TimeScale", c.timeScale);
    c.minLit = ReadF("MinLit", c.minLit);
    c.skyDump = (int)ReadF("SkyDump", 0);
    c.dumpNow = (int)ReadF("DumpNow", 0);
    c.luaBridge = (int)ReadF("LuaBridge", 0);
    c.hideOrig = (int)ReadF("HideOriginalClouds", 0);
    c.colorSource = (int)ReadF("ColorSource", 5);
    c.colorTrace = (int)ReadF("ColorTrace", 0);
    c.layer = (int)ReadF("Layer", 1);
    c.cloudPrims = (int)ReadF("CloudPrims", 372);
    c.skyColorPrims = (int)ReadF("SkyColorPrims", 298);
    c.skyColorDraw = (int)ReadF("SkyColorDraw", 1);
    c.skySampleEvery = (int)ReadF("SkySampleEvery", 12);
    if (c.skySampleEvery < 1) c.skySampleEvery = 1;
    c.tintMix = ReadF("TintMix", c.tintMix);
    c.colorGain = ReadF("ColorGain", c.colorGain);
    c.skyLumNight = ReadF("SkyLumNight", c.skyLumNight);
    c.skyLumDay = ReadF("SkyLumDay", c.skyLumDay);
    {
        char buf[160];
        GetPrivateProfileStringA("Clouds", "CloudDraws", "", buf, sizeof buf, g_iniPath);
        unsigned long long m = 0;
        for (const char* p = buf; *p;) {
            while (*p && (*p < '0' || *p > '9')) p++;
            if (!*p) break;
            int v = atoi(p);
            if (v >= 0 && v < 64) m |= 1ULL << v;
            while (*p >= '0' && *p <= '9') p++;
        }
        c.cloudMask = m;
    }
    c.scale = ReadF("Scale", c.scale);
    c.coverage = ReadF("Coverage", c.coverage);
    c.softness = ReadF("Softness", c.softness);
    c.opacity = ReadF("Opacity", c.opacity);
    c.thrFloor = ReadF("ThrFloor", c.thrFloor);

    // -- Rain transition stuff --
    c.rainCoverage = ReadF("RainCoverage", c.rainCoverage);
    c.rainOpacity = ReadF("RainOpacity", c.rainOpacity);
    c.rainDarken = ReadF("RainDarken", c.rainDarken);
    c.rainBlendSpeed = ReadF("RainBlendSpeed", c.rainBlendSpeed);
    c.zoneBlendSpeed = ReadF("ZoneBlendSpeed", c.zoneBlendSpeed);
    c.zoneDarkenBlendSpeed = ReadF("ZoneDarkenBlendSpeed", c.zoneBlendSpeed);
    c.zoneSpeedBlendSpeed = ReadF("ZoneSpeedBlendSpeed", c.zoneBlendSpeed);
    c.zoneCoverageBlendSpeed = ReadF("ZoneCoverageBlendSpeed", c.zoneBlendSpeed);
    c.zoneTintMixBlendSpeed = ReadF("ZoneTintMixBlendSpeed", c.zoneBlendSpeed);
    c.zoneWeatherBlendSpeedMultBlendSpeed = ReadF("ZoneWeatherBlendSpeedMultBlendSpeed", c.zoneBlendSpeed);
    c.dayTransparencyEnabled = (int)ReadF("DayTransparencyEnabled", (float)c.dayTransparencyEnabled);
    c.dayTransparencyAmount = ReadF("DayTransparencyAmount", c.dayTransparencyAmount);

    c.wxBaselineTau = ReadF("WxBaselineTau", c.wxBaselineTau);
    c.wxDevEnter = ReadF("WxDevEnter", c.wxDevEnter);
    c.wxDevExit = ReadF("WxDevExit", c.wxDevExit);
    c.wxConfirmSec = ReadF("WxConfirmSec", c.wxConfirmSec);
    c.wxJumpRate = ReadF("WxJumpRate", c.wxJumpRate);
    c.wxJumpWindow = ReadF("WxJumpWindow", c.wxJumpWindow);
    c.wxLog = (int)ReadF("WxLog", (float)c.wxLog);
    c.areaLog = (int)ReadF("AreaLog", (float)c.areaLog);
    c.presetLog = (int)ReadF("PresetLog", (float)c.presetLog);
    c.wxUseSlots = (int)ReadF("WxUseSlots", (float)c.wxUseSlots);
    c.wxUsePulse = (int)ReadF("WxUsePulse", (float)c.wxUsePulse);

    c.windX = ReadF("WindX", c.windX);
    c.windY = ReadF("WindY", c.windY);
    c.horizonFade = ReadF("HorizonFade", c.horizonFade);
    c.sunAz = ReadF("SunAzimuth", c.sunAz);
    c.sunEl = ReadF("SunElevation", c.sunEl);
    c.quadDepth = ReadF("QuadDepth", c.quadDepth);
    c.nightBrightness = ReadF("NightBrightness", c.nightBrightness);
    c.renderScale = ReadF("RenderScale", c.renderScale);
    if (c.renderScale > 1.0f) c.renderScale = 1.0f;   // upscaling makes no sense here; clamp instead of silently ignoring a typo
    ReadColor("SunColor", c.sunColor);
    ReadColor("ShadowColor", c.shadowColor);

    // -- Extra cloud layers (depth) --
    // Layer 2: a bit closer/smaller than layer 1 and a bit windier, so it visibly drifts faster.
    c.layer2.scale = 2.2f;
    c.layer2.coverage = 0.35f;
    c.layer2.opacity = 0.90f;
    c.layer2.windX = 0.045f;
    c.layer2.windY = 0.018f;
    c.layer2.noiseSeed = 41.0f;
    // Layer 3: the closest/smallest, light coverage, fastest wind.
    c.layer3.scale = 3.2f;
    c.layer3.coverage = 0.20f;
    c.layer3.opacity = 0.85f;
    c.layer3.windX = 0.065f;
    c.layer3.windY = 0.026f;
    c.layer3.noiseSeed = 97.0f;
    LoadExtraCloudLayer("CloudsLayer2", c.layer2);
    LoadExtraCloudLayer("CloudsLayer3", c.layer3);

    g_cfg = c;
    TunablesClearDirty();   // the file is the source of truth again: in-game edits not saved are dropped
    LogAlways("[config] loaded: Enabled=%d LuaBridge=%d Scale=%g (ini: %s)", c.enabled, c.luaBridge, c.scale, g_iniPath);
    LoadZonePresets();

    if (c.dumpNow) {
        g_dumpWindowActive = true;
        g_dumpWindowUntil = NowSec() + 3.0;
        LogAlways("[DumpNow] triggered: dumping every sky-phase draw call for the next ~3 seconds");
        // Reset it in the file itself, so a later save/hot-reload doesn't re-trigger a dump the
        // person forgot was still set to 1. Done before the mtime check right below, so this
        // write doesn't cause an extra, pointless reload on the next poll.
        WritePrivateProfileStringA("Clouds", "DumpNow", "0", g_iniPath);
    }

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad)) g_iniTime = fad.ftLastWriteTime;
}

static void MaybeReloadConfig() {
    DWORD now = GetTickCount();
    if (now - g_lastIniCheck < 1000) return;
    g_lastIniCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad) &&
        CompareFileTime(&fad.ftLastWriteTime, &g_iniTime) != 0) {
        LoadConfig();
        LogAlways("Config reloaded");
    }
}

// ---------------------------------------------------------------------------------------------
// Shaders (HLSL, they are compiled at runtime with d3dcompiler)
// ---------------------------------------------------------------------------------------------
static const char* VS_SRC = R"HLSL(
float4 C0 : register(c0); // x=1/P11  y=1/P22  z=forward sign w = quad depth
float4 C1 : register(c1); // row 0 of the view matrix (3x3)
float4 C2 : register(c2); // row 1
float4 C3 : register(c3); // row 2
struct VSIn  { float2 p : POSITION; };
struct VSOut { float4 pos : POSITION; float3 dir : TEXCOORD0; };
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.p, C0.w, 1.0);
    float3 dv = float3(i.p.x * C0.x, i.p.y * C0.y, C0.z);
    o.dir = float3(dot(dv, C1.xyz), dot(dv, C2.xyz), dot(dv, C3.xyz));
    return o;
}
)HLSL";

static const char* PS_SRC = R"HLSL(
float4 P0 : register(c0); // x=time y=scale z=threshold w=edge softness
float4 P1 : register(c1); // x=windX y=windY z=opacity w=horizon fade
float4 P2 : register(c2); // xy=displacement towards the sun (noise space) z=test mode w=dome
float4 P3 : register(c3); // "up" vector of the world
float4 P4 : register(c4); // Axis 1 of the cloud plane
float4 P5 : register(c5); // Axis 2 of the cloud plane
float4 P6 : register(c6); // illuminated color
float4 P7 : register(c7); // shadow color
float4 P8 : register(c8); // x = evolution (change in shape) y = detail (weight of fine octaves) z = band smoothness
float4 P9 : register(c9); // xy = per-layer noise offset (keeps extra layers from looking like an exact copy)

float hash12(float2 p) {
    float3 p3 = frac(float3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float vnoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + float2(1.0, 0.0));
    float c = hash12(i + float2(0.0, 1.0));
    float d = hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float2 rot(float2 p) {
    return float2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y) * 2.03 + 17.1;
}
// 5 octaves. Each octave moves at a different speed and direction: the shape of the cloud changes over time,
// it doesn't just slide. dt = weight of the fine octaves (more detail / sharper edges).
float fbm5(float2 p, float t, float ev, float dt) {
    float k = t * ev;
    float s = 0.5 * vnoise(p);
    float2 q = rot(p) + float2(0.7, -0.4) * k;
    s += 0.25 * vnoise(q);
    q = rot(q) + float2(-1.1, 0.9) * k;
    s += 0.125 * vnoise(q);
    q = rot(q) + float2(1.6, 1.3) * k;
    s += 0.0625 * dt * vnoise(q);
    q = rot(q) + float2(-2.1, 1.7) * k;
    s += 0.03125 * dt * vnoise(q);
    return s / (0.875 + 0.09375 * dt);
}
float fbm3(float2 p, float t, float ev) {
    float k = t * ev;
    float s = 0.5 * vnoise(p);
    float2 q = rot(p) + float2(0.7, -0.4) * k;
    s += 0.25 * vnoise(q);
    q = rot(q) + float2(-1.1, 0.9) * k;
    s += 0.125 * vnoise(q);
    return s / 0.875;
}

float4 main(float3 dir : TEXCOORD0) : COLOR {
    if (P2.z > 0.5) return float4(1.0, 0.0, 1.0, 0.7);

    float3 d = normalize(dir);
    float h = dot(d, P3.xyz);
    float horizon = smoothstep(0.02, 0.02 + max(P1.w, 0.01), h);

    // Projection onto a plane of clouds: near the horizon, the clouds appear smaller and more tightly packed.
    float2 uv = float2(dot(d, P4.xyz), dot(d, P5.xyz)) / max(h + P2.w, 0.05);
    float2 p = uv * P0.y + float2(P1.x, P1.y) * P0.x + P9.xy;

    float n = fbm5(p, P0.x, P8.x, P8.y);
    float thr = P0.z;
    float w = max(P0.w, fwidth(n) * 1.25);             // an edge as crisp as the Softness setting dictates, but anti-aliased to ~1 pixel
    float cov = smoothstep(thr, thr + w, n);           // cloud shape
    float thick = saturate((n - thr) / 0.22);          // 0 at the edge, 1 at the core

    // Low-cost lighting: compare the density (thickness) of the clouds relative to the sun. If there is less cloud cover toward the sun, that edge is illuminated.
    float n3 = fbm3(p, P0.x, P8.x);
    float n2 = fbm3(p + P2.xy, P0.x, P8.x);
    float shade = saturate(0.5 + (n3 - n2) * 4.5);
    float light = saturate(shade * 0.7 + (1.0 - thick) * 0.45);

    // Three tones (painted look): bluish shadow -> mid-tone -> luminous white. Bands are more or less distinct depending on BandSoftness.
    float bw = max(P8.z, fwidth(light) * 1.25);
    float3 mid = lerp(P7.rgb, P6.rgb, 0.55);
    float3 col = lerp(P7.rgb, mid, smoothstep(0.42 - bw, 0.42 + bw, light));
    col = lerp(col, P6.rgb, smoothstep(0.65 - bw, 0.65 + bw, light));

    float a = cov * P1.z * horizon;
    return float4(col, a);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
    UINT, UINT, ID3DBlob**, ID3DBlob**);

static IDirect3DDevice9* g_dev;
static IDirect3DVertexShader9* g_vs;
static IDirect3DPixelShader9* g_ps;
static IDirect3DVertexDeclaration9* g_decl;
static IDirect3DStateBlock9* g_sb;
static ID3DBlob* g_vsBlob;
static ID3DBlob* g_psBlob;
static bool                         g_failed;
static bool                         g_inDraw;

static D3DMATRIX g_view, g_proj;
static D3DMATRIX g_camView;         // the last view matrix that is NOT the identity matrix (the one for the sky/world; the game alternates with identity matrices)
static bool      g_haveView, g_haveCamView, g_haveProj, g_persp, g_worldDrawn, g_drawnThisFrame, g_pendingSkyClouds;
static int       g_worldDraws;
static unsigned  g_worldPrims;
static void* g_curRT;           // current render target 0 (used only as an identifier)
static void* g_worldRT;         // render target where the world was drawn in perspective
static int       g_dbgSkipLogs, g_dbgViewLogs, g_dbgAttnLogs;
static DWORD     g_frame;
static DWORD     g_fogLast;
static DWORD     g_ambient;
static float     g_light0[3];
static bool      g_haveFog;
static float     g_fogSmooth[3];
static int       g_dbgTransformLogs;
static int       g_dbgDrawLogs;

static float Smooth(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}

static void ReleaseDeviceResources() {
    if (g_sb) { g_sb->Release();   g_sb = NULL; }
    if (g_vs) { g_vs->Release();   g_vs = NULL; }
    if (g_ps) { g_ps->Release();   g_ps = NULL; }
    if (g_decl) { g_decl->Release(); g_decl = NULL; }
}

// ---- game state shadows (to identify and read sky draw calls) ----
static DWORD g_rs[256];                 // last seen value of each render state
static DWORD g_tss0[32];                // stage 0 texture
static void* g_tex0;                    // stage 0 texture (IDirect3DBaseTexture9*, identifier only)
static DWORD g_fvf;
static IDirect3DVertexBuffer9* g_vb0;
static UINT  g_vb0Off, g_vb0Stride;
static D3DCOLORVALUE g_matDiffuse;
static int   g_skyOrd;
static bool  g_skyEnded;
// Guards the sky-phase cloud trigger (see OnDraw) so it only fires once per frame, even
// though the sky-gradient anchor call (isSkyCol) may be hit multiple times in a row.
static bool  g_skyCloudsTriggeredThisFrame;
// Sheathing/unsheathing a weapon makes the game issue a depth-writing draw (the weapon/model swap)
// BEFORE the sky is drawn in that frame. The first depth-writing draw normally means "terrain
// starts, sky phase over", so that frame's sky was classified as already finished and all its sky
// draws were ignored (clouds not hidden, our clouds not triggered). These two track whether the
// sky gradient has been seen yet, so early depth-writing draws can be tolerated (see OnDraw).
static bool  g_skyGradSeen;
static int   g_skyEarlyZ;
static int   g_dbgDumpFrames;
static bool  g_cloudColorValid;
static float g_cloudColor[3];
static bool  g_skyVValid;               // sky gradient vertex color
static float g_skyVColor[3];

// ---- sampling the color of the already-rendered sky ----
static IDirect3DSurface9* g_smallRT;
static IDirect3DSurface9* g_sysSurf;
static D3DFORMAT g_smallFmt;
static bool  g_skyValid, g_sampleFailed;
static float g_skyColor[3];
static DWORD g_lastSampleFrame;
static int   g_dbgSampleLogs;

static void ReleaseSampleBuffers() {
    if (g_smallRT) { g_smallRT->Release(); g_smallRT = NULL; }
    if (g_sysSurf) { g_sysSurf->Release(); g_sysSurf = NULL; }
    g_smallFmt = D3DFMT_UNKNOWN;
}

// ---- offscreen buffer for RenderScale < 1 (see DrawCloudLayersDownsampled) ----
static IDirect3DSurface9* g_cloudRT;
static UINT      g_cloudRTW, g_cloudRTH;
static D3DFORMAT g_cloudRTFmt = D3DFMT_UNKNOWN;

static void ReleaseCloudRT() {
    if (g_cloudRT) { g_cloudRT->Release(); g_cloudRT = NULL; }
    g_cloudRTW = g_cloudRTH = 0;
    g_cloudRTFmt = D3DFMT_UNKNOWN;
}

// Creates (or recreates, if the size/format changed since last frame) the small offscreen buffer
// the downsampled cloud path renders into.
static bool EnsureCloudRT(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt) {
    if (g_cloudRT && g_cloudRTW == w && g_cloudRTH == h && g_cloudRTFmt == fmt) return true;
    ReleaseCloudRT();
    if (FAILED(dev->CreateRenderTarget(w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_cloudRT, NULL))) return false;
    g_cloudRTW = w; g_cloudRTH = h; g_cloudRTFmt = fmt;
    return true;
}

static void SampleSky(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DSURFACE_DESC& rd) {
    if (g_sampleFailed) return;
    if (g_skyValid && g_frame - g_lastSampleFrame < (DWORD)g_cfg.skySampleEvery) return;
    // The top strip is only sky if the camera is facing the horizon or looking upwards (column 3 of the view = camera direction).
    float fwdUp = (g_cfg.upAxis == 0 ? g_camView._13 : (g_cfg.upAxis == 1 ? g_camView._23 : g_camView._33)) * g_cfg.upSign;
    if (fwdUp < 0.0f) return;
    if (rd.MultiSampleType != D3DMULTISAMPLE_NONE || (rd.Format != D3DFMT_A8R8G8B8 && rd.Format != D3DFMT_X8R8G8B8)) {
        LogAlways("Unsupported sky sampling (format=%u, multisample=%u); fixed colors are used", (unsigned)rd.Format, (unsigned)rd.MultiSampleType);
        g_sampleFailed = true;
        return;
    }
    if (!g_smallRT || g_smallFmt != rd.Format) {
        ReleaseSampleBuffers();
        if (FAILED(dev->CreateRenderTarget(8, 8, rd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_smallRT, NULL)) ||
            FAILED(dev->CreateOffscreenPlainSurface(8, 8, rd.Format, D3DPOOL_SYSTEMMEM, &g_sysSurf, NULL))) {
            LogAlways("The sampling surfaces could not be created.; fixed colors are used");
            ReleaseSampleBuffers();
            g_sampleFailed = true;
            return;
        }
        g_smallFmt = rd.Format;
    }
    RECT src;
    src.left = (LONG)(rd.Width * 0.2f);
    src.right = (LONG)(rd.Width * 0.8f);
    src.top = 0;
    src.bottom = (LONG)(rd.Height * 0.18f);
    D3DLOCKED_RECT lr;
    if (FAILED(dev->StretchRect(rt, &src, g_smallRT, NULL, D3DTEXF_LINEAR)) || FAILED(dev->GetRenderTargetData(g_smallRT, g_sysSurf)) ||
        FAILED(g_sysSurf->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
        LogAlways("Sky sampling failed (StretchRect/GetRenderTargetData/LockRect); fixed colors are used");
        g_sampleFailed = true;
        return;
    }
    float sum[3] = { 0, 0, 0 };
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const BYTE* px = (const BYTE*)lr.pBits + y * lr.Pitch + x * 4;
            sum[0] += px[2]; sum[1] += px[1]; sum[2] += px[0];
        }
    g_sysSurf->UnlockRect();
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = sum[i] / (64.0f * 255.0f);
    for (int i = 0; i < 3; i++) g_skyColor[i] = g_skyValid ? g_skyColor[i] + (c[i] - g_skyColor[i]) * 0.3f : c[i];
    g_skyValid = true;
    g_lastSampleFrame = g_frame;
    if (g_dbgSampleLogs < 12) {
        g_dbgSampleLogs++;
        LogDbg("[frame %lu] sampled sky=(%.3f %.3f %.3f) smoothed=(%.3f %.3f %.3f) luminance=%.3f", g_frame, c[0], c[1], c[2],
            g_skyColor[0], g_skyColor[1], g_skyColor[2], 0.299f * g_skyColor[0] + 0.587f * g_skyColor[1] + 0.114f * g_skyColor[2]);
    }
}

static bool CompileOne(D3DCompile_t fn, const char* src, const char* target, ID3DBlob** out) {
    ID3DBlob* err = NULL;
    HRESULT hr = fn(src, strlen(src), NULL, NULL, NULL, "main", target, (1 << 15) /*OPTIMIZATION_LEVEL3*/, 0, out, &err);
    if (FAILED(hr)) {
        LogAlways("Error compiling %s: %s", target, err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

static bool CompileShaders() {
    if (g_vsBlob && g_psBlob) return true;
    HMODULE h = LoadLibraryA("d3dcompiler_47.dll");
    if (!h) h = LoadLibraryA("d3dcompiler_43.dll");
    if (!h) { LogAlways("d3dcompiler_47.dll and d3dcompiler_43.dll were not found."); return false; }
    D3DCompile_t fn = (D3DCompile_t)GetProcAddress(h, "D3DCompile");
    if (!fn) { LogAlways("D3DCompile not found"); return false; }
    if (!CompileOne(fn, VS_SRC, "vs_3_0", &g_vsBlob)) return false;
    if (!CompileOne(fn, PS_SRC, "ps_3_0", &g_psBlob)) return false;
    return true;
}

static bool EnsureResources(IDirect3DDevice9* dev) {
    if (dev != g_dev) {            // new device
        ReleaseDeviceResources();
        ReleaseSampleBuffers();
        g_sampleFailed = false;
        g_skyValid = false;
        g_dev = dev;
    }
    if (g_vs && g_ps && g_decl) return true;

    D3DCAPS9 caps;
    if (FAILED(dev->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0) ||
        caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
        LogAlways("The GPU/device does not support Shader Model 3.0; effect disabled");
        g_failed = true;
        return false;
    }
    if (!CompileShaders()) { g_failed = true; return false; }

    if (FAILED(dev->CreateVertexShader((const DWORD*)g_vsBlob->GetBufferPointer(), &g_vs)) ||
        FAILED(dev->CreatePixelShader((const DWORD*)g_psBlob->GetBufferPointer(), &g_ps))) {
        LogAlways("CreateVertexShader/CreatePixelShader failed");
        ReleaseDeviceResources();
        g_failed = true;
        return false;
    }
    const D3DVERTEXELEMENT9 elems[] = { { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                                        D3DDECL_END() };
    if (FAILED(dev->CreateVertexDeclaration(elems, &g_decl))) {
        LogAlways("CreateVertexDeclaration failed");
        ReleaseDeviceResources();
        g_failed = true;
        return false;
    }
    LogAlways("Shaders created successfully");
    return true;
}

static void ComputeColors(float lit[3], float sh[3]) {
    // Base: fixed colors from the .ini file
    for (int i = 0; i < 3; i++) { lit[i] = g_cfg.sunColor[i]; sh[i] = g_cfg.shadowColor[i]; }

    float msh = g_cfg.shadowColor[0] > g_cfg.shadowColor[1] ? g_cfg.shadowColor[0] : g_cfg.shadowColor[1];
    if (g_cfg.shadowColor[2] > msh) msh = g_cfg.shadowColor[2];
    if (msh < 1e-3f) msh = 1e-3f;

    const float* T = NULL;   //Sky-blue shade for tint and shine
    bool cloudMode = g_cfg.colorSource >= 2 && g_cfg.colorSource <= 4 && g_cloudColorValid;
    if (!cloudMode) {
        if (g_cfg.colorSource == 5 && g_skyVValid) T = g_skyVColor;        // sky gradient vertices
        else if (g_cfg.colorSource != 0 && g_skyValid) T = g_skyColor;     // screen sampling (backup)
    }

    if (T) {
        float lum = 0.299f * T[0] + 0.587f * T[1] + 0.114f * T[2];
        float m = T[0] > T[1] ? T[0] : T[1];
        if (T[2] > m) m = T[2];
        if (m < 1e-3f) m = 1e-3f;
        float nb = g_cfg.nightBrightness;
        float bright = nb + (1.0f - nb) * Smooth(g_cfg.skyLumNight, g_cfg.skyLumDay, lum);
        float mix = g_zoneTintMix;   // smoothed, per-zone override of TintMix (falls back to g_cfg.tintMix outside any preset)
        for (int i = 0; i < 3; i++) {
            float tn = T[i] / m;
            lit[i] = (g_cfg.sunColor[i] * (1.0f - mix) + tn * mix) * bright;
            sh[i] = (g_cfg.shadowColor[i] * (1.0f - mix) + tn * 0.7f * mix) * bright;
            float fl = g_cfg.minLit * g_cfg.shadowColor[i] / msh;   // bluish ground: dark gray at night, not black
            if (lit[i] < fl) lit[i] = fl;
            if (sh[i] < fl * 0.6f) sh[i] = fl * 0.6f;
        }
    }
    else if (cloudMode) {
        // Color taken from the draw call of the original clouds.
        for (int i = 0; i < 3; i++) {
            float v = g_cloudColor[i] * g_cfg.colorGain;
            float fl = g_cfg.minLit * g_cfg.shadowColor[i] / msh;
            if (v < fl) v = fl;
            lit[i] = v > 1.0f ? 1.0f : v;
            sh[i] = lit[i] * g_cfg.shadowColor[i];
        }
    }
    // dk combines the rain darkening (g_rainBlend) with that of the active zone preset
    // (g_zoneDarken); both use the same units (1.0 = no change), so they are multiplied.
    float dk = (1.0f - (1.0f - g_cfg.rainDarken) * g_rainBlend) * g_zoneDarken;
    for (int i = 0; i < 3; i++) { lit[i] *= dk; sh[i] *= dk; }
}

static bool NeedScreenSample() {
    int src = g_cfg.colorSource;
    if (src == 0) return false;
    if (src == 1) return true;
    if (src <= 4) return !g_cloudColorValid;
    return !g_skyVValid;
}

// Draws a single cloud layer. lc holds that layer's appearance (scale/coverage/wind/etc.); t is its
// already-computed animation time (see DrawAllCloudLayers for how layer 1 vs. 2/3 compute it).
// Global rendering setup that isn't a per-layer aesthetic choice (sun direction, test mode, quad
// depth, up axis, write-alpha, the coverage/threshold curve shape) stays shared across all layers.
// offscreenTarget is set by DrawCloudLayersDownsampled when the active render target is our own
// smaller buffer rather than the real back buffer -- in that case the usual "must match the back
// buffer's size" sanity check doesn't apply (that's the whole point of rendering smaller).
// Captures the current device state into g_sb, for callers that already know g_sb exists (the
// very first creation always happens inline in DrawClouds instead -- see manageState below -- so
// that the first-ever draw of the mod's lifetime keeps exactly the timing/context that was already
// tested and working; only later frames, once g_sb already exists, use this shared path).
// This is the fixed per-call overhead the RenderScale test pointed at: temperature didn't change
// between 0.1 and 1.0, which rules out the per-pixel shader cost and points at something that
// happens the same number of times regardless of resolution, like a Capture()/Apply() pair.
static void CaptureStateBlock() {
    if (g_sb) g_sb->Capture();
}

// manageState: true (the default) makes this call create/capture state before drawing and restore
// it after, exactly as before this optimization -- so any future standalone caller, and the very
// first draw of the mod's lifetime (when g_sb doesn't exist yet), keep working unchanged. The
// 3-layer callers (DrawAllCloudLayers / DrawCloudLayersDownsampled), once g_sb already exists from
// an earlier frame, instead capture once themselves (CaptureStateBlock) and pass false here for
// every layer, restoring once after the last one.
static bool DrawClouds(IDirect3DDevice9* dev, bool atSky, const CloudLayerConfig& lc, float t, bool offscreenTarget = false, bool manageState = true) {
    if (!g_cfg.enabled || g_failed) return false;
    if (!g_haveCamView || !g_haveProj) return false;
    if (!EnsureResources(dev)) return false;

    // Normally we only draw if the current render target has the same size as the back buffer and a
    // depth buffer exists. When offscreenTarget is true, the size check is skipped on purpose.
    IDirect3DSurface9* rt = NULL, * bb = NULL, * zs = NULL;
    D3DSURFACE_DESC rd, bd;
    memset(&rd, 0, sizeof rd);
    memset(&bd, 0, sizeof bd);
    bool gotRT = SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt;
    bool gotBB = SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb;
    if (gotRT) rt->GetDesc(&rd);
    if (gotBB) bb->GetDesc(&bd);
    HRESULT zr = dev->GetDepthStencilSurface(&zs);
    bool isBB = (rt == bb);
    bool ok = gotRT && SUCCEEDED(zr) && zs &&
        (offscreenTarget || (gotBB && rd.Width == bd.Width && rd.Height == bd.Height));
    if (ok && !atSky && NeedScreenSample()) SampleSky(dev, rt, rd);
    if (rt) rt->Release();
    if (bb) bb->Release();
    if (zs) zs->Release();
    if (!ok) {
        if (g_dbgSkipLogs < 20) {
            g_dbgSkipLogs++;
            LogDbg("[frame %lu] DrawClouds skipped: rt=%ux%u bb=%ux%u isBackbuffer=%d zbuffer=0x%08lX", g_frame,
                (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)bd.Width, (unsigned)bd.Height, (int)isBB, (unsigned long)zr);
        }
        return false;
    }

    // The very first creation of g_sb always happens right here, exactly as before this
    // optimization existed -- this keeps that one-time, first-frame-of-the-mod's-life code path
    // byte-for-byte the same as what was already tested and working. Only once g_sb already exists
    // does manageState start to matter: true captures again here (the old per-layer behavior,
    // still used for layer 1's very first call and for any future standalone caller); false skips
    // it because the caller (DrawAllCloudLayers / DrawCloudLayersDownsampled) already captured once
    // for every layer in this pass via CaptureStateBlock().
    if (!g_sb) {
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) { g_sb = NULL; LogAlways("CreateStateBlock failed"); g_failed = true; return false; }
    }
    else if (manageState) {
        g_sb->Capture();
    }

    g_inDraw = true;

    // --- vertex shader constants ---
    float invP11 = fabsf(g_proj._11) > 1e-6f ? 1.0f / g_proj._11 : 1.0f;
    float invP22 = fabsf(g_proj._22) > 1e-6f ? 1.0f / g_proj._22 : 1.0f;
    float fwd = g_proj._34 >= 0 ? 1.0f : -1.0f;
    float vsc[16] = {
        invP11, invP22, fwd, g_cfg.quadDepth,
        g_camView._11, g_camView._12, g_camView._13, 0,
        g_camView._21, g_camView._22, g_camView._23, 0,
        g_camView._31, g_camView._32, g_camView._33, 0,
    };

    // --- pixel shader constants ---
    int a = g_cfg.upAxis, b = (a + 1) % 3, c = (a + 2) % 3;
    float up[4] = { 0, 0, 0, 0 }, e1[4] = { 0, 0, 0, 0 }, e2[4] = { 0, 0, 0, 0 };
    up[a] = g_cfg.upSign; e1[b] = 1; e2[c] = 1;
    const float deg = 0.01745329252f;
    float az = g_cfg.sunAz * deg, el = g_cfg.sunEl * deg;
    float sunLen = 0.05f + 0.13f * (1.0f - sinf(el));
    //float cov = lc.coverage < 0 ? 0 : (lc.coverage > 1 ? 1 : lc.coverage);

    // -- Rain transition stuff start --
    // Where this layer's coverage/opacity go while weather is active. A layer with its own
    // RainCoverage/RainOpacity (>= 0) goes straight to that value. Otherwise it inherits the global
    // weather shift, applied as the same absolute change layer 1 gets (global RainX minus layer 1's
    // base), so layers without their own setting still thicken/clear together.
    float rainCovTarget = (lc.rainCoverage >= 0.0f) ? lc.rainCoverage : lc.coverage + (g_cfg.rainCoverage - g_cfg.coverage);
    float rainOpaTarget = (lc.rainOpacity >= 0.0f) ? lc.rainOpacity : lc.opacity + (g_cfg.rainOpacity - g_cfg.opacity);
    float covBase = lc.coverage + (rainCovTarget - lc.coverage) * g_rainBlend + g_zoneCoverageAdd;
    float cov = covBase < 0 ? 0 : (covBase > 1 ? 1 : covBase);
    float opaBase = lc.opacity + (rainOpaTarget - lc.opacity) * g_rainBlend;
    // -- Rain transition stuff end --

    // Clouds on clear days are slightly more transparent. The factor cancels itself out (returning to 1—i.e., 
    // normal opacity) at night (g_dayness->0) and during any active weather (g_rainBlend->1), so it never 
    // conflicts with the weather effect mentioned above.
    float dayTransparencyFactor = 1.0f - g_cfg.dayTransparencyAmount * g_dayness * (1.0f - g_rainBlend);
    opaBase *= dayTransparencyFactor;

    // (Non-linear) curve instead of linear interpolation: for a given intermediate "cov" value (e.g., the
    // default Coverage=0.55, without rain), the threshold remains practically the same as before, but as
    // it approaches cov=1.0 (RainCoverage=1.0 with active rain), it drops to g_cfg.thrFloor—which, by
    // default, is lower than the actual noise minimum—ensuring there are no gaps of clear sky. 
    // Previously, the floor was fixed at 0.36, a level the fbm5 noise frequently reaches.
    float thr = 0.74f + (g_cfg.thrFloor - 0.74f) * powf(cov, 2.5f);
    float lit[3], sh[3];
    ComputeColors(lit, sh);
    // Arbitrary (decorrelating) multipliers -- not meant to mean anything, just spread NoiseSeed
    // into a 2D offset far enough that different layers don't sample overlapping noise. NoiseSeed=0
    // (layer 1's case) gives offset (0,0), i.e. the exact same noise field as before this feature.
    float noiseOffX = lc.noiseSeed * 12.9898f;
    float noiseOffY = lc.noiseSeed * 78.233f;
    float psc[40] = {
        t, lc.scale, thr, lc.softness < 0.01f ? 0.01f : lc.softness,
        lc.windX, lc.windY, opaBase, lc.horizonFade,
        cosf(az) * sunLen, sinf(az) * sunLen, g_cfg.testMode ? 1.0f : 0.0f, lc.dome,
        up[0], up[1], up[2], 0,
        e1[0], e1[1], e1[2], 0,
        e2[0], e2[1], e2[2], 0,
        lit[0], lit[1], lit[2], 1,
        sh[0], sh[1], sh[2], 1,
        lc.evolve, lc.detail, lc.bandSoft < 0.005f ? 0.005f : lc.bandSoft, 0,
        noiseOffX, noiseOffY, 0, 0,
    };

    // --- states ---
    // Nothing has been drawn on the sky layer yet, except for the gradient and the celestial bodies; without a depth test, the terrain that follows would obscure them.
    dev->SetRenderState(D3DRS_ZENABLE, atSky ? FALSE : TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, g_cfg.writeAlpha ? 0xF : 0x7);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);

    dev->SetVertexDeclaration(g_decl);
    dev->SetVertexShader(g_vs);
    dev->SetPixelShader(g_ps);
    dev->SetVertexShaderConstantF(0, vsc, 4);
    dev->SetPixelShaderConstantF(0, psc, 10);

    static const float quad[8] = { -1, -1, -1, 1, 1, -1, 1, 1 };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 2 * sizeof(float));

    if (manageState) g_sb->Apply();
    g_inDraw = false;

    if (g_dbgDrawLogs < 20) {
        g_dbgDrawLogs++;
        LogDbg("[frame %lu] Clouds drawn (%s) on rt %ux%u (isBackbuffer=%d) fog=%06lX ambient=%08lX light0=(%.2f %.2f %.2f) lit=(%.2f %.2f %.2f) shadow=(%.2f %.2f %.2f)",
            g_frame, atSky ? "sky layer" : "end of the world", (unsigned)rd.Width, (unsigned)rd.Height, (int)isBB, g_fogLast, g_ambient,
            g_light0[0], g_light0[1], g_light0[2], lit[0], lit[1], lit[2], sh[0], sh[1], sh[2]);
    }
    // Uncapped (unlike the log right above, which stops after 20 total): compares the color used on
    // the "end of the world" fallback frames against the normal sky-layer frames around them, to see
    // whether the fallback is drawing with stale/jumped color -- the current suspect for the
    // sheath/unsheath flicker, now that we've confirmed (via TryDraw's own log) that the fallback
    // always succeeds in actually drawing something on the zero-sky-calls frames.
    LogDbg("[colorcheck frame %lu] (%s) colorValid=%d lit=(%.3f %.3f %.3f) shadow=(%.3f %.3f %.3f)",
        g_frame, atSky ? "sky" : "fallback", (int)g_cloudColorValid, lit[0], lit[1], lit[2], sh[0], sh[1], sh[2]);
    return true;
}

// Builds a CloudLayerConfig from layer 1's legacy top-level [Clouds] fields, so DrawClouds can be
// called the same way for all 3 layers. NoiseSeed=0 here on purpose: layer 1 must keep sampling
// the exact same (unshifted) noise field it always has.
static CloudLayerConfig Layer1Params() {
    CloudLayerConfig lc;
    lc.enabled = g_cfg.enabled;
    lc.scale = g_cfg.scale;
    lc.coverage = g_cfg.coverage;
    lc.softness = g_cfg.softness;
    lc.opacity = g_cfg.opacity;
    lc.windX = g_cfg.windX;
    lc.windY = g_cfg.windY;
    lc.horizonFade = g_cfg.horizonFade;
    lc.dome = g_cfg.dome;
    lc.evolve = g_cfg.evolve;
    lc.detail = g_cfg.detail;
    lc.bandSoft = g_cfg.bandSoft;
    lc.timeScale = g_cfg.timeScale;   // unused here (layer 1 uses g_animTime directly below), kept for clarity
    lc.noiseSeed = 0.0f;
    lc.rainCoverage = g_cfg.rainCoverage;   // layer 1's weather targets are the global ones
    lc.rainOpacity = g_cfg.rainOpacity;
    return lc;
}

// RenderScale < 1 path: copies the current (pre-cloud) render target content down into a smaller
// offscreen buffer, draws all 3 layers into that buffer (so the noise shader runs on far fewer
// pixels), then stretches the result back onto the real target. This is what actually cuts GPU
// load -- unlike the per-draw-call state-block overhead, which is unrelated to resolution.
// Only used for the sky-phase draw (atSky==true), where ZENABLE=FALSE means there's no depth
// buffer to keep matched to a smaller target; the caller never requests this for atSky==false.
// Side effect worth knowing: the down+up round trip slightly blurs the WHOLE sky area it covers
// (not just the cloud shapes themselves), since the background gets downsampled along with them.
static bool DrawCloudLayersDownsampled(IDirect3DDevice9* dev, float t1, float t2, float t3) {
    IDirect3DSurface9* rt = NULL;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC rd;
    rt->GetDesc(&rd);
    if (rd.MultiSampleType != D3DMULTISAMPLE_NONE) { rt->Release(); return false; }   // StretchRect between MSAA and non-MSAA surfaces isn't reliable; skip downsampling this frame, draw full-res instead (see caller)

    UINT w = (UINT)(rd.Width * g_cfg.renderScale);
    UINT h = (UINT)(rd.Height * g_cfg.renderScale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (!EnsureCloudRT(dev, w, h, rd.Format)) { rt->Release(); return false; }

    // Copy the not-yet-cloud background down, so the layers' alpha blend composites against the
    // real sky color instead of whatever was left in the buffer from a previous frame.
    if (FAILED(dev->StretchRect(rt, NULL, g_cloudRT, NULL, D3DTEXF_LINEAR))) { rt->Release(); return false; }
    if (FAILED(dev->SetRenderTarget(0, g_cloudRT))) { rt->Release(); return false; }

    // See the comment on CaptureStateBlock/manageState above DrawClouds: if g_sb doesn't exist yet,
    // leave its first creation (and implicit initial capture) to layer 1's own DrawClouds call below.
    CaptureStateBlock();

    bool any = false;
    if (DrawClouds(dev, true, Layer1Params(), t1, true, false)) any = true;
    if (g_cfg.layer2.enabled && DrawClouds(dev, true, g_cfg.layer2, t2, true, false)) any = true;
    if (g_cfg.layer3.enabled && DrawClouds(dev, true, g_cfg.layer3, t3, true, false)) any = true;
    if (g_sb) g_sb->Apply();

    dev->SetRenderTarget(0, rt);   // also resets the viewport back to the real target's full size
    if (any) dev->StretchRect(g_cloudRT, NULL, rt, NULL, D3DTEXF_LINEAR);

    rt->Release();
    return any;
}

// Draws every enabled cloud layer, farthest (layer 1) first and nearest (layer 3) last, so each
// later layer paints over the ones before it where they overlap -- the same painter's-order trick
// already used elsewhere in this file (see the sky-phase trigger notes near isSkyCol) to keep our
// clouds correctly behind whatever is supposed to be in front of them.
static bool DrawAllCloudLayers(IDirect3DDevice9* dev, bool atSky) {
    float t1 = (float)fmod(g_animTime, 36000.0);
    float t2 = (float)fmod(g_animTimeRaw * g_cfg.layer2.timeScale, 36000.0);
    float t3 = (float)fmod(g_animTimeRaw * g_cfg.layer3.timeScale, 36000.0);

    if (atSky && g_cfg.renderScale < 0.999f && g_cfg.renderScale > 0.0f) {
        if (DrawCloudLayersDownsampled(dev, t1, t2, t3)) return true;
        // If the downsample path bailed out for some reason (MSAA target, surface creation
        // failure, etc.), fall through and draw full-res instead of silently losing the clouds.
    }

    // One Capture()/Apply() for however many of the 3 layers actually draw this frame, instead of
    // one pair per layer (see CaptureStateBlock for why this is the change that should actually
    // move the needle on GPU temperature/CPU overhead, unlike RenderScale). If g_sb doesn't exist
    // yet (the very first draw of the mod's lifetime), leave its creation to layer 1's own
    // DrawClouds call below, at the exact point it has always happened.
    if (!g_cfg.enabled || g_failed) return false;
    CaptureStateBlock();

    bool any = false;
    if (DrawClouds(dev, atSky, Layer1Params(), t1, false, false)) any = true;
    if (g_cfg.layer2.enabled && DrawClouds(dev, atSky, g_cfg.layer2, t2, false, false)) any = true;
    if (g_cfg.layer3.enabled && DrawClouds(dev, atSky, g_cfg.layer3, t3, false, false)) any = true;

    if (g_sb) g_sb->Apply();
    return any;
}

// ---------------------------------------------------------------------------------------------
// IDirect3DDevice9 Hooks (vtable patching)
// ---------------------------------------------------------------------------------------------
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* SetLight_t)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*);
typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* SetTSS_t)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetFVF_t)(IDirect3DDevice9*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetVertexDecl_t)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
typedef HRESULT(STDMETHODCALLTYPE* SetStreamSource_t)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* SetMaterial_t)(IDirect3DDevice9*, const D3DMATERIAL9*);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

struct VT {
    void** vtbl;
    Reset_t         reset;
    Present_t       present;
    SetTransform_t  setTransform;
    SetRenderState_t setRenderState;
    SetRenderTarget_t setRenderTarget;
    SetLight_t setLight;
    SetTexture_t setTexture;
    SetTSS_t setTSS;
    SetFVF_t setFVF;
    SetVertexDecl_t setVertexDecl;
    SetStreamSource_t setStreamSource;
    SetMaterial_t setMaterial;
    DrawPrimitive_t drawPrimitive;
    DrawIndexedPrimitive_t drawIndexedPrimitive;
    DrawPrimitiveUP_t drawPrimitiveUP;
    DrawIndexedPrimitiveUP_t drawIndexedPrimitiveUP;
};
static VT  g_vts[4];
static int g_nvt;
static CreateDevice_t g_origCreateDevice;

static VT* FindVT(IDirect3DDevice9* d) {
    void** vt = *(void***)d;
    for (int i = 0; i < g_nvt; i++) if (g_vts[i].vtbl == vt) return &g_vts[i];
    return g_nvt ? &g_vts[0] : NULL;
}

static void* PatchVTable(void** vtbl, int idx, void* hook) {
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    void* orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[idx], sizeof(void*));
    return orig;
}

static HRESULT STDMETHODCALLTYPE H_SetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v) {
    VT* t = FindVT(d);
    if (!g_inDraw && s == D3DRS_FOGCOLOR && (v & 0xFFFFFF)) { g_fogLast = v & 0xFFFFFF; g_haveFog = true; }
    if (!g_inDraw && s == D3DRS_AMBIENT) g_ambient = v;
    if (!g_inDraw && (unsigned)s < 256) g_rs[(unsigned)s] = v;
    return t->setRenderState(d, s, v);
}

static void* CurRT(IDirect3DDevice9* d) {
    if (!g_curRT) {
        IDirect3DSurface9* sfc = NULL;
        if (SUCCEEDED(d->GetRenderTarget(0, &sfc)) && sfc) { g_curRT = sfc; sfc->Release(); }
    }
    return g_curRT;
}

// Draw the clouds if the world has already been drawn in perspective and the current render target is the world's.
static void TryDraw(IDirect3DDevice9* d) {
    // Diagnostic for the "zero sky-phase calls" frames found via DumpNow: shows whether this
    // end-of-frame fallback actually fires on those frames, and if not, which check stopped it.
    LogDbg("[frame %lu] TryDraw called: worldDrawn=%d drawnThisFrame=%d haveCamView=%d haveProj=%d rtMatch=%d",
        g_frame, (int)g_worldDrawn, (int)g_drawnThisFrame, (int)g_haveCamView, (int)g_haveProj, (int)(CurRT(d) == g_worldRT));
    if (!g_worldDrawn || g_drawnThisFrame || !g_haveCamView || !g_haveProj) return;
    if (CurRT(d) != g_worldRT) return;
    // This fallback paints our clouds with depth testing at the END of the world pass. If it fires
    // while the sky phase is still open (e.g. a model draw before the sky set g_worldDrawn, then the
    // game switched render targets), it would paint the clouds BEFORE the sky is drawn, the sky then
    // covers them, and the sky-phase injection is skipped for the frame because g_drawnThisFrame is
    // already set -- i.e. the clouds vanish for a frame and the stars show through.
    if (!g_skyEnded) {
        if (g_dumpWindowActive && NowSec() < g_dumpWindowUntil)
            LogAlways("[trydraw f%lu] skipped: sky phase not finished yet (gradSeen=%d earlyZ=%d)", g_frame, (int)g_skyGradSeen, g_skyEarlyZ);
        return;
    }
    if (g_dumpWindowActive && NowSec() < g_dumpWindowUntil)
        LogAlways("[trydraw f%lu] fallback drawing clouds (skyEnded=1)", g_frame);
    bool drew = DrawAllCloudLayers(d, false);
    LogDbg("[frame %lu] TryDraw fallback drew clouds = %d", g_frame, (int)drew);
    if (drew) g_drawnThisFrame = true;
}

static float RsF(DWORD v) { float f; memcpy(&f, &v, sizeof f); return f; }

// Fog status and global counters, to study how conditions change with the weather (rain, snow, sandstorm).
static void LogWeather(const char* tag) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    LogAlways("[weather f%lu %02d:%02d:%02d %s] fog: on=%lu color=%06lX table=%lu vertex=%lu start=%g end=%g density=%g | ambient=%08lX | world_calls=%d world_triangles=%u | sky=(%.3f %.3f %.3f)",
        g_frame, st.wHour, st.wMinute, st.wSecond, tag,
        (unsigned long)g_rs[D3DRS_FOGENABLE], (unsigned long)(g_rs[D3DRS_FOGCOLOR] & 0xFFFFFF), (unsigned long)g_rs[D3DRS_FOGTABLEMODE],
        (unsigned long)g_rs[D3DRS_FOGVERTEXMODE], RsF(g_rs[D3DRS_FOGSTART]), RsF(g_rs[D3DRS_FOGEND]), RsF(g_rs[D3DRS_FOGDENSITY]),
        (unsigned long)g_rs[D3DRS_AMBIENT], g_worldDraws, g_worldPrims, g_skyVColor[0], g_skyVColor[1], g_skyVColor[2]);
}

struct DrawInfo { const char* kind; UINT prims; UINT first; UINT count; const BYTE* up; UINT upStride; };

static int FvfDiffuseOffset(DWORD fvf) {
    int off;
    DWORD pos = fvf & D3DFVF_POSITION_MASK;
    if (pos == D3DFVF_XYZ) off = 12;
    else if (pos == D3DFVF_XYZRHW) off = 16;
    else return -1;
    if (fvf & D3DFVF_NORMAL) off += 12;
    if (fvf & D3DFVF_PSIZE) off += 4;
    if (!(fvf & D3DFVF_DIFFUSE)) return -1;
    return off;
}

// The client uses vertex declarations (not FVF): we look for where COLOR0 is located in stream 0.
struct DeclCacheEntry { void* decl; int colorOff; };
static DeclCacheEntry g_declCache[48];
static int g_declCacheN;
static int g_declColorOff = -1;

static int LookupDeclColorOffset(IDirect3DVertexDeclaration9* decl) {
    for (int i = 0; i < g_declCacheN; i++)
        if (g_declCache[i].decl == (void*)decl) return g_declCache[i].colorOff;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    int off = -1;
    if (SUCCEEDED(decl->GetDeclaration(el, &n))) {
        for (UINT i = 0; i < n; i++) {
            if (el[i].Stream == 0xFF) break;   // D3DDECL_END
            if (el[i].Stream == 0 && el[i].Usage == D3DDECLUSAGE_COLOR && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_D3DCOLOR) {
                off = el[i].Offset;
                break;
            }
        }
    }
    if (g_declCacheN < 48) { g_declCache[g_declCacheN].decl = (void*)decl; g_declCache[g_declCacheN].colorOff = off; g_declCacheN++; }
    return off;
}

static int DiffuseOffset() { return g_fvf ? FvfDiffuseOffset(g_fvf) : g_declColorOff; }

// Average diffuse color of the first vertices of the call (if the format supports it and it can be read).
static bool ReadVertexColor(const DrawInfo& di, float rgb[3], float* alpha) {
    int off = DiffuseOffset();
    if (off < 0) return false;
    const BYTE* base = NULL;
    UINT stride = 0, count = di.count ? di.count : 1;
    bool locked = false;
    if (di.up) { base = di.up; stride = di.upStride; }
    else {
        if (!g_vb0 || !g_vb0Stride) return false;
        stride = g_vb0Stride;
        D3DVERTEXBUFFER_DESC vd;
        if (FAILED(g_vb0->GetDesc(&vd))) return false;
        UINT start = g_vb0Off + di.first * stride;
        if (start + stride > vd.Size) return false;
        if (start + count * stride > vd.Size) count = (vd.Size - start) / stride;
        void* p = NULL;
        if (FAILED(g_vb0->Lock(start, count * stride, &p, D3DLOCK_READONLY)) || !p) return false;
        base = (const BYTE*)p;
        locked = true;
    }
    bool ok = stride >= (UINT)off + 4;
    if (ok) {
        float sr = 0, sg = 0, sb = 0, sa = 0;
        UINT step = count > 48 ? count / 48 : 1, used = 0;   // sample distributed throughout the dome, not just at the beginning
        for (UINT i = 0; i < count; i += step, used++) {
            DWORD c = *(const DWORD*)(base + i * stride + off);
            sa += (float)((c >> 24) & 255); sr += (float)((c >> 16) & 255); sg += (float)((c >> 8) & 255); sb += (float)(c & 255);
        }
        float k = 1.0f / (255.0f * (float)(used ? used : 1));
        rgb[0] = sr * k; rgb[1] = sg * k; rgb[2] = sb * k;
        if (alpha) *alpha = sa * k;
    }
    if (locked) g_vb0->Unlock();
    return ok;
}

// It is called before each draw call. It returns true if the call should be skipped (original clouds hidden).
static bool OnDraw(IDirect3DDevice9* d, const DrawInfo& di) {
    if (g_inDraw) return false;
    // Diagnostic (DumpNow window only): every sky-gradient / cloud-dome sized draw, with the state
    // flags OnDraw uses to decide whether to look at it. The "0 calls" frames show NO sky draws
    // before the world starts, so this tells us where in the frame they actually happen.
    if (g_dumpWindowActive && NowSec() < g_dumpWindowUntil &&
        (di.prims == (UINT)g_cfg.skyColorPrims || di.prims == (UINT)g_cfg.cloudPrims))
        LogAlways("[skydraw f%lu] %s prims=%u tex=%s persp=%d skyEnded=%d zwrite=%lu",
            g_frame, di.kind, (unsigned)di.prims, g_tex0 ? "yes" : "no", (int)g_persp, (int)g_skyEnded,
            (unsigned long)g_rs[D3DRS_ZWRITEENABLE]);
    if (!g_persp) return false;
    g_worldRT = CurRT(d);
    g_worldDrawn = true;
    g_worldDraws++;
    g_worldPrims += di.prims;
    if (g_skyEnded) return false;

    if (g_dumpWindowActive && NowSec() >= g_dumpWindowUntil) g_dumpWindowActive = false;   // window elapsed
    bool dumping = (g_cfg.skyDump && g_dbgDumpFrames < 6 && (g_frame % 600 == 300 || g_frame % 600 == 301)) || g_dumpWindowActive;
    if (g_rs[D3DRS_ZWRITEENABLE]) {   // First draw call that writes depth = terrain/objects begin.
        // ...unless the sky gradient hasn't been drawn yet this frame: then it's an early model draw
        // (weapon swap), not the start of the terrain, so keep the sky phase open. Bounded, so a
        // frame that has no gradient at all still ends its sky phase after a few draws.
        if (!g_skyGradSeen && g_skyEarlyZ < 64) {
            g_skyEarlyZ++;
            if (dumping && g_skyEarlyZ == 1) LogAlways("[sky f%lu] depth-writing draw before the sky gradient: sky phase kept open", g_frame);
            return false;
        }
        g_skyEnded = true;
        if (g_cfg.colorTrace && (g_frame % 72) == 0) LogWeather("terrain-start");
        if (dumping) LogAlways("[sky f%lu] end of sky phase after %d calls", g_frame, g_skyOrd);
        return false;
    }
    int ord = g_skyOrd++;
    // The ordinal numbers change depending on the celestial bodies present in the sky (sun, moon, stars); they are identified by geometry.
    bool isCloud = g_cfg.cloudPrims > 0 ? (di.prims == (UINT)g_cfg.cloudPrims && g_tex0 != NULL)
        : (ord < 64 && ((g_cfg.cloudMask >> ord) & 1ULL));

    float vc[3] = { 0, 0, 0 }, va = 0;
    bool haveVC = false;
    bool wantColor = isCloud && g_cfg.colorSource == 2 && (!g_cloudColorValid || (g_frame % (DWORD)g_cfg.skySampleEvery) == 0);
    bool isSkyCol = g_cfg.skyColorPrims > 0 ? (di.prims == (UINT)g_cfg.skyColorPrims && g_tex0 == NULL) : (ord == g_cfg.skyColorDraw);
    if (isSkyCol) g_skyGradSeen = true;
    bool wantSky = isSkyCol && g_cfg.colorSource == 5 && (!g_skyVValid || (g_frame % (DWORD)g_cfg.skySampleEvery) == 0);
    bool traceNow = g_cfg.colorTrace && ord < 8 && (g_frame % 72) == 0;
    if (dumping || wantColor || wantSky || traceNow) haveVC = ReadVertexColor(di, vc, &va);
    if (haveVC && wantSky) {
        for (int i = 0; i < 3; i++) g_skyVColor[i] = g_skyVValid ? g_skyVColor[i] + (vc[i] - g_skyVColor[i]) * 0.3f : vc[i];
        g_skyVValid = true;
    }
    if (traceNow) {
        if (haveVC) LogAlways("[trace f%lu] #%d %s prims=%u %s%s%s color=(%.3f %.3f %.3f a=%.3f)", g_frame, ord, di.kind, di.prims, g_tex0 ? "tex" : "sin-tex", isCloud ? " [cloud]" : "", isSkyCol ? " [sky]" : "", vc[0], vc[1], vc[2], va);
        else LogAlways("[trace f%lu] #%d %s prims=%u %s%s%s color=n/d", g_frame, ord, di.kind, di.prims, g_tex0 ? "tex" : "sin-tex", isCloud ? " [cloud]" : "", isSkyCol ? " [sky]" : "");
    }
    // Anchor for the sky-phase draw: prefer the sky-gradient call (isSkyCol) over the original
    // cloud dome (isCloud). The gradient always exists and is drawn before the horizon mountain
    // silhouette, so our clouds still end up correctly behind it. The cloud dome, on the other hand,
    // may be skipped entirely by a third-party mod that disables the original clouds itself (e.g. a
    // volumetric lighting mod) -- in that case relying only on isCloud would leave g_pendingSkyClouds
    // unset for the whole frame, and our clouds would fall back to the end-of-frame TryDraw() path,
    // which draws *after* the (depth-less) mountain silhouette and ends up in front of it instead.
    if ((isCloud || isSkyCol) && g_cfg.layer == 1 && !g_skyCloudsTriggeredThisFrame) {
        g_pendingSkyClouds = true;
        g_skyCloudsTriggeredThisFrame = true;
    }

    if (isCloud) {
        if (g_cfg.colorSource == 2) {
            if (haveVC && wantColor) {
                for (int i = 0; i < 3; i++) g_cloudColor[i] = g_cloudColorValid ? g_cloudColor[i] + (vc[i] - g_cloudColor[i]) * 0.3f : vc[i];
                g_cloudColorValid = true;
                static int s_logs = 0;
                if (s_logs < 15) { s_logs++; LogDbg("[frame %lu] original cloud color (vertices)=(%.3f %.3f %.3f a=%.3f)", g_frame, vc[0], vc[1], vc[2], va); }
            }
        }
        else if (g_cfg.colorSource == 3) {
            DWORD tf = g_rs[D3DRS_TEXTUREFACTOR];
            g_cloudColor[0] = ((tf >> 16) & 255) / 255.0f; g_cloudColor[1] = ((tf >> 8) & 255) / 255.0f; g_cloudColor[2] = (tf & 255) / 255.0f;
            g_cloudColorValid = true;
        }
        else if (g_cfg.colorSource == 4) {
            g_cloudColor[0] = g_matDiffuse.r; g_cloudColor[1] = g_matDiffuse.g; g_cloudColor[2] = g_matDiffuse.b;
            g_cloudColorValid = true;
        }
    }

    if (dumping) {
        char tex[96] = "no texture";
        if (g_tex0) {
            IDirect3DBaseTexture9* bt = (IDirect3DBaseTexture9*)g_tex0;
            D3DSURFACE_DESC td;
            if (bt->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(((IDirect3DTexture9*)bt)->GetLevelDesc(0, &td)))
                snprintf(tex, sizeof tex, "%p %ux%u fmt=%u", g_tex0, (unsigned)td.Width, (unsigned)td.Height, (unsigned)td.Format);
            else
                snprintf(tex, sizeof tex, "%p (not 2D)", g_tex0);
        }
        char vcs[64] = "n/d";
        if (haveVC) snprintf(vcs, sizeof vcs, "(%.2f %.2f %.2f a=%.2f)", vc[0], vc[1], vc[2], va);
        LogAlways("[sky f%lu #%d]%s %s prims=%u tex=%s | zfunc=%lu ablend=%lu src=%lu dst=%lu atest=%lu light=%lu cull=%lu tfactor=%08lX fvf=0x%lX coff=%d | tss0 op=%lu a1=%lu a2=%lu aop=%lu | vcolor=%s mat=(%.2f %.2f %.2f)",
            g_frame, ord, isCloud ? " [CloudDraws]" : "", di.kind, di.prims, tex,
            (unsigned long)g_rs[D3DRS_ZFUNC], (unsigned long)g_rs[D3DRS_ALPHABLENDENABLE], (unsigned long)g_rs[D3DRS_SRCBLEND],
            (unsigned long)g_rs[D3DRS_DESTBLEND], (unsigned long)g_rs[D3DRS_ALPHATESTENABLE], (unsigned long)g_rs[D3DRS_LIGHTING],
            (unsigned long)g_rs[D3DRS_CULLMODE], (unsigned long)g_rs[D3DRS_TEXTUREFACTOR], (unsigned long)g_fvf, DiffuseOffset(),
            (unsigned long)g_tss0[1], (unsigned long)g_tss0[2], (unsigned long)g_tss0[3], (unsigned long)g_tss0[4], vcs,
            g_matDiffuse.r, g_matDiffuse.g, g_matDiffuse.b);
    }
    return isCloud && g_cfg.hideOrig;
}

static HRESULT STDMETHODCALLTYPE H_SetTransform(IDirect3DDevice9* d, D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) {
    VT* t = FindVT(d);
    HRESULT hr = t->setTransform(d, s, m);
    if (g_inDraw || !m) return hr;

    if (s == D3DTS_VIEW) {
        g_view = *m; g_haveView = true;
        {   // The game switches camera views based on identities: it settles on the last one that features rotation.
            bool ident = fabsf(m->_11 - 1) < 1e-4f && fabsf(m->_22 - 1) < 1e-4f && fabsf(m->_33 - 1) < 1e-4f &&
                fabsf(m->_12) < 1e-4f && fabsf(m->_13) < 1e-4f && fabsf(m->_21) < 1e-4f && fabsf(m->_23) < 1e-4f && fabsf(m->_31) < 1e-4f && fabsf(m->_32) < 1e-4f;
            if (!ident) { g_camView = *m; g_haveCamView = true; }
        }
        if (g_cfg.debug && g_dbgViewLogs < 6) {
            g_dbgViewLogs++;
            LogDbg("[frame %lu] VIEW row0=(%g %g %g) row1=(%g %g %g) row2=(%g %g %g) row3=(%g %g %g)", g_frame,
                m->_11, m->_12, m->_13, m->_21, m->_22, m->_23, m->_31, m->_32, m->_33, m->_41, m->_42, m->_43);
        }
    }
    else if (s == D3DTS_PROJECTION) {
        bool persp = fabsf(m->_44) < 1e-6f && fabsf(m->_34) > 0.5f;
        if (g_cfg.debug && g_dbgTransformLogs < 80) {
            g_dbgTransformLogs++;
            LogDbg("[frame %lu] SetTransform(PROJECTION) %s  _11=%g _22=%g _33=%g _34=%g _43=%g _44=%g  worldDrawn=%d drawn=%d",
                g_frame, persp ? "PERSP" : "ORTHO", m->_11, m->_22, m->_33, m->_34, m->_43, m->_44, (int)g_worldDrawn, (int)g_drawnThisFrame);
        }
        if (g_dumpWindowActive && NowSec() < g_dumpWindowUntil)
            LogAlways("[proj f%lu] SetTransform(PROJECTION) %s (g_persp was %d, worldDrawn=%d)", g_frame, persp ? "PERSP" : "ORTHO", (int)g_persp, (int)g_worldDrawn);
        if (persp) {
            g_proj = *m; g_haveProj = true; g_persp = true;
        }
        else {
            if (g_persp && g_worldDrawn && g_cfg.colorTrace && (g_frame % 72) == 0) LogWeather("world-end");
            g_persp = false;
            TryDraw(d);   // first step toward an orthographic view after drawing the world (create a backup if the drawing wasn't done on the sky layer).
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* tex) {
    VT* t = FindVT(d);
    if (!g_inDraw && stage == 0) g_tex0 = tex;
    return t->setTexture(d, stage, tex);
}
static HRESULT STDMETHODCALLTYPE H_SetTSS(IDirect3DDevice9* d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD v) {
    VT* t = FindVT(d);
    if (!g_inDraw && stage == 0 && (unsigned)type < 32) g_tss0[(unsigned)type] = v;
    return t->setTSS(d, stage, type, v);
}
static HRESULT STDMETHODCALLTYPE H_SetFVF(IDirect3DDevice9* d, DWORD fvf) {
    VT* t = FindVT(d);
    if (!g_inDraw) g_fvf = fvf;
    return t->setFVF(d, fvf);
}
static HRESULT STDMETHODCALLTYPE H_SetVertexDecl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl) {
    VT* t = FindVT(d);
    if (!g_inDraw) { g_fvf = 0; g_declColorOff = decl ? LookupDeclColorOffset(decl) : -1; }
    return t->setVertexDecl(d, decl);
}
static HRESULT STDMETHODCALLTYPE H_SetStreamSource(IDirect3DDevice9* d, UINT stream, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    VT* t = FindVT(d);
    if (!g_inDraw && stream == 0) { g_vb0 = vb; g_vb0Off = off; g_vb0Stride = stride; }
    return t->setStreamSource(d, stream, vb, off, stride);
}
static HRESULT STDMETHODCALLTYPE H_SetMaterial(IDirect3DDevice9* d, const D3DMATERIAL9* m) {
    VT* t = FindVT(d);
    if (!g_inDraw && m) g_matDiffuse = m->Diffuse;
    return t->setMaterial(d, m);
}

static HRESULT STDMETHODCALLTYPE H_SetLight(IDirect3DDevice9* d, DWORD idx, const D3DLIGHT9* l) {
    VT* t = FindVT(d);
    if (!g_inDraw && idx == 0 && l) { g_light0[0] = l->Diffuse.r; g_light0[1] = l->Diffuse.g; g_light0[2] = l->Diffuse.b; }
    return t->setLight(d, idx, l);
}

static HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* sfc) {
    VT* t = FindVT(d);
    // If the game is going to switch render targets after drawing the world, we draw beforehand (while the world's render target is still active).
    if (!g_inDraw && idx == 0 && g_worldDrawn && !g_drawnThisFrame && (void*)sfc != CurRT(d)) TryDraw(d);
    HRESULT hr = t->setRenderTarget(d, idx, sfc);
    if (SUCCEEDED(hr) && idx == 0 && !g_inDraw) g_curRT = sfc;
    return hr;
}

// Following a call from the sky—that of the original clouds—draw ours at that same point in the frame.
static inline void PostDraw(IDirect3DDevice9* d) {
    if (!g_pendingSkyClouds) return;
    g_pendingSkyClouds = false;
    if (g_dumpWindowActive && NowSec() < g_dumpWindowUntil)
        LogAlways("[postdraw f%lu] sky-phase clouds (alreadyDrawn=%d skyOrd=%d)", g_frame, (int)g_drawnThisFrame, g_skyOrd);
    if (!g_drawnThisFrame && DrawAllCloudLayers(d, true)) g_drawnThisFrame = true;
}

static HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE pt, UINT sv, UINT pc) {
    VT* t = FindVT(d);
    DrawInfo di = { "DP", pc, sv, 8, NULL, 0 };
    HRESULT hr = OnDraw(d, di) ? D3D_OK : t->drawPrimitive(d, pt, sv, pc);
    PostDraw(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    VT* t = FindVT(d);
    DrawInfo di = { "DIP", pc, (UINT)(bv + (INT)mi), nv < 1024 ? nv : 1024, NULL, 0 };
    HRESULT hr = OnDraw(d, di) ? D3D_OK : t->drawIndexedPrimitive(d, pt, bv, mi, nv, si, pc);
    PostDraw(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE pt, UINT pc, const void* data, UINT stride) {
    VT* t = FindVT(d);
    DrawInfo di = { "DPUP", pc, 0, 8, (const BYTE*)data, stride };
    HRESULT hr = OnDraw(d, di) ? D3D_OK : t->drawPrimitiveUP(d, pt, pc, data, stride);
    PostDraw(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE pt, UINT mi, UINT nv, UINT pc, const void* idx, D3DFORMAT ifmt, const void* vdata, UINT stride) {
    VT* t = FindVT(d);
    DrawInfo di = { "DIPUP", pc, 0, nv < 1024 ? nv : 1024, (const BYTE*)vdata + (size_t)mi * stride, stride };
    HRESULT hr = OnDraw(d, di) ? D3D_OK : t->drawIndexedPrimitiveUP(d, pt, mi, nv, pc, idx, ifmt, vdata, stride);
    PostDraw(d);
    return hr;
}

// It detects weather conditions by observing how quickly the sky color (g_skyVColor) darkens relative to 
// a "baseline" that tracks it slowly. Rain darkens the sky in a few seconds, whereas the day/night cycle 
// takes minutes or hours—meaning it does not shift the baseline enough to trigger the detector.
static void UpdateWeatherFromSky(double now) {
    float lum = (g_skyVColor[0] + g_skyVColor[1] + g_skyVColor[2]) / 3.0f;

    if (!g_wxBaselineInit) {
        g_wxBaseline = lum;
        g_wxLastLum = lum;
        g_wxLastJumpCheckTime = now;
        g_lastWxTime = now;
        g_wxBaselineInit = true;
        return;
    }

    float dt = (float)(now - g_lastWxTime);
    g_lastWxTime = now;
    if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;   // first frame after a long pause

    // A jump much faster than what rain can produce, measured over a window of at least WxJumpWindow 
    // seconds (not frame-by-frame, to avoid confusing single-frame noise with a real jump). Likely a 
    // manual time change or another external event. We resynchronize the baseline with the current 
    // color and do NOT count it as weather.
    double sinceCheck = now - g_wxLastJumpCheckTime;
    if (sinceCheck >= g_cfg.wxJumpWindow) {
        float stepRate = (sinceCheck > 0.0001) ? fabsf(lum - g_wxLastLum) / (float)sinceCheck : 0.0f;
        bool wasJump = stepRate >= g_cfg.wxJumpRate;
        g_wxLastLum = lum;
        g_wxLastJumpCheckTime = now;
        if (wasJump) {
            g_wxBaseline = lum;
            g_wxCandidate = g_weather;
            g_wxCandidateSince = -1.0;
            if (g_cfg.wxLog)
                LogAlways("[WXSKY] sudden jump (rate=%.3f/s en %.2fs) -> resyncing baseline, not counted as weather", stepRate, sinceCheck);
            return;
        }
    }

    // The baseline only updates while the confirmed weather is clear;
    // if it is already raining, it freezes to avoid "chasing" the darkening.
    if (g_weather == WX_CLEAR && g_cfg.wxBaselineTau > 0.01f) {
        float alpha = dt / g_cfg.wxBaselineTau;
        if (alpha > 1.0f) alpha = 1.0f;
        g_wxBaseline += (lum - g_wxBaseline) * alpha;
    }

    float dev = g_wxBaseline - lum;   // positive = darker than the baseline

    int cand = (dev >= g_cfg.wxDevEnter) ? WX_RAIN : (dev <= g_cfg.wxDevExit ? WX_CLEAR : g_weather);

    if (cand != g_wxCandidate) {
        g_wxCandidate = cand;
        g_wxCandidateSince = now;
    }
    if (cand != g_weather && g_wxCandidateSince >= 0.0 && (now - g_wxCandidateSince) >= g_cfg.wxConfirmSec) {
        g_weather = cand;
    }

    if (g_cfg.wxLog && (g_frame % 60) == 0)
        LogAlways("[WXSKY] lum=%.3f base=%.3f dev=%.3f ´candidate=%d confirmed=%d",
            lum, g_wxBaseline, dev, g_wxCandidate, g_weather);
}

// Reads CMapWeather+0x20 (see lighting.md). Confirmed with Cheat Engine: it sends a pulse with
// the correct type (0=clear, 1=rain, 2=snow, 3=dust) for a few seconds when the weather
// changes, and rests at -1 (0xFFFFFFFF) the rest of the time.
static int ReadClientWeatherTypeRaw() {
    __try {
        DWORD base = (DWORD)GetModuleHandleA(NULL);
        DWORD slot = base + (0xc6326c - 0x400000);
        DWORD mgr = *(volatile DWORD*)slot;
        if (!mgr) return -2;   // manager not yet initialized
        return (int)(*(volatile DWORD*)(mgr + 0x20));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;   // invalid read (address not mapped)
    }
}

// Calculates "how much daylight there is" (0 = pitch black, 1 = full daylight) based on the actual sky color
// (using the same source—g_skyVColor/g_skyColor—and the same SkyLumNight/
// SkyLumDay thresholds already used by ComputeColors to darken clouds at night). This ensures the
// daytime transparency effect is perfectly synchronized with the existing nighttime darkening,
// without relying on any new memory address.
static float ComputeDayness() {
    const float* T = NULL;
    if (g_cfg.colorSource == 5 && g_skyVValid) T = g_skyVColor;
    else if (g_cfg.colorSource != 0 && g_skyValid) T = g_skyColor;
    if (!T) return 0.0f;   // no valid color yet (e.g., just starting up) -> no effect
    float lum = 0.299f * T[0] + 0.587f * T[1] + 0.114f * T[2];
    return Smooth(g_cfg.skyLumNight, g_cfg.skyLumDay, lum);
}

// Main detector: reads the CMapWeather effect slots. +0x28 = pointer to the active
// rain effect (0 if none), +0x2c = snow, +0x30 = dust. Confirmed with
// Cheat Engine: they hold a value while the effect exists, revert to 0 when it ends, and—
// unlike +0x20 (pulse)—they also trigger when transitioning out of a sustained clear state.
// Returns WX_UNKNOWN only if the manager does not yet exist (e.g., on the loading screen).
static int ReadClientWeatherSlots() {
    __try {
        DWORD base = (DWORD)GetModuleHandleA(NULL);
        DWORD slotPtr = base + (0xc6326c - 0x400000);
        DWORD mgr = *(volatile DWORD*)slotPtr;
        if (!mgr) return WX_UNKNOWN;
        DWORD rainSlot = *(volatile DWORD*)(mgr + 0x28);
        DWORD snowSlot = *(volatile DWORD*)(mgr + 0x2c);
        DWORD sandSlot = *(volatile DWORD*)(mgr + 0x30);
        if (rainSlot != 0) return WX_RAIN;
        if (snowSlot != 0) return WX_SNOW;
        if (sandSlot != 0) return WX_SAND;
        return WX_CLEAR;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return WX_UNKNOWN;
    }
}

// 0x00B71EA8: Current AreaId (zone/sub-zone). Found and confirmed by the user using Cheat Engine
// (ID search, eliminating candidates, validated by cross-referencing a full client restart).
static int ReadClientAreaId() {
    __try {
        DWORD base = (DWORD)GetModuleHandleA(NULL);
        DWORD addr = base + (0x00b71ea8 - 0x400000);
        return (int)(*(volatile DWORD*)addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;   // invalid read (address not mapped)
    }
}

// Holds the last valid pulse (0-3) seen in CMapWeather+0x20, since the field itself
// only holds it for a few seconds before reverting to -1. Call once per frame.
static void UpdatePulseWeather() {
    int raw = ReadClientWeatherTypeRaw();
    if (raw >= WX_CLEAR && raw <= WX_SAND) {
        g_wxPulseHeld = raw;
        g_wxPulseEverSeen = true;
    }
    // raw == -1 (idle), -2 (manager not ready), or -3 (invalid read): nothing is changed,
    // g_wxPulseHeld remains held.
}

// ---------------------------------------------------------------------------------------------
// Lua bridge: lets in-game addons read and change this DLL's settings in real time.
// Engine addresses come from ClassicAPI's Offsets.h (GPL-3.0), stock 1.12.1 client layout.
// The engine's lua_* API is __fastcall (L in ECX). Registration needs no code hook: WoW's Lua
// functions are plain globals, so once the engine's Lua state exists we register ours from the
// Present hook (game thread, outside any Lua call).
//
// Lua-side API (all numbers/strings, no tables, to keep the engine calls minimal):
//   WowCloudsPing()                -> 12345 (connectivity test)
//   WowCloudsCount()               -> number of tunable variables
//   WowCloudsInfo(i)               -> name, group, min, max, step, value   (i is 1-based; nil if out of range)
//   WowCloudsGet(name)             -> value, or nil
//   WowCloudsSet(name, value)      -> true/false; clamps to [min,max]; applies immediately, not saved
//   WowCloudsSave()                -> writes the variables changed since the last load/save to the ini, returns how many
//   WowCloudsReload()              -> drops unsaved changes by re-reading the ini, returns true
// ---------------------------------------------------------------------------------------------
static const DWORD LUA_ADDR_REGISTER_FUNCTION = 0x00704120;   // FrameScript_RegisterFunction, __fastcall(name, func)
static const DWORD LUA_ADDR_STATE_VAR = 0x00CEEF74;           // pointer to the active lua_State (glue state at login, world state in-game)
static const DWORD LUA_ADDR_GET_TOP = 0x006F3070;
static const DWORD LUA_ADDR_IS_NUMBER = 0x006F34D0;
static const DWORD LUA_ADDR_IS_STRING = 0x006F3510;
static const DWORD LUA_ADDR_TO_NUMBER = 0x006F3620;
static const DWORD LUA_ADDR_TO_STRING = 0x006F3690;
static const DWORD LUA_ADDR_PUSH_NIL = 0x006F37F0;
static const DWORD LUA_ADDR_PUSH_NUMBER = 0x006F3810;
static const DWORD LUA_ADDR_PUSH_STRING = 0x006F3890;
static const DWORD LUA_ADDR_PUSH_BOOLEAN = 0x006F39F0;

typedef int(__fastcall* LuaCFunction_t)(void* L);
typedef void(__fastcall* FrameScriptRegisterFunction_t)(const char* name, LuaCFunction_t func);

// Engine address -> this process's address (WoW.exe is linked at 0x400000).
static DWORD GameAddr(DWORD a) { return (DWORD)GetModuleHandleA(NULL) + (a - 0x400000); }

static inline int    LuaIsNumber(void* L, int idx) { return ((int(__fastcall*)(void*, int))GameAddr(LUA_ADDR_IS_NUMBER))(L, idx); }
static inline int    LuaIsString(void* L, int idx) { return ((int(__fastcall*)(void*, int))GameAddr(LUA_ADDR_IS_STRING))(L, idx); }
static inline double LuaToNumber(void* L, int idx) { return ((double(__fastcall*)(void*, int))GameAddr(LUA_ADDR_TO_NUMBER))(L, idx); }
static inline const char* LuaToString(void* L, int idx) { return ((const char* (__fastcall*)(void*, int))GameAddr(LUA_ADDR_TO_STRING))(L, idx); }
static inline void   LuaPushNil(void* L) { ((void(__fastcall*)(void*))GameAddr(LUA_ADDR_PUSH_NIL))(L); }
static inline void   LuaPushNumber(void* L, double n) { ((void(__fastcall*)(void*, double))GameAddr(LUA_ADDR_PUSH_NUMBER))(L, n); }
static inline void   LuaPushString(void* L, const char* str) { ((void(__fastcall*)(void*, const char*))GameAddr(LUA_ADDR_PUSH_STRING))(L, str); }
static inline void   LuaPushBoolean(void* L, int b) { ((void(__fastcall*)(void*, int))GameAddr(LUA_ADDR_PUSH_BOOLEAN))(L, b); }

// ---- Tunable variables: what the in-game window can edit. ----
// Each entry points straight at a field of g_cfg (or one of its extra layers), so a change applies
// on the very next frame. `dirty` marks values changed in-game and not yet written to the ini.
// Ranges/steps are only hints for the UI sliders; Set clamps to [lo,hi].
struct TunableVar {
    const char* name;    // public name used by Lua
    const char* group;   // UI tab
    const char* sec;     // ini section, for Save
    const char* key;     // ini key, for Save
    float* f;            // exactly one of f / i is non-null
    int* i;
    float lo, hi, step;
    bool dirty;
};
#define TV_F(n, g, s, k, fld, lo, hi, st) { n, g, s, k, &g_cfg.fld, NULL, lo, hi, st, false }
#define TV_I(n, g, s, k, fld)             { n, g, s, k, NULL, &g_cfg.fld, 0.0f, 1.0f, 1.0f, false }
// The same appearance keys exist for each extra layer (section CloudsLayerN, names prefixed "LN.").
#define TV_LAYER(P, G, S, L) \
    TV_I(P "Enabled", G, S, "Enabled", L.enabled), \
    TV_F(P "Scale", G, S, "Scale", L.scale, 0.2f, 8.0f, 0.05f), \
    TV_F(P "Coverage", G, S, "Coverage", L.coverage, 0.0f, 1.0f, 0.01f), \
    TV_F(P "Opacity", G, S, "Opacity", L.opacity, 0.0f, 2.0f, 0.05f), \
    TV_F(P "Softness", G, S, "Softness", L.softness, 0.01f, 0.5f, 0.01f), \
    TV_F(P "HorizonFade", G, S, "HorizonFade", L.horizonFade, 0.0f, 1.0f, 0.01f), \
    TV_F(P "Dome", G, S, "Dome", L.dome, 0.0f, 1.0f, 0.01f), \
    TV_F(P "WindX", G, S, "WindX", L.windX, -0.3f, 0.3f, 0.005f), \
    TV_F(P "WindY", G, S, "WindY", L.windY, -0.3f, 0.3f, 0.005f), \
    TV_F(P "TimeScale", G, S, "TimeScale", L.timeScale, 0.0f, 2.0f, 0.01f), \
    TV_F(P "Evolve", G, S, "Evolve", L.evolve, 0.0f, 1.0f, 0.01f), \
    TV_F(P "Detail", G, S, "Detail", L.detail, 0.0f, 3.0f, 0.1f), \
    TV_F(P "BandSoftness", G, S, "BandSoftness", L.bandSoft, 0.005f, 0.3f, 0.005f), \
    TV_F(P "RainCoverage", G, S, "RainCoverage", L.rainCoverage, -1.0f, 1.0f, 0.01f), /* -1 = inherit the global weather shift */ \
    TV_F(P "RainOpacity", G, S, "RainOpacity", L.rainOpacity, -1.0f, 2.0f, 0.05f)     /* -1 = inherit */

static TunableVar g_vars[] = {
    // Layer 1 (the [Clouds] keys)
    TV_F("Scale", "Layer1", "Clouds", "Scale", scale, 0.2f, 8.0f, 0.05f),
    TV_F("Coverage", "Layer1", "Clouds", "Coverage", coverage, 0.0f, 1.0f, 0.01f),
    TV_F("Opacity", "Layer1", "Clouds", "Opacity", opacity, 0.0f, 2.0f, 0.05f),
    TV_F("Softness", "Layer1", "Clouds", "Softness", softness, 0.01f, 0.5f, 0.01f),
    TV_F("HorizonFade", "Layer1", "Clouds", "HorizonFade", horizonFade, 0.0f, 1.0f, 0.01f),
    TV_F("Dome", "Layer1", "Clouds", "Dome", dome, 0.0f, 1.0f, 0.01f),
    TV_F("WindX", "Layer1", "Clouds", "WindX", windX, -0.3f, 0.3f, 0.005f),
    TV_F("WindY", "Layer1", "Clouds", "WindY", windY, -0.3f, 0.3f, 0.005f),
    TV_F("TimeScale", "Layer1", "Clouds", "TimeScale", timeScale, 0.0f, 2.0f, 0.01f),
    TV_F("Evolve", "Layer1", "Clouds", "Evolve", evolve, 0.0f, 1.0f, 0.01f),
    TV_F("Detail", "Layer1", "Clouds", "Detail", detail, 0.0f, 3.0f, 0.1f),
    TV_F("BandSoftness", "Layer1", "Clouds", "BandSoftness", bandSoft, 0.005f, 0.3f, 0.005f),
    // Extra layers
    TV_LAYER("L2.", "Layer2", "CloudsLayer2", layer2),
    TV_LAYER("L3.", "Layer3", "CloudsLayer3", layer3),
    // Light and color
    TV_F("SunAzimuth", "Light", "Clouds", "SunAzimuth", sunAz, 0.0f, 360.0f, 1.0f),
    TV_F("SunElevation", "Light", "Clouds", "SunElevation", sunEl, 0.0f, 90.0f, 1.0f),
    TV_F("NightBrightness", "Light", "Clouds", "NightBrightness", nightBrightness, 0.0f, 1.0f, 0.01f),
    TV_F("TintMix", "Light", "Clouds", "TintMix", tintMix, 0.0f, 2.0f, 0.05f),
    TV_F("ColorGain", "Light", "Clouds", "ColorGain", colorGain, 0.0f, 3.0f, 0.05f),
    TV_F("MinLit", "Light", "Clouds", "MinLit", minLit, 0.0f, 0.5f, 0.01f),
    // Weather
    TV_F("RainCoverage", "Weather", "Clouds", "RainCoverage", rainCoverage, 0.0f, 1.0f, 0.01f),
    TV_F("RainOpacity", "Weather", "Clouds", "RainOpacity", rainOpacity, 0.0f, 2.0f, 0.05f),
    TV_F("RainDarken", "Weather", "Clouds", "RainDarken", rainDarken, 0.0f, 1.0f, 0.01f),
    TV_F("RainBlendSpeed", "Weather", "Clouds", "RainBlendSpeed", rainBlendSpeed, 0.01f, 1.0f, 0.01f),
    TV_I("DayTransparencyEnabled", "Weather", "Clouds", "DayTransparencyEnabled", dayTransparencyEnabled),
    TV_F("DayTransparencyAmount", "Weather", "Clouds", "DayTransparencyAmount", dayTransparencyAmount, 0.0f, 1.0f, 0.01f),
    // Global
    TV_I("Enabled", "Global", "Clouds", "Enabled", enabled),
    TV_I("HideOriginalClouds", "Global", "Clouds", "HideOriginalClouds", hideOrig),
    TV_F("RenderScale", "Global", "Clouds", "RenderScale", renderScale, 0.25f, 1.0f, 0.05f),
};
static const int g_varCount = (int)(sizeof(g_vars) / sizeof(g_vars[0]));

static float TvGet(const TunableVar& v) { return v.f ? *v.f : (float)*v.i; }
static void TvSet(TunableVar& v, float x) {
    if (x < v.lo) x = v.lo;
    if (x > v.hi) x = v.hi;
    if (v.f) *v.f = x; else *v.i = (int)(x + (x >= 0.0f ? 0.5f : -0.5f));
    v.dirty = true;
}
static TunableVar* TvFind(const char* name) {
    if (!name) return NULL;
    for (int k = 0; k < g_varCount; k++) if (_stricmp(g_vars[k].name, name) == 0) return &g_vars[k];
    return NULL;
}
static void TunablesClearDirty() { for (int k = 0; k < g_varCount; k++) g_vars[k].dirty = false; }

// ---- Lua-callable functions (engine ABI: int __fastcall(lua_State*); returns how many values it pushed) ----
static int __fastcall Script_WowCloudsPing(void* L) { LuaPushNumber(L, 12345.0); return 1; }

static int __fastcall Script_WowCloudsCount(void* L) { LuaPushNumber(L, (double)g_varCount); return 1; }

static int __fastcall Script_WowCloudsInfo(void* L) {
    if (!LuaIsNumber(L, 1)) { LuaPushNil(L); return 1; }
    int idx = (int)LuaToNumber(L, 1) - 1;
    if (idx < 0 || idx >= g_varCount) { LuaPushNil(L); return 1; }
    const TunableVar& v = g_vars[idx];
    LuaPushString(L, v.name);
    LuaPushString(L, v.group);
    LuaPushNumber(L, v.lo);
    LuaPushNumber(L, v.hi);
    LuaPushNumber(L, v.step);
    LuaPushNumber(L, TvGet(v));
    return 6;
}

static int __fastcall Script_WowCloudsGet(void* L) {
    TunableVar* v = LuaIsString(L, 1) ? TvFind(LuaToString(L, 1)) : NULL;
    if (!v) { LuaPushNil(L); return 1; }
    LuaPushNumber(L, TvGet(*v));
    return 1;
}

static int __fastcall Script_WowCloudsSet(void* L) {
    TunableVar* v = (LuaIsString(L, 1) && LuaIsNumber(L, 2)) ? TvFind(LuaToString(L, 1)) : NULL;
    if (!v) { LuaPushBoolean(L, 0); return 1; }
    TvSet(*v, (float)LuaToNumber(L, 2));
    LuaPushBoolean(L, 1);
    return 1;
}

static int __fastcall Script_WowCloudsSave(void* L) {
    int written = 0;
    for (int k = 0; k < g_varCount; k++) {
        TunableVar& v = g_vars[k];
        if (!v.dirty) continue;
        char buf[32];
        snprintf(buf, sizeof buf, "%g", TvGet(v));
        WritePrivateProfileStringA(v.sec, v.key, buf, g_iniPath);
        v.dirty = false;
        written++;
    }
    if (written) {
        // The values in memory already match what we just wrote, so skip the hot reload it would trigger.
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad)) g_iniTime = fad.ftLastWriteTime;
    }
    LuaPushNumber(L, (double)written);
    return 1;
}

static int __fastcall Script_WowCloudsReload(void* L) {
    LoadConfig();
    LuaPushBoolean(L, 1);
    return 1;
}

struct LuaExport { const char* name; LuaCFunction_t fn; };
static const LuaExport g_luaExports[] = {
    { "WowCloudsPing", Script_WowCloudsPing },
    { "WowCloudsCount", Script_WowCloudsCount },
    { "WowCloudsInfo", Script_WowCloudsInfo },
    { "WowCloudsGet", Script_WowCloudsGet },
    { "WowCloudsSet", Script_WowCloudsSet },
    { "WowCloudsSave", Script_WowCloudsSave },
    { "WowCloudsReload", Script_WowCloudsReload },
};

// Registers our Lua functions on whichever Lua state is active. The state is replaced on every
// glue <-> world transition (login screen, logout), so this re-registers when the pointer changes,
// and only once it has stayed the same for ~1 second (so the engine has finished populating it).
static void LuaBridgeTick() {
    static void* s_pendingState = NULL;
    static void* s_doneState = NULL;
    static DWORD s_seenFrame = 0;
    static DWORD s_lastRegFrame = 0;
    if (!g_cfg.luaBridge) return;

    void* L = NULL;
    __try { L = *(void**)GameAddr(LUA_ADDR_STATE_VAR); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!L) return;
    if (L != s_pendingState) {
        s_pendingState = L;
        s_seenFrame = g_frame;
        LogAlways("[lua] active lua_State is now %p (frame %lu)", L, (unsigned long)g_frame);
        return;
    }
    if (g_frame - s_seenFrame < 60) return;

    // Register once the state has been stable for ~1 s. After that, keep re-registering every ~5 s:
    // the engine can tear down and rebuild its Lua state (login <-> world), and a new state may be
    // allocated at the very same address as the old one, which a pointer comparison alone can't
    // tell apart -- the functions would then be missing from the new state. Registering is just
    // setting a global, so repeating it is harmless.
    bool firstTime = (L != s_doneState);
    if (!firstTime && g_frame - s_lastRegFrame < 300) return;
    s_doneState = L;
    s_lastRegFrame = g_frame;

    __try {
        FrameScriptRegisterFunction_t reg = (FrameScriptRegisterFunction_t)GameAddr(LUA_ADDR_REGISTER_FUNCTION);
        for (int k = 0; k < (int)(sizeof(g_luaExports) / sizeof(g_luaExports[0])); k++) reg(g_luaExports[k].name, g_luaExports[k].fn);
        if (firstTime) LogAlways("[lua] registered %d functions on lua_State %p (frame %lu)", (int)(sizeof(g_luaExports) / sizeof(g_luaExports[0])), L, (unsigned long)g_frame);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (firstTime) LogAlways("[lua] registering the Lua functions raised an exception");
    }
}

static HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    VT* t = FindVT(d);
    if (g_cfg.skyDump && g_skyOrd > 0 && (g_frame % 600 == 300 || g_frame % 600 == 301)) g_dbgDumpFrames++;
    g_frame++;
    LuaBridgeTick();
    if (g_worldDrawn && !g_drawnThisFrame && g_dbgAttnLogs < 20) {
        g_dbgAttnLogs++;
        LogDbg("[frame %lu] WARNING: the world was drawn, but the clouds were not drawn.", g_frame);
    }
    g_worldDrawn = false;
    g_drawnThisFrame = false;
    g_persp = false;
    g_skyOrd = 0;
    g_skyEnded = false;
    g_skyCloudsTriggeredThisFrame = false;
    g_skyGradSeen = false;
    g_skyEarlyZ = 0;
    g_pendingSkyClouds = false;
    g_worldDraws = 0;
    g_worldPrims = 0;
    if (g_haveFog) {   // smoothed to prevent flickering
        float r8 = (float)((g_fogLast >> 16) & 255) / 255.0f, g8 = (float)((g_fogLast >> 8) & 255) / 255.0f, b8 = (float)(g_fogLast & 255) / 255.0f;
        if (g_frame < 3) { g_fogSmooth[0] = r8; g_fogSmooth[1] = g8; g_fogSmooth[2] = b8; }
        else { g_fogSmooth[0] += (r8 - g_fogSmooth[0]) * 0.08f; g_fogSmooth[1] += (g8 - g_fogSmooth[1]) * 0.08f; g_fogSmooth[2] += (b8 - g_fogSmooth[2]) * 0.08f; }
    }

    double now = NowSec();

    // Primary detector: CMapWeather effect slots (+0x28/+0x2c/+0x30), held
    // for the duration of the weather, with no pulses to track. Backup 1: the +0x20 pulse (in
    // case the manager isn't ready yet). Backup 2: sky color (in case the
    // CMapWeather layout changes in a future patch and neither of the previous two works).
    UpdateWeatherFromSky(now);   // It always runs, even when not in use, so as not to lose the baseline.

    g_dayness = g_cfg.dayTransparencyEnabled ? ComputeDayness() : 0.0f;

    const char* wxSource = "color";
    if (g_cfg.wxUseSlots) {
        int slots = ReadClientWeatherSlots();
        if (slots != WX_UNKNOWN) {
            g_weather = slots;
            wxSource = "slots";
        }
        else if (g_cfg.wxUsePulse) {
            UpdatePulseWeather();
            if (g_wxPulseEverSeen) { g_weather = g_wxPulseHeld; wxSource = "pulse"; }
        }
    }
    else if (g_cfg.wxUsePulse) {
        UpdatePulseWeather();
        if (g_wxPulseEverSeen) { g_weather = g_wxPulseHeld; wxSource = "pulse"; }
    }

    // AreaId confirmed by the user using Cheat Engine. To validate the log: walk between a
    // couple of zones/sub-zones and check that the number changes at the expected times.
    if (g_cfg.areaLog && (g_frame % 60) == 0) {
        int areaId = ReadClientAreaId();
        LogAlways("[AREAID] frame=%lu 0x00B71EA8=%d", (unsigned long)g_frame, areaId);
    }

    // The effect (coverage/opacity/darkening) applies equally to rain, snow, and dust;
    // Only clearing the sky leaves the clouds in their normal state.
    float rainTarget = (g_weather == WX_RAIN || g_weather == WX_SNOW || g_weather == WX_SAND) ? 1.0f : 0.0f;
    float dt = (g_lastBlendTime < 0.0) ? (1.0f / 60.0f) : (float)(now - g_lastBlendTime);
    g_lastBlendTime = now;
    if (dt < 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;   // first frame or long pause: use a reasonable value
    // g_zoneWeatherBlendSpeedMult holds last frame's smoothed value (updated further below); using
    // it here, one frame behind, is fine since it only ever changes gradually.
    float step = g_cfg.rainBlendSpeed * g_zoneWeatherBlendSpeedMult * dt;
    if (g_rainBlend < rainTarget) { g_rainBlend += step; if (g_rainBlend > rainTarget) g_rainBlend = rainTarget; }
    else if (g_rainBlend > rainTarget) { g_rainBlend -= step; if (g_rainBlend < rainTarget) g_rainBlend = rainTarget; }

    // Zone preset active by AreaId: each value (darken/speedMult/coverageAdd) transitions independently,
    // at a rate of ZoneBlendSpeed ​​per second, toward the current preset's value (or toward neutral if none
    // is active). Since it does not rely on a single 0–1 factor, it also handles direct transitions between
    // presets well, without passing through a "base" zone.
    {
        int areaId = ReadClientAreaId();
        int idx = FindPresetForArea(areaId);
        float targetDarken = (idx >= 0) ? g_zonePresets[idx].darken : 1.0f;
        float targetSpeedMult = (idx >= 0) ? g_zonePresets[idx].speedMult : 1.0f;
        float targetCoverageAdd = (idx >= 0) ? g_zonePresets[idx].coverageAdd : 0.0f;
        // Neutral target (no active preset) is the global TintMix/1.0, same spirit as Darken/SpeedMult above.
        float targetTintMix = (idx >= 0) ? g_zonePresets[idx].tintMix : g_cfg.tintMix;
        float targetWeatherBlendSpeedMult = (idx >= 0) ? g_zonePresets[idx].weatherBlendSpeedMult : 1.0f;
        float zstepDarken = g_cfg.zoneDarkenBlendSpeed * dt;
        float zstepSpeed = g_cfg.zoneSpeedBlendSpeed * dt;
        float zstepCoverage = g_cfg.zoneCoverageBlendSpeed * dt;
        float zstepTintMix = g_cfg.zoneTintMixBlendSpeed * dt;
        float zstepWeatherBlendSpeedMult = g_cfg.zoneWeatherBlendSpeedMultBlendSpeed * dt;
        if (g_zoneDarken < targetDarken) { g_zoneDarken += zstepDarken; if (g_zoneDarken > targetDarken) g_zoneDarken = targetDarken; }
        else if (g_zoneDarken > targetDarken) { g_zoneDarken -= zstepDarken; if (g_zoneDarken < targetDarken) g_zoneDarken = targetDarken; }
        if (g_zoneSpeedMult < targetSpeedMult) { g_zoneSpeedMult += zstepSpeed; if (g_zoneSpeedMult > targetSpeedMult) g_zoneSpeedMult = targetSpeedMult; }
        else if (g_zoneSpeedMult > targetSpeedMult) { g_zoneSpeedMult -= zstepSpeed; if (g_zoneSpeedMult < targetSpeedMult) g_zoneSpeedMult = targetSpeedMult; }
        if (g_zoneCoverageAdd < targetCoverageAdd) { g_zoneCoverageAdd += zstepCoverage; if (g_zoneCoverageAdd > targetCoverageAdd) g_zoneCoverageAdd = targetCoverageAdd; }
        else if (g_zoneCoverageAdd > targetCoverageAdd) { g_zoneCoverageAdd -= zstepCoverage; if (g_zoneCoverageAdd < targetCoverageAdd) g_zoneCoverageAdd = targetCoverageAdd; }
        if (g_zoneTintMix < targetTintMix) { g_zoneTintMix += zstepTintMix; if (g_zoneTintMix > targetTintMix) g_zoneTintMix = targetTintMix; }
        else if (g_zoneTintMix > targetTintMix) { g_zoneTintMix -= zstepTintMix; if (g_zoneTintMix < targetTintMix) g_zoneTintMix = targetTintMix; }
        if (g_zoneWeatherBlendSpeedMult < targetWeatherBlendSpeedMult) { g_zoneWeatherBlendSpeedMult += zstepWeatherBlendSpeedMult; if (g_zoneWeatherBlendSpeedMult > targetWeatherBlendSpeedMult) g_zoneWeatherBlendSpeedMult = targetWeatherBlendSpeedMult; }
        else if (g_zoneWeatherBlendSpeedMult > targetWeatherBlendSpeedMult) { g_zoneWeatherBlendSpeedMult -= zstepWeatherBlendSpeedMult; if (g_zoneWeatherBlendSpeedMult < targetWeatherBlendSpeedMult) g_zoneWeatherBlendSpeedMult = targetWeatherBlendSpeedMult; }

        // With the zoneSpeedMult for this frame already updated, the effective time is integrated.
        g_animTime += (double)dt * g_cfg.timeScale * g_zoneSpeedMult;
        // Same integration, but without TimeScale baked in -- see g_animTimeRaw's declaration.
        g_animTimeRaw += (double)dt * g_zoneSpeedMult;

        if (g_cfg.presetLog && (g_frame % 60) == 0) {
            const char* presetName = (idx >= 0) ? g_zonePresets[idx].name : "(none)";
            LogAlways("[ZONEPRESET] frame=%lu areaId=%d preset=\"%s\" darken=%.3f speedMult=%.3f coverageAdd=%.3f tintMix=%.3f weatherBlendSpeedMult=%.3f",
                (unsigned long)g_frame, areaId, presetName, g_zoneDarken, g_zoneSpeedMult, g_zoneCoverageAdd, g_zoneTintMix, g_zoneWeatherBlendSpeedMult);
        }
    }

    MaybeReloadConfig();
    return t->present(d, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    VT* t = FindVT(d);
    if (g_sb) { g_sb->Release(); g_sb = NULL; }   // the state blocks must be released before Reset.
    g_curRT = NULL; g_worldRT = NULL;
    ReleaseSampleBuffers();   // DEFAULT pool surfaces: must be released before Reset
    ReleaseCloudRT();         // same reason
    return t->reset(d, pp);
}

static void PatchDevice(IDirect3DDevice9* dev) {
    void** vt = *(void***)dev;
    for (int i = 0; i < g_nvt; i++) if (g_vts[i].vtbl == vt) return;
    if (g_nvt >= 4) return;
    VT t;
    t.vtbl = vt;
    t.reset = (Reset_t)PatchVTable(vt, 16, (void*)H_Reset);
    t.present = (Present_t)PatchVTable(vt, 17, (void*)H_Present);
    t.setTransform = (SetTransform_t)PatchVTable(vt, 44, (void*)H_SetTransform);
    t.setRenderState = (SetRenderState_t)PatchVTable(vt, 57, (void*)H_SetRenderState);
    t.setRenderTarget = (SetRenderTarget_t)PatchVTable(vt, 37, (void*)H_SetRenderTarget);
    t.setLight = (SetLight_t)PatchVTable(vt, 51, (void*)H_SetLight);
    t.setTexture = (SetTexture_t)PatchVTable(vt, 65, (void*)H_SetTexture);
    t.setTSS = (SetTSS_t)PatchVTable(vt, 67, (void*)H_SetTSS);
    t.setFVF = (SetFVF_t)PatchVTable(vt, 89, (void*)H_SetFVF);
    t.setVertexDecl = (SetVertexDecl_t)PatchVTable(vt, 87, (void*)H_SetVertexDecl);
    t.setStreamSource = (SetStreamSource_t)PatchVTable(vt, 100, (void*)H_SetStreamSource);
    t.setMaterial = (SetMaterial_t)PatchVTable(vt, 49, (void*)H_SetMaterial);
    t.drawPrimitive = (DrawPrimitive_t)PatchVTable(vt, 81, (void*)H_DrawPrimitive);
    t.drawIndexedPrimitive = (DrawIndexedPrimitive_t)PatchVTable(vt, 82, (void*)H_DrawIndexedPrimitive);
    t.drawPrimitiveUP = (DrawPrimitiveUP_t)PatchVTable(vt, 83, (void*)H_DrawPrimitiveUP);
    t.drawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)PatchVTable(vt, 84, (void*)H_DrawIndexedPrimitiveUP);
    g_vts[g_nvt++] = t;
    LogAlways("D3D9 device hooked (vtable %p)", (void*)vt);
}

static HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
    D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        LogAlways("CreateDevice ok (flags=0x%lX, pure=%d)", flags, (flags & D3DCREATE_PUREDEVICE) ? 1 : 0);
        g_curRT = NULL; g_worldRT = NULL; g_worldDrawn = false; g_persp = false;
        PatchDevice(*out);
    }
    return hr;
}

static DWORD WINAPI InitThread(LPVOID) {
    HMODULE h = LoadLibraryA("d3d9.dll");
    if (!h) { LogAlways("Couldn't load d3d9.dll"); return 0; }
    typedef IDirect3D9* (WINAPI* Create9_t)(UINT);
    Create9_t create = (Create9_t)GetProcAddress(h, "Direct3DCreate9");
    if (!create) { LogAlways("Direct3DCreate9 not found"); return 0; }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { LogAlways("Direct3DCreate9 returned NULL"); return 0; }
    void** vt = *(void***)d3d;
    g_origCreateDevice = (CreateDevice_t)PatchVTable(vt, 16, (void*)H_CreateDevice);
    d3d->Release();
    LogAlways("IDirect3D9::CreateDevice hooked; waiting for the game to create the device");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)inst;
        g_rs[D3DRS_ZWRITEENABLE] = 1;
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH];
        GetModuleFileNameA(g_self, path, MAX_PATH);
        char* dot = strrchr(path, '.');
        if (dot) *dot = 0;
        snprintf(g_iniPath, sizeof g_iniPath, "%s.ini", path);
        snprintf(g_logPath, sizeof g_logPath, "%s.log", path);
        DeleteFileA(g_logPath);
        LoadConfig();
        LogAlways("wow_clouds loaded (ini: %s)", g_iniPath);
        HANDLE th = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    return TRUE;
}