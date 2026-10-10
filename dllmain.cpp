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

    //-- Rain transition parameters --
    float rainCoverage = 1.0f;    // cloud cover with active weather (rain/snow/dust)
    float rainOpacity = 1.00f;    // cloud opacity with active weather
    float rainDarken = 0.55f;     // 1 = no change, lower = darker
    float rainBlendSpeed = 0.6f;  // how fast the transition occurs (per second)

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
    // Experimental read of CMapWeather+0x20 (see lighting.md). We suspect it caused the game to 
    // hang during a test; disabled by default until confirmed safe.
    int   wxRawProbe = 0;
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
    // 1 = the [WXRAW] log writes *every* frame (not every 60). It generates a very large log file; 
    // use it only for short, specific tests to see if the pulse lasts less than 60 frames.
    int   wxRawEveryFrame = 0;

    float windX = 0.030f, windY = 0.012f;
    float horizonFade = 0.20f;
    float sunAz = 45.0f, sunEl = 50.0f;   // degrees
    float quadDepth = 1.0f;
    float nightBrightness = 0.18f;
    float sunColor[3] = { 1.00f, 0.97f, 0.90f };
    float shadowColor[3] = { 0.52f, 0.60f, 0.78f };
};

static Config g_cfg;
static HMODULE g_self;
static char g_iniPath[MAX_PATH], g_logPath[MAX_PATH];
static FILETIME g_iniTime;
static DWORD g_lastIniCheck;

// -- Weather: detected by the rate of change of the sky color (g_skyVColor) --
enum { WX_UNKNOWN = -1, WX_CLEAR = 0, WX_RAIN = 1, WX_SNOW = 2, WX_SAND = 3 };
static volatile int g_weather = WX_CLEAR;
static float g_rainBlend = 0.0f;   // 0 = normal weather, 1 = full rain
static double g_lastBlendTime = -1.0;   // to measure the actual time between frames (independent of FPS)

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
static void ReadColor(const char* key, float out[3]) {
    char buf[96];
    GetPrivateProfileStringA("Clouds", key, "", buf, sizeof buf, g_iniPath);
    float r, g, b;
    if (sscanf(buf, "%f , %f , %f", &r, &g, &b) == 3) { out[0] = r; out[1] = g; out[2] = b; }
}

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

    // -- Rain transition stuff --
    c.rainCoverage = ReadF("RainCoverage", c.rainCoverage);
    c.rainOpacity = ReadF("RainOpacity", c.rainOpacity);
    c.rainDarken = ReadF("RainDarken", c.rainDarken);
    c.rainBlendSpeed = ReadF("RainBlendSpeed", c.rainBlendSpeed);
    c.dayTransparencyEnabled = (int)ReadF("DayTransparencyEnabled", (float)c.dayTransparencyEnabled);
    c.dayTransparencyAmount = ReadF("DayTransparencyAmount", c.dayTransparencyAmount);

    c.wxBaselineTau = ReadF("WxBaselineTau", c.wxBaselineTau);
    c.wxDevEnter = ReadF("WxDevEnter", c.wxDevEnter);
    c.wxDevExit = ReadF("WxDevExit", c.wxDevExit);
    c.wxConfirmSec = ReadF("WxConfirmSec", c.wxConfirmSec);
    c.wxJumpRate = ReadF("WxJumpRate", c.wxJumpRate);
    c.wxJumpWindow = ReadF("WxJumpWindow", c.wxJumpWindow);
    c.wxLog = (int)ReadF("WxLog", (float)c.wxLog);
    c.wxRawProbe = (int)ReadF("WxRawProbe", (float)c.wxRawProbe);
    c.wxUseSlots = (int)ReadF("WxUseSlots", (float)c.wxUseSlots);
    c.wxUsePulse = (int)ReadF("WxUsePulse", (float)c.wxUsePulse);
    c.wxRawEveryFrame = (int)ReadF("WxRawEveryFrame", (float)c.wxRawEveryFrame);

    c.windX = ReadF("WindX", c.windX);
    c.windY = ReadF("WindY", c.windY);
    c.horizonFade = ReadF("HorizonFade", c.horizonFade);
    c.sunAz = ReadF("SunAzimuth", c.sunAz);
    c.sunEl = ReadF("SunElevation", c.sunEl);
    c.quadDepth = ReadF("QuadDepth", c.quadDepth);
    c.nightBrightness = ReadF("NightBrightness", c.nightBrightness);
    ReadColor("SunColor", c.sunColor);
    ReadColor("ShadowColor", c.shadowColor);

    g_cfg = c;
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
        LogAlways("Configuracion recargada");
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
    float2 p = uv * P0.y + float2(P1.x, P1.y) * P0.x;

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

