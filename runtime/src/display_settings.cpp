#include "det_clock.h"
#include "display_settings.h"

#include "memory.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

#include <cmath>
#include <mutex>

namespace DisplaySettings {
namespace {

// NSMBW (SMNE01 rev 1) EGG::Screen statics. sTVModeInfo holds one record per TV mode,
// {u16 width, u16 height, f32 scaleX = fbWidth / width, f32 scaleY = efbHeight / height}, written
// by EGG::Screen::Initialize from the game's own sizes; record 1 is 16:9 (832x456 canvas units).
// EGG::Screen::SetTVMode copies the current record into the root screen's canvas and flags it for
// rebuild, and NSMBW registers no TV-mode change callback, so doing those writes from here is what
// SetTVMode would do. 2D layouts (m2d), the HOME menu and depth of field read the record directly.
constexpr uint32_t kTVModeInfo = 0x803504E0u;  // EGG::Screen::sTVModeInfo
constexpr uint32_t kTVModeRecordSize = 0x0Cu;
constexpr uint32_t kTVModeWide = 1u;
constexpr uint32_t kTVMode = 0x8042AF08u;      // EGG::Screen::sTVMode
constexpr uint32_t kRootScreen = 0x8042AF0Cu;  // EGG::Screen::spRoot
constexpr uint32_t kFbWidth = 0x8042AF38u;     // EGG::StateGX::s_widthFb (u16, upper half of the word)
constexpr uint32_t kCanvasWidth = 0x08u;       // EGG::Screen: canvas width (f32)
constexpr uint32_t kScreenFlags = 0x34u;       // EGG::Screen: flags (u16), bit 0 = rebuild
// NSMBW's own copy of the 16:9 size {f32 width, f32 height}, taken from the record by d_screen.cpp's
// static initializer before EGG::Screen::Initialize runs. dScreen::GetScreenSize (the game's camera
// and screen-space maths) reads it, so it has to widen with the record.
constexpr uint32_t kGameScreenSize169 = 0x8042A058u;  // dScreen::l_ScreenSize16x9

constexpr float kLinesPerScale = 480.0f;
constexpr float kRatio16x9 = 16.0f / 9.0f;

std::mutex g_mutex;
Settings g_current;
Settings g_pending;
bool g_hasPending = false;
bool g_bootWidescreen = true;
bool g_rendererDirty = true;
uint32_t g_surfaceWidth = 0;
uint32_t g_surfaceHeight = 0;
uint16_t g_gameWideWidth = 0;  // the game's own 16:9 canvas width, captured before any widening

// The game booted in 4:3: neither widescreen mode exists until the next start.
int32_t EffectiveAspect(int32_t aspect) {
    return g_bootWidescreen ? aspect : kAspect4x3;
}

bool SurfaceWiderThan169() {
    return g_surfaceWidth != 0 && g_surfaceHeight != 0 &&
           static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight) > kRatio16x9;
}

void ApplyRenderer(const Settings& settings) {
    const int32_t aspect = EffectiveAspect(settings.aspect);
    if (aspect == kAspect4x3) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
    } else if (aspect == kAspect16x9 || !SurfaceWiderThan169()) {
        // 16:9, or Fill on a surface no wider than 16:9 (the picture is never squashed).
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_16_9);
    } else {
        // The framebuffer takes the surface's aspect; the game draws the wider view into it.
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    }
    VISetFrameBufferLines(LinesForScale(settings.renderScale));
    RT_LOGF(RT_TAG_VI, "display: aspect %d%s, render %.0f lines (%.2gx), surface %ux%u\n", aspect,
            aspect != settings.aspect ? " (game booted in 4:3)" : "", LinesForScale(settings.renderScale),
            settings.renderScale, g_surfaceWidth, g_surfaceHeight);
}

// Fill: widen the game's 16:9 canvas to the surface (Hor+), keeping the game's own pixel aspect
// (its 832 units stand for 16:9). Otherwise keep, or put back, the game's own width.
void ApplyGameCanvas(int32_t aspect) {
    uint32_t root = 0;
    if (!Memory::TryRead32(kRootScreen, root) || root == 0) {
        return;  // EGG::Screen::Initialize has not run yet
    }
    const uint32_t record = kTVModeInfo + kTVModeWide * kTVModeRecordSize;
    uint32_t size = 0;
    if (!Memory::TryRead32(record, size)) {
        return;
    }
    const auto width = static_cast<uint16_t>(size >> 16);
    if (g_gameWideWidth == 0) {
        g_gameWideWidth = width;
    }
    uint16_t target = g_gameWideWidth;
    if (aspect == kAspectFill && SurfaceWiderThan169()) {
        const float surfaceAspect = static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight);
        target = static_cast<uint16_t>(std::lround(g_gameWideWidth * surfaceAspect / kRatio16x9));
    }
    if (target == width) {
        return;
    }
    uint32_t fbWord = 0;
    Memory::TryRead32(kFbWidth, fbWord);
    const auto fbWidth = static_cast<float>(fbWord >> 16);
    Memory::Write16(record, target);
    if (fbWidth > 0.0f) {
        Memory::WriteFloat32(record + 4u, fbWidth / static_cast<float>(target));
    }
    Memory::WriteFloat32(kGameScreenSize169, static_cast<float>(target));
    uint32_t mode = 0;
    Memory::TryRead32(kTVMode, mode);
    if (mode == kTVModeWide) {
        uint32_t flagsWord = 0;
        Memory::TryRead32(root + kScreenFlags, flagsWord);
        Memory::WriteFloat32(root + kCanvasWidth, static_cast<float>(target));
        Memory::Write16(root + kScreenFlags, static_cast<uint16_t>((flagsWord >> 16) | 1u));
    }
    RT_LOGF(RT_TAG_VI, "display: 16:9 canvas %u -> %u units wide, fb %.0f px (surface %ux%u)\n", width,
            target, fbWidth, g_surfaceWidth, g_surfaceHeight);
}

} // namespace

float LinesForScale(float renderScale) {
    return renderScale > 0.0f ? kLinesPerScale * renderScale : 0.0f;
}

// Deterministic mode (network play) keeps every device on the same 16:9 canvas: how wide the game
// draws decides which enemies it wakes, so Fill's screen-dependent width would desync the session.
Settings Lockstep(Settings settings) {
    if (DetClock::Enabled()) {
        settings.aspect = kAspect16x9;
    }
    return settings;
}

void Initialize(const Settings& requested) {
    const Settings settings = Lockstep(requested);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_current = settings;
    g_bootWidescreen = settings.aspect != kAspect4x3;
    g_rendererDirty = true;
}

void Request(const Settings& settings) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = Lockstep(settings);
    g_hasPending = true;
}

void OnFrameBoundary(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    Settings settings;
    bool renderer = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hasPending) {
            g_current = g_pending;
            g_hasPending = false;
            g_rendererDirty = true;
        }
        if (surfaceWidth != 0 && surfaceHeight != 0 &&
            (surfaceWidth != g_surfaceWidth || surfaceHeight != g_surfaceHeight)) {
            const bool wasWide = SurfaceWiderThan169();
            g_surfaceWidth = surfaceWidth;
            g_surfaceHeight = surfaceHeight;
            // Fill switches between stretching and the 16:9 frame as the surface crosses 16:9.
            if (SurfaceWiderThan169() != wasWide) {
                g_rendererDirty = true;
            }
        }
        settings = g_current;
        renderer = g_rendererDirty;
        g_rendererDirty = false;
    }
    if (renderer) {
        ApplyRenderer(settings);
    }
    ApplyGameCanvas(EffectiveAspect(settings.aspect));
}

Settings Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_current;
}

bool GameWidescreen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bootWidescreen;
}

} // namespace DisplaySettings