static double NowSec() {
    static LARGE_INTEGER f, t0;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
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
        float mix = g_cfg.tintMix;
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
    float dk = 1.0f - (1.0f - g_cfg.rainDarken) * g_rainBlend;
    for (int i = 0; i < 3; i++) { lit[i] *= dk; sh[i] *= dk; }
}

static bool NeedScreenSample() {
    int src = g_cfg.colorSource;
    if (src == 0) return false;
    if (src == 1) return true;
    if (src <= 4) return !g_cloudColorValid;
    return !g_skyVValid;
}

static bool DrawClouds(IDirect3DDevice9* dev, bool atSky) {
    if (!g_cfg.enabled || g_failed) return false;
    if (!g_haveCamView || !g_haveProj) return false;
    if (!EnsureResources(dev)) return false;

    // We only draw if the current render target has the same size as the back buffer and a depth buffer exists.
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
    bool ok = gotRT && gotBB && rd.Width == bd.Width && rd.Height == bd.Height && SUCCEEDED(zr) && zs;
    if (ok && !atSky && NeedScreenSample()) SampleSky(dev, rt, rd);
    if (rt) rt->Release();
    if (bb) bb->Release();
    if (zs) zs->Release();
    if (!ok) {
        if (g_dbgSkipLogs < 20) {
            g_dbgSkipLogs++;
            LogDbg("[frame %lu] DrawClouds skipped: rt=%ux%u bb=%ux%u esBackbuffer=%d zbuffer=0x%08lX", g_frame,
                (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)bd.Width, (unsigned)bd.Height, (int)isBB, (unsigned long)zr);
        }
        return false;
    }

    if (!g_sb) {
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) { g_sb = NULL; LogAlways("CreateStateBlock failed"); g_failed = true; return false; }
    }
    else {
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
    //float cov = g_cfg.coverage < 0 ? 0 : (g_cfg.coverage > 1 ? 1 : g_cfg.coverage);

    // -- Rain transition stuff start --
    float covBase = g_cfg.coverage + (g_cfg.rainCoverage - g_cfg.coverage) * g_rainBlend;
    float cov = covBase < 0 ? 0 : (covBase > 1 ? 1 : covBase);
    float opaBase = g_cfg.opacity + (g_cfg.rainOpacity - g_cfg.opacity) * g_rainBlend;
    // -- Rain transition stuff end --

    // Clouds on clear days are slightly more transparent. The factor cancels itself out (returning to 1—i.e., 
    // normal opacity) at night (g_dayness->0) and during any active weather (g_rainBlend->1), so it never 
    // conflicts with the weather effect mentioned above.
    float dayTransparencyFactor = 1.0f - g_cfg.dayTransparencyAmount * g_dayness * (1.0f - g_rainBlend);
    opaBase *= dayTransparencyFactor;

    float thr = 0.74f + (0.36f - 0.74f) * cov;
    float t = (float)fmod(NowSec() * g_cfg.timeScale, 36000.0);
    float lit[3], sh[3];
    ComputeColors(lit, sh);
    float psc[36] = {
        t, g_cfg.scale, thr, g_cfg.softness < 0.01f ? 0.01f : g_cfg.softness,
        g_cfg.windX, g_cfg.windY, opaBase, g_cfg.horizonFade,
        cosf(az) * sunLen, sinf(az) * sunLen, g_cfg.testMode ? 1.0f : 0.0f, g_cfg.dome,
        up[0], up[1], up[2], 0,
        e1[0], e1[1], e1[2], 0,
        e2[0], e2[1], e2[2], 0,
        lit[0], lit[1], lit[2], 1,
        sh[0], sh[1], sh[2], 1,
        g_cfg.evolve, g_cfg.detail, g_cfg.bandSoft < 0.005f ? 0.005f : g_cfg.bandSoft, 0,
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
    dev->SetPixelShaderConstantF(0, psc, 9);

    static const float quad[8] = { -1, -1, -1, 1, 1, -1, 1, 1 };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 2 * sizeof(float));

    g_sb->Apply();
    g_inDraw = false;

    if (g_dbgDrawLogs < 20) {
        g_dbgDrawLogs++;
        LogDbg("[frame %lu] Clouds drawn (%s) on rt %ux%u (esBackbuffer=%d) fog=%06lX ambient=%08lX light0=(%.2f %.2f %.2f) lit=(%.2f %.2f %.2f) shadow=(%.2f %.2f %.2f)",
            g_frame, atSky ? "sky layer" : "end of the world", (unsigned)rd.Width, (unsigned)rd.Height, (int)isBB, g_fogLast, g_ambient,
            g_light0[0], g_light0[1], g_light0[2], lit[0], lit[1], lit[2], sh[0], sh[1], sh[2]);
    }
    return true;
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
    if (!g_worldDrawn || g_drawnThisFrame || !g_haveCamView || !g_haveProj) return;
    if (CurRT(d) != g_worldRT) return;
    if (DrawClouds(d, false)) g_drawnThisFrame = true;
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
    if (g_inDraw || !g_persp) return false;
    g_worldRT = CurRT(d);
    g_worldDrawn = true;
    g_worldDraws++;
    g_worldPrims += di.prims;
    if (g_skyEnded) return false;

    bool dumping = g_cfg.skyDump && g_dbgDumpFrames < 6 && (g_frame % 600 == 300 || g_frame % 600 == 301);
    if (g_rs[D3DRS_ZWRITEENABLE]) {   // First draw call that writes depth = terrain/objects begin.
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
    if (isCloud && g_cfg.layer == 1) g_pendingSkyClouds = true;   // our clouds take shape right after this call.

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
    if (!g_drawnThisFrame && DrawClouds(d, true)) g_drawnThisFrame = true;
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
    if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;   // primer frame tras una pausa larga

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

static HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    VT* t = FindVT(d);
    if (g_cfg.skyDump && g_skyOrd > 0 && (g_frame % 600 == 300 || g_frame % 600 == 301)) g_dbgDumpFrames++;
    g_frame++;
    if (g_worldDrawn && !g_drawnThisFrame && g_dbgAttnLogs < 20) {
        g_dbgAttnLogs++;
        LogDbg("[frame %lu] WARNING: the world was drawn, but the clouds were not drawn.", g_frame);
    }
    g_worldDrawn = false;
    g_drawnThisFrame = false;
    g_persp = false;
    g_skyOrd = 0;
    g_skyEnded = false;
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

    if (g_cfg.wxRawProbe && g_cfg.wxLog) {
        bool shouldLog = g_cfg.wxRawEveryFrame || (g_frame % 60) == 0;
        if (shouldLog) {
            int raw20 = ReadClientWeatherTypeRaw();
            int slots = ReadClientWeatherSlots();
            LogAlways("[WXRAW] frame=%lu +0x20=%d slots=%d sustained_pulse=%d g_weather=%d using=%s | dayness=%.2f",
                (unsigned long)g_frame, raw20, slots, g_wxPulseHeld, g_weather, wxSource,
                g_dayness);
        }
    }

    // The effect (coverage/opacity/darkening) applies equally to rain, snow, and dust;
    // Only clearing the sky leaves the clouds in their normal state.
    float rainTarget = (g_weather == WX_RAIN || g_weather == WX_SNOW || g_weather == WX_SAND) ? 1.0f : 0.0f;
    float dt = (g_lastBlendTime < 0.0) ? (1.0f / 60.0f) : (float)(now - g_lastBlendTime);
    g_lastBlendTime = now;
    if (dt < 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;   // first frame or long pause: use a reasonable value
    float step = g_cfg.rainBlendSpeed * dt;
    if (g_rainBlend < rainTarget) { g_rainBlend += step; if (g_rainBlend > rainTarget) g_rainBlend = rainTarget; }
    else if (g_rainBlend > rainTarget) { g_rainBlend -= step; if (g_rainBlend < rainTarget) g_rainBlend = rainTarget; }

    MaybeReloadConfig();
    return t->present(d, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    VT* t = FindVT(d);
    if (g_sb) { g_sb->Release(); g_sb = NULL; }   // the state blocks must be released before Reset.
    g_curRT = NULL; g_worldRT = NULL;
    ReleaseSampleBuffers();   // DEFAULT pool surfaces: must be released before Reset
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