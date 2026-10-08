#include "netplay_ui.h"

#include "netplay/game_layout.h"
#include "netplay/game_menus.h"
#include "netplay_lobby.h"
#include "netplay_session.h"
#include "netplay_start.h"
#include "runtime_log.h"

#include "aurora_events.h"
#include "hle_stubs.h"
#include "display_settings.h"
#include "keybinds.h"
#include "settings_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Defined in hle/vi.cpp: whether an aurora frame is open for drawing.
extern std::atomic_bool g_auroraFrameActive;

namespace NetplayUi {
namespace {

using GameLayout::Utf16;
using Phase = NetplayLobby::Phase;

// WPAD_BUTTON_* bits.
constexpr uint32_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kPlus = 0x0010, kTwo = 0x0100,
                   kOne = 0x0200, kB = 0x0400, kA = 0x0800, kMinus = 0x1000;
// The same D-pad by screen direction, for a remote held sideways (D-pad under the left thumb).
constexpr uint32_t kScreenLeft = kUp, kScreenRight = kDown, kScreenUp = kRight, kScreenDown = kLeft;

// None is Couch, the game's own screens. Every other screen is part of LAN play.
enum class Screen { None, Lan, ChooseFile, Count, Rooms, Room, Message, Settings };

constexpr uint16_t kSceneBoot = 0x000;       // the strap screen ("Hold the Wii Remote sideways")
constexpr uint16_t kSceneStage = 0x005;      // a course, or the title screen's
constexpr uint16_t kSceneWorldMap = 0x003;
constexpr uint16_t kSceneGameSetup = 0x00A;  // the file select and "Select Players"
constexpr uint16_t kProfileOpeningTitle = 0x2BB;  // EVENT_OPENING_TITLE: only on the title screen

enum Event : uint32_t {
    kEvLeft = 1, kEvRight = 2, kEvUp = 4, kEvDown = 8, kEvConfirm = 16, kEvBack = 32,
};

struct Ui {
    Screen screen = Screen::None;
    int cursor = 0;
    bool toggleShown = false;    // the LAN / Couch toggle is on the file select right now
    bool toggleFocus = false;    // the cursor is on the top bar (moved up from the files): the remote is ours
    int topItem = 0;             // which of the top bar: 0 the Couch / LAN toggle, 1 the settings gear
    uint32_t prevHold = 0;       // channel 1's buttons at the last read
    uint32_t events = 0;         // menu events since the last frame
    bool swallowConfirm = false; // keep a confirm press from the game until it is released
    uint32_t heldFromMenu = 0;   // buttons held when a menu handed the remote back: hidden till released
    bool countPicked = false;    // Create Room: the player confirmed a number on the game's screen
    std::string message;
    Screen afterMessage = Screen::Lan;
    int frame = 0;               // frames the current screen has been up
    int countHintFrames = 0;
    bool restarting = false;
    uint32_t injectButtons = 0;  // buttons the game is given for a few reads (a Back press)
    int injectReads = 0;
    bool pauseGearShown = false;  // a pause menu is open: the settings gear sits at the top right
    bool settingsFromPause = false;
};

Ui g_ui;

// ---------------------------------------------------------------- touch and mouse
// These windows' buttons as last drawn, in layout units: a tap on one picks it as the remote would.
struct Hit {
    float x, y, w, h;
    int cursor;      // -1: leave the cursor as it is
    uint32_t event;  // the menu event a tap makes (Event, below)
};
std::vector<Hit> g_hits;

void AddHit(float x, float y, float w, float h, int cursor, bool back = false) {
    g_hits.push_back(Hit{x, y, w, h, cursor, back ? static_cast<uint32_t>(kEvBack) : static_cast<uint32_t>(kEvConfirm)});
}

void AddHitEvent(float x, float y, float w, float h, int cursor, uint32_t event) {
    g_hits.push_back(Hit{x, y, w, h, cursor, event});
}

std::atomic<uint64_t> g_tap{0};  // a tap for the next frame: bit 63, then x and y as 16-bit fractions
std::atomic<bool> g_tapScreen{false};

// A tap on the game's own screens is played back as remote presses on channel 1: one press, or
// D-pad steps toward a cursor position (checked against the game's cursor before each) and then
// 2 to pick it.
enum class NavKind { None, Press, File, Players };
struct Nav {
    NavKind kind = NavKind::None;
    int target = 0;           // the cursor position, or for Press the buttons
    uint32_t button = 0;      // what is pressed during this step
    int reads = 0;
    int steps = 0;
    bool confirming = false;
};
Nav g_nav;

void StartNav(NavKind kind, int target) {
    static const char* const kKinds[] = {"none", "press", "file select", "select players"};
    RT_LOGF(RT_TAG_RUNTIME, "tap: %s 0x%X\n", kKinds[static_cast<int>(kind)], target);
    g_nav = Nav{};
    g_nav.kind = kind;
    g_nav.target = target;
}

uint32_t NextNavButton(Nav& n) {
    if (n.kind == NavKind::Press) {
        if (n.steps++ == 0) {
            return static_cast<uint32_t>(n.target);
        }
        n.kind = NavKind::None;
        return 0;
    }
    if (n.confirming || ++n.steps > 16) {
        n.kind = NavKind::None;
        return 0;
    }
    int cursor = 0;
    int row = 0, col = 0, targetRow = 0, targetCol = 0;
    if (n.kind == NavKind::File) {
        if (!GameMenus::FileSelectWaiting()) {
            return 0;  // still moving: wait
        }
        // 0-2 the files in a row, 3 and 4 Free-for-All and Coin Battle below them.
        cursor = GameMenus::FileSelectRawCursor();
        row = cursor <= 2 ? 0 : 1;
        col = cursor <= 2 ? cursor : cursor - 3;
        targetRow = n.target <= 2 ? 0 : 1;
        targetCol = n.target <= 2 ? n.target : n.target - 3;
    } else {
        if (!GameMenus::SelectPlayersSettled()) {
            return 0;
        }
        // 0 "1 Player" on top, 1-3 "2-4 Players" in a row below it.
        cursor = GameMenus::SelectPlayersCursor();
        row = cursor == 0 ? 0 : 1;
        col = cursor == 0 ? 0 : cursor - 1;
        targetRow = n.target == 0 ? 0 : 1;
        targetCol = n.target == 0 ? 0 : n.target - 1;
    }
    RT_LOGF(RT_TAG_RUNTIME, "tap: cursor %d, going to %d\n", cursor, n.target);
    if (cursor == n.target) {
        n.confirming = true;
        return kTwo;
    }
    if (row != targetRow) {
        return targetRow > row ? kScreenDown : kScreenUp;
    }
    return targetCol > col ? kScreenRight : kScreenLeft;
}

// Once per read of channel 1: the buttons the current tap holds now (4 reads down, 4 up per step).
uint32_t NavButtons() {
    Nav& n = g_nav;
    if (n.kind == NavKind::None) {
        return 0;
    }
    const int phase = n.reads++ % 8;
    if (phase == 0) {
        n.button = NextNavButton(n);
    }
    return phase < 4 ? n.button : 0;
}

struct Assets {
    bool tried = false;
    GameLayout::Layout yesNo;
    GameLayout::Layout cursor;
    std::shared_ptr<GameLayout::Font> message;
    std::shared_ptr<GameLayout::Font> mario;
};

Assets& GetAssets() {
    static Assets assets;
    if (!assets.tried) {
        assets.tried = true;
        assets.yesNo.Load("/Layout/yesnoWindow/yesnoWindow.arc");
        assets.cursor.Load("/Layout/select_cursor/select_cursor.arc");
        assets.message = GameLayout::LoadFont("mj2d00_MessageFont_32_I4.brfnt");
        assets.mario = GameLayout::LoadFont("mj2d01_marioFont_64_IA4.brfnt");
    }
    return assets;
}

// The steps of Create Room that happen on the game's own screens keep the remote with the game.
bool IsModal(Screen screen) {
    return screen != Screen::None && screen != Screen::ChooseFile && screen != Screen::Count;
}

const char* ScreenName(Screen screen) {
    switch (screen) {
    case Screen::None: return "couch";
    case Screen::Lan: return "Join or Create Room";
    case Screen::ChooseFile: return "room save file (game screen)";
    case Screen::Count: return "room size (game screen)";
    case Screen::Rooms: return "rooms";
    case Screen::Room: return "room";
    case Screen::Message: return "message";
    case Screen::Settings: return "settings";
    }
    return "?";
}

void Show(Screen screen, int cursor = 0) {
    if (screen != g_ui.screen) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay ui: %s -> %s\n", ScreenName(g_ui.screen), ScreenName(screen));
    }
    // Handing the remote back to the game: a button still held from the menu must not reach it as a
    // fresh press (the game would take the menu's confirm as its own).
    if (IsModal(g_ui.screen) && !IsModal(screen)) {
        g_ui.heldFromMenu = g_ui.prevHold;
    }
    g_ui.screen = screen;
    g_ui.cursor = cursor;
    g_ui.frame = 0;
    g_ui.events = 0;
}

void ShowMessage(const std::string& text, Screen after) {
    g_ui.message = text;
    g_ui.afterMessage = after;
    Show(Screen::Message);
}

// Menus that own the remote: the game sees it at rest.
bool Modal() {
    return IsModal(g_ui.screen);
}

// ---------------------------------------------------------------- drawing helpers
struct Canvas {
    ImDrawList* list;
    GameLayout::View view;
    float alpha;
};

void Veil(const Canvas& c) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    c.list->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, static_cast<int>(110 * c.alpha)));
}

// The yes/no window, `height` layout units tall (322 as authored), without its texts and buttons.
void Window(const Canvas& c, float height, float width = 528.0f) {
    Assets& a = GetAssets();
    GameLayout::DrawParams p;
    p.alpha = c.alpha;
    p.visible["T_questionS_00"] = false;
    p.visible["N_button_00"] = false;
    p.visible["N_otehonText_00"] = false;
    p.paneSize["P_bg_00"] = {width, height};
    p.paneSize["P_stripe_00"] = {width + 2, height};
    p.paneSize["W_yesnoWindow_00"] = {width + 14, height + 8};
    a.yesNo.Draw(c.list, c.view, "N_yesnoWindow_00", p);
}

// The game's message text: white with its dark edge, centred on (x, y) unless told otherwise.
void Text(const Canvas& c, const std::string& text, float x, float y, float size = 30.0f, int originH = 1,
          uint32_t color = 0xFFFFFFFFu) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.message.get())) {
        return;
    }
    const std::u16string t = Utf16(text);
    GameLayout::DrawText(c.list, c.view, *a.message, t, x + 2.5f, y - 2.5f, size, 0x30200AFFu, 0x30200AFFu,
                         0.55f * c.alpha, originH, 1);
    GameLayout::DrawText(c.list, c.view, *a.message, t, x, y, size, color, color, c.alpha, originH, 1);
}

// A title in the game's Mario font (yellow to orange), like "Select Players".
void Title(const Canvas& c, const std::string& text, float x, float y, float size = 44.0f) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.mario.get())) {
        return;
    }
    GameLayout::DrawText(c.list, c.view, *a.mario, Utf16(text), x, y, size, 0xFFFF00FFu, 0xFF8C00FFu, c.alpha, 1, 1);
}

float Pulse(int frame) {
    return 0.5f + 0.5f * std::sin(frame * 0.12f);
}

// The game's selection cursor (four yellow corners) around a w x h box centred on (x, y).
void Cursor(const Canvas& c, float x, float y, float w, float h, int frame) {
    Assets& a = GetAssets();
    const float out = 3.0f * Pulse(frame);
    struct Corner { const char* pane; float ax, ay, sx, sy; };
    static constexpr Corner kCorners[] = {
        {"N_LU_00", -67, 42, -1, 1}, {"N_RU_00", 67, 42, 1, 1}, {"N_LD_00", -67, -42, -1, -1}, {"N_RD_00", 67, -42, 1, -1}};
    for (const auto& corner : kCorners) {
        GameLayout::DrawParams p;
        p.alpha = c.alpha;
        p.offsetX = x + corner.sx * (w / 2 + out) - corner.ax;
        p.offsetY = y + corner.sy * (h / 2 + out) - corner.ay;
        a.cursor.Draw(c.list, c.view, corner.pane, p);
    }
}

// A green-to-yellow button pill like the game's yes/no buttons, w x 53 units, centred on (x, y).
void Button(const Canvas& c, const std::string& label, float x, float y, float w, bool selected, bool enabled,
            int frame, float textSize = 30.0f, float h = 53.0f) {
    Assets& a = GetAssets();
    GameLayout::DrawParams p;
    p.alpha = c.alpha * (enabled ? 1.0f : 0.55f);
    p.offsetX = x;
    p.offsetY = y + 118.0f;  // the pane is authored at (0, -118)
    p.scale = selected ? 1.0f + 0.04f * Pulse(frame) : 0.94f;
    p.paneScale["P_centerBase_00"] = 1.0f;  // authored at 0: it grows in through an animation
    p.paneSize["P_centerBase_00"] = {w, h};
    p.paneSize["W_button_02"] = {w + 5.0f, h + 10.0f};
    p.paneSize["P_shadow_02"] = {w, h};
    p.visible["T_center_00"] = false;
    p.visible["T_center_01"] = false;
    if (!enabled) {
        p.vertexTint["P_centerBase_00"] = 0xB0B0B0FFu;
    }
    a.yesNo.Draw(c.list, c.view, "P_centerBase_00", p);
    const float scale = p.scale;
    Canvas t = c;
    t.alpha = p.alpha;
    Text(t, label, x, y, textSize * scale);
    if (selected) {
        Cursor(c, x, y, w * scale + 6, h * scale + 6, frame);
    }
}

// Two side-by-side buttons at the bottom of a window `height` tall.
void ButtonPair(const Canvas& c, const std::string& left, const std::string& right, float height, int cursor,
                bool leftEnabled = true, bool rightEnabled = true) {
    const float y = -height / 2 + 43.0f;
    Button(c, left, -126, y, 232, cursor == 0, leftEnabled, g_ui.frame);
    Button(c, right, 126, y, 232, cursor == 1, rightEnabled, g_ui.frame);
    AddHit(-126, y, 240, 60, 0);
    AddHit(126, y, 240, 60, 1);
}

std::string PlayerLine(uint8_t slot, const std::string& name, bool you) {
    return std::to_string(slot + 1) + "P  " + name + (you ? "  (you)" : "");
}

// ---------------------------------------------------------------- screens
// The LAN / Couch toggle in the empty right half of the file select's title bar: one of the game's
// green buttons, reached by moving up from the top file. Couch is the game as it is; pressing it
// switches to LAN (Join or Create Room), and back to Couch from any LAN step on the file select.
constexpr float kToggleX = 214.0f, kToggleY = 192.0f, kToggleW = 166.0f;
constexpr float kGearY = kToggleY;

constexpr float kGearX = kToggleX + kToggleW / 2 + 46.0f, kGearW = 62.0f;

ImVec2 ToScreen(const Canvas& c, float x, float y) {
    return ImVec2(c.view.centerX + x * c.view.scale, c.view.centerY - y * c.view.scale);
}

// A cog: eight teeth around a ring, drawn in the dark of the game's text edges.
void Gear(const Canvas& c, float x, float y, float r) {
    const ImVec2 centre = ToScreen(c, x, y);
    const float s = c.view.scale;
    const ImU32 color = IM_COL32(70, 62, 40, static_cast<int>(235 * c.alpha));
    for (int i = 0; i < 8; ++i) {
        const float a = i * 3.14159265f / 4.0f;
        const float ca = std::cos(a), sa = std::sin(a);
        const float in = r * 0.72f * s, out = r * 1.18f * s, half = r * 0.26f * s;
        c.list->AddQuadFilled(ImVec2(centre.x + ca * in - sa * half, centre.y + sa * in + ca * half),
                              ImVec2(centre.x + ca * out - sa * half, centre.y + sa * out + ca * half),
                              ImVec2(centre.x + ca * out + sa * half, centre.y + sa * out - ca * half),
                              ImVec2(centre.x + ca * in + sa * half, centre.y + sa * in - ca * half), color);
    }
    c.list->AddCircle(centre, r * 0.62f * s, color, 24, r * 0.42f * s);
}

// The gear over the game's pause menus (the world map's + menu, a level's pause window): at the
// screen's top right, under the FPS counter.
constexpr float kPauseGearY = 120.0f;
float PauseGearX(const GameLayout::View& view) {
    return view.centerX / view.scale - 52.0f;
}

void DrawPauseGear(const Canvas& c) {
    const float x = PauseGearX(c.view);
    Button(c, "", x, kPauseGearY, kGearW, false, true, g_ui.frame);
    Gear(c, x, kPauseGearY, 13.0f);
}

void DrawToggle(const Canvas& c) {
    const bool lan = g_ui.screen != Screen::None;
    Text(c, "Play:", kToggleX - kToggleW / 2 - 12, kToggleY, 24, 2, 0xFFFFFFFFu);
    Button(c, lan ? "LAN" : "Couch", kToggleX, kToggleY, kToggleW, g_ui.toggleFocus && g_ui.topItem == 0, true,
           g_ui.frame);
    Button(c, "", kGearX, kGearY, kGearW, g_ui.toggleFocus && g_ui.topItem == 1, true, g_ui.frame);
    Gear(c, kGearX, kGearY, 13.0f);
}

// ---------------------------------------------------------------- settings
// Two tabs. Video: "Nx" renders N x 480 lines in a 16:9 picture (black bars beside it on a wider
// screen), "Nx Ultrawide" (offered on screens wider than 16:9) the same lines across the whole
// screen with the game's view widened, and Match Screen Resolution the screen's own pixels across
// the whole screen (the multiplier is greyed out meanwhile). Keybinds: the keyboard's remote.
constexpr float kScales[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
std::atomic<bool> g_settingsChanged{false};  // the Android app copies them into its own settings
int g_settingsTab = 0;                       // 0 Video, 1 Keybinds
float g_lastScale = 1.0f;                    // the multiplier Match Screen Resolution returns to
bool g_lastWide = true;

// Cursor rows. Video: 0 tabs, 1 resolution, 2 match screen, 3 FPS, 4 OK. Keybinds: 0 tabs,
// 1-12 the controls (two columns of six), 13 Reset Defaults, 14 OK.
constexpr int kVideoOk = 4;
constexpr int kKeyRows = 6;
constexpr int kKeyReset = 1 + Keybinds::kActionCount, kKeyOk = kKeyReset + 1;

struct VideoChoice {
    float scale;
    bool wide;
};

bool CurrentWide() {
    return settings_overlay::Aspect() == DisplaySettings::kAspectFill;
}

std::vector<VideoChoice> VideoChoices() {
    std::vector<VideoChoice> choices;
    for (const float scale : kScales) {
        choices.push_back({scale, false});
        if (DisplaySettings::SurfaceWide()) {
            choices.push_back({scale, true});
        }
    }
    return choices;
}

int CurrentChoice(const std::vector<VideoChoice>& choices) {
    const float current = settings_overlay::RenderScale();
    const float scale = current > 0.0f ? current : g_lastScale;
    const bool wide = (current > 0.0f ? CurrentWide() : g_lastWide) && DisplaySettings::SurfaceWide();
    int best = -1;
    for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
        if (std::fabs(choices[i].scale - scale) < 0.01f && choices[i].wide == wide) {
            best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            if (std::fabs(choices[i].scale - 1.0f) < 0.01f && choices[i].wide == wide) {
                best = i;
            }
        }
    }
    return std::max(best, 0);
}

std::string ChoiceLabel(const VideoChoice& choice) {
    char text[40];
    if (choice.wide) {
        std::snprintf(text, sizeof(text), "%gx Ultrawide", choice.scale);
    } else {
        std::snprintf(text, sizeof(text), "%gx (%dp)", choice.scale,
                      static_cast<int>(std::lround(480.0f * choice.scale)));
    }
    return text;
}

void ApplyVideo(float scale, bool wide) {
    if (settings_overlay::Aspect() != DisplaySettings::kAspect4x3) {
        settings_overlay::SetAspect(wide ? DisplaySettings::kAspectFill : DisplaySettings::kAspect16x9);
    }
    settings_overlay::SetRenderScale(scale);
    g_settingsChanged = true;
}

void StepVideo(int direction, bool wrap) {
    if (settings_overlay::RenderScale() <= 0.0f) {
        return;  // greyed out: Match Screen Resolution decides
    }
    const std::vector<VideoChoice> choices = VideoChoices();
    const int count = static_cast<int>(choices.size());
    int next = CurrentChoice(choices) + direction;
    if (next >= count) {
        next = wrap ? 0 : count - 1;
    } else if (next < 0) {
        next = wrap ? count - 1 : 0;
    }
    g_lastScale = choices[next].scale;
    g_lastWide = choices[next].wide;
    ApplyVideo(choices[next].scale, choices[next].wide);
}

void ToggleMatchScreen() {
    const float scale = settings_overlay::RenderScale();
    if (scale <= 0.0f) {
        ApplyVideo(g_lastScale, g_lastWide && DisplaySettings::SurfaceWide());
    } else {
        g_lastScale = scale;
        g_lastWide = CurrentWide();
        ApplyVideo(0.0f, true);
    }
}

void Arrow(const Canvas& c, float x, float y, bool right, bool selected, bool enabled = true) {
    const ImVec2 p = ToScreen(c, x, y);
    const float s = c.view.scale * (selected ? 1.0f + 0.12f * Pulse(g_ui.frame) : 1.0f);
    const float d = right ? 1.0f : -1.0f;
    const float alpha = c.alpha * (enabled ? 1.0f : 0.45f);
    const ImU32 fill = enabled ? IM_COL32(255, 230, 60, static_cast<int>(255 * alpha))
                               : IM_COL32(170, 170, 170, static_cast<int>(255 * alpha));
    const ImU32 edge = IM_COL32(48, 32, 10, static_cast<int>(220 * alpha));
    const ImVec2 a(p.x + d * 14 * s, p.y), b(p.x - d * 10 * s, p.y - 15 * s), e(p.x - d * 10 * s, p.y + 15 * s);
    c.list->AddTriangleFilled(a, b, e, fill);
    c.list->AddTriangle(a, b, e, edge, 2.5f * c.view.scale);
}

void Checkbox(const Canvas& c, float x, float y, bool on) {
    const ImVec2 p = ToScreen(c, x, y);
    const float s = c.view.scale, h = 15.0f * s;
    c.list->AddRectFilled(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), IM_COL32(255, 255, 255, static_cast<int>(240 * c.alpha)), 5 * s);
    c.list->AddRect(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), IM_COL32(48, 32, 10, static_cast<int>(230 * c.alpha)), 5 * s, 0, 3 * s);
    if (on) {
        const ImVec2 points[] = {ImVec2(p.x - 9 * s, p.y), ImVec2(p.x - 2 * s, p.y + 8 * s), ImVec2(p.x + 10 * s, p.y - 9 * s)};
        c.list->AddPolyline(points, 3, IM_COL32(40, 160, 40, static_cast<int>(255 * c.alpha)), 0, 5 * s);
    }
}

constexpr float kSettingsHeight = 440.0f, kSettingsWidth = 720.0f;

void DrawSettingsTabs(const Canvas& c) {
    const float y = kSettingsHeight / 2 - 42.0f;
    const char* names[] = {"Video", "Keybinds"};
    for (int tab = 0; tab < 2; ++tab) {
        const float x = tab == 0 ? -125.0f : 125.0f;
        Button(c, names[tab], x, y, 220, g_ui.cursor == 0 && g_settingsTab == tab, g_settingsTab == tab, g_ui.frame);
        AddHitEvent(x, y, 228, 60, 0, tab == 0 ? kEvLeft : kEvRight);
    }
}

void DrawVideoTab(const Canvas& c) {
    const float scale = settings_overlay::RenderScale();
    const bool match = scale <= 0.0f;
    const std::vector<VideoChoice> choices = VideoChoices();
    const VideoChoice shown = choices[CurrentChoice(choices)];
    Text(c, "Resolution", -300, 95, 26, 0, match ? 0xA0A0A0FFu : 0xFFFFFFFFu);
    Button(c, ChoiceLabel(shown), 105, 95, 270, g_ui.cursor == 1, !match, g_ui.frame);
    Arrow(c, -50, 95, false, g_ui.cursor == 1, !match);
    Arrow(c, 260, 95, true, g_ui.cursor == 1, !match);
    if (!match) {
        AddHitEvent(-50, 95, 54, 62, 1, kEvLeft);
        AddHitEvent(260, 95, 54, 62, 1, kEvRight);
        AddHitEvent(105, 95, 276, 62, 1, kEvRight);
    }
    Button(c, "Match Screen Resolution", 24, 25, 470, g_ui.cursor == 2, true, g_ui.frame);
    Checkbox(c, -185, 25, match);
    AddHit(0, 25, 480, 62, 2);
    Button(c, "Show FPS Counter", 24, -45, 470, g_ui.cursor == 3, true, g_ui.frame);
    Checkbox(c, -185, -45, settings_overlay::ShowFps());
    AddHit(0, -45, 480, 62, 3);
    const char* hint = match ? "The screen's own resolution, filling the whole screen."
                       : !DisplaySettings::SurfaceWide() ? "Rendered at this many lines, then scaled to the screen."
                       : shown.wide                      ? "Fills the whole screen with a wider view."
                                                         : "16:9, with black bars at the sides.";
    Text(c, hint, 0, -108, 21);
    Button(c, "OK", 0, -kSettingsHeight / 2 + 43.0f, 232, g_ui.cursor == kVideoOk, true, g_ui.frame);
    AddHit(0, -kSettingsHeight / 2 + 43.0f, 240, 60, kVideoOk);
}

void DrawKeybindsTab(const Canvas& c) {
    const int capturing = Keybinds::Capturing();
    for (int action = 0; action < Keybinds::kActionCount; ++action) {
        const int column = action / kKeyRows, row = action % kKeyRows;
        const float x0 = column == 0 ? -178.0f : 178.0f;
        const float y = 122.0f - 44.0f * row;
        Text(c, Keybinds::ActionName(action), x0 - 168, y, 21, 0);
        const std::string label = capturing == action ? "Press a key..." : Keybinds::KeyLabel(action);
        const float size = label.size() > 16 ? 17.0f : label.size() > 11 ? 19.0f : 22.0f;
        Button(c, label, x0 + 78, y, 172, g_ui.cursor == 1 + action, true, g_ui.frame, size, 38.0f);
        AddHit(x0 + 78, y, 178, 42, 1 + action);
    }
#ifdef __ANDROID__
    const char* note = "For a keyboard plugged into the device.";
#else
    const char* note = "Controllers: F10 > Controls.";
#endif
    Text(c, capturing >= 0 ? "Press the new key (Esc cancels)." : note, 0, -140, 20);
    const float y = -kSettingsHeight / 2 + 43.0f;
    Button(c, "Reset Defaults", -126, y, 232, g_ui.cursor == kKeyReset, true, g_ui.frame, 26.0f);
    Button(c, "OK", 126, y, 232, g_ui.cursor == kKeyOk, true, g_ui.frame);
    AddHit(-126, y, 240, 60, kKeyReset);
    AddHit(126, y, 240, 60, kKeyOk);
}

void DrawSettings(const Canvas& c) {
    Window(c, kSettingsHeight, kSettingsWidth);
    DrawSettingsTabs(c);
    if (g_settingsTab == 0) {
        DrawVideoTab(c);
    } else {
        DrawKeybindsTab(c);
    }
}

void CloseSettings() {
    Show(Screen::None);
    if (g_ui.settingsFromPause) {
        g_ui.settingsFromPause = false;  // back to the pause menu, as it was
        return;
    }
    g_ui.toggleFocus = true;  // back to the file select, the cursor on the gear
    g_ui.topItem = 1;
}

void HandleSettings(bool left, bool right, bool up, bool down, bool confirm, bool back) {
    if (Keybinds::Capturing() >= 0) {
        Keybinds::PollCapture();  // the keyboard is the capture's until a key is picked
        return;
    }
    int& cursor = g_ui.cursor;
    if (cursor == 0) {
        if (left || right) {
            g_settingsTab = left ? 0 : 1;
        } else if (confirm) {
            g_settingsTab ^= 1;
        } else if (down) {
            cursor = g_settingsTab == 0 && settings_overlay::RenderScale() <= 0.0f ? 2 : 1;
        } else if (back) {
            CloseSettings();
        }
        return;
    }
    if (back) {
        CloseSettings();
        return;
    }
    if (g_settingsTab == 0) {
        const bool match = settings_overlay::RenderScale() <= 0.0f;
        if (up) cursor = cursor == 2 && match ? 0 : cursor - 1;
        if (down) cursor = std::min(kVideoOk, cursor + 1);
        if (cursor == 1 && (left || right || confirm)) {
            StepVideo(left ? -1 : 1, confirm);
        } else if (confirm && cursor == 2) {
            ToggleMatchScreen();
        } else if (confirm && cursor == 3) {
            settings_overlay::SetShowFps(!settings_overlay::ShowFps());
            g_settingsChanged = true;
        } else if (confirm && cursor == kVideoOk) {
            CloseSettings();
        }
        return;
    }
    if (cursor >= kKeyReset) {
        if (left || right) cursor = cursor == kKeyReset ? kKeyOk : kKeyReset;
        if (up) cursor = cursor == kKeyReset ? kKeyRows : 2 * kKeyRows;
        if (confirm && cursor == kKeyReset) Keybinds::ResetDefaults();
        if (confirm && cursor == kKeyOk) CloseSettings();
        return;
    }
    const int action = cursor - 1, column = action / kKeyRows, row = action % kKeyRows;
    if (up) cursor = row == 0 ? 0 : cursor - 1;
    if (down) cursor = row == kKeyRows - 1 ? (column == 0 ? kKeyReset : kKeyOk) : cursor + 1;
    if ((left && column == 1) || (right && column == 0)) cursor = 1 + (1 - column) * kKeyRows + row;
    if (confirm) Keybinds::BeginCapture(action);
}

void DrawLan(const Canvas& c) {
    Window(c, 322);
    Title(c, "LAN", 0, 112, 40);
    Text(c, "Join a room on another device,", 0, 40);
    Text(c, "or create one here.", 0, 5);
    ButtonPair(c, "Join", "Create Room", 322, g_ui.cursor);
    Text(c, "(1) Back", 240, 130, 22, 2);  // back to Couch; tappable
    AddHit(195, 130, 140, 44, -1, true);
}

// A hint over one of the game's own screens, in the empty right half of its title bar.
void Banner(const Canvas& c, const std::string& text, uint32_t color = 0xFFFFFFFFu, float y = 192.0f) {
    const float x = 140.0f, halfWidth = 178.0f, halfHeight = 19.0f;
    c.list->AddRectFilled(
        ImVec2(c.view.centerX + (x - halfWidth) * c.view.scale, c.view.centerY - (y + halfHeight) * c.view.scale),
        ImVec2(c.view.centerX + (x + halfWidth) * c.view.scale, c.view.centerY - (y - halfHeight) * c.view.scale),
        IM_COL32(20, 40, 90, static_cast<int>(185 * c.alpha)), 10 * c.view.scale);
    Text(c, text, x, y, 24, 1, color);
}

void DrawChooseFileHint(const Canvas& c) {
    Banner(c, "Pick the save for your room.", 0xFFFFFFFFu, 148.0f);  // under the toggle
}

void DrawCountHint(const Canvas& c) {
    if (g_ui.countHintFrames > 0) {
        Banner(c, "A LAN room needs 2 to 4 players.", 0xFFE040FFu);
    } else {
        Banner(c, "How many can join your room?");
    }
}

void DrawRooms(const Canvas& c, const NetplayLobby::Snapshot& lobby) {
    const float height = 390;
    Window(c, height, 560);
    Title(c, "Rooms", 0, height / 2 - 40, 40);
    const int count = static_cast<int>(lobby.rooms.size());
    if (count == 0) {
        const int dots = (g_ui.frame / 20) % 4;
        Text(c, "Looking for rooms" + std::string(dots, '.'), 0, 30);
        Text(c, "Rooms on your Wi-Fi and Tailscale", 0, -20, 24);
        Text(c, "show up here.", 0, -48, 24);
    }
    const int visible = std::min(count, 4);
    const int first = std::clamp(g_ui.cursor - 3, 0, std::max(0, count - 4));
    for (int i = 0; i < visible; ++i) {
        const auto& room = lobby.rooms[first + i];
        const float y = height / 2 - 105 - i * 62;
        std::string label = room.name + "  " + std::to_string(room.players) + "/" + std::to_string(room.maxPlayers);
        if (room.tailscale) {
            label += "  (Tailscale)";
        }
        Button(c, label, 0, y, 480, g_ui.cursor == first + i, !room.full, g_ui.frame);
        AddHit(0, y, 490, 58, first + i);
    }
    Text(c, "(1) Back", 225, -height / 2 + 22, 22, 2);
    AddHit(175, -height / 2 + 22, 140, 44, -1, true);
}

void DrawRoom(const Canvas& c, const NetplayLobby::Snapshot& lobby) {
    const float height = 400;
    Window(c, height, 560);
    const bool host = lobby.localSlot == 0;
    Title(c, lobby.roomName.empty() ? "Room" : lobby.roomName, 0, height / 2 - 40, 36);
    for (int slot = 0; slot < std::max<int>(lobby.maxPlayers, 2); ++slot) {
        const auto member = std::find_if(lobby.members.begin(), lobby.members.end(),
                                         [&](const NetplayLobby::Member& m) { return m.slot == slot; });
        const float y = height / 2 - 100 - slot * 44;
        if (member != lobby.members.end()) {
            Text(c, PlayerLine(static_cast<uint8_t>(slot), member->name, slot == lobby.localSlot), -230, y, 28, 0);
        } else {
            Text(c, std::to_string(slot + 1) + "P  waiting...", -230, y, 28, 0, 0xC8D2E6FFu);
        }
    }
    std::string status;
    if (lobby.phase == Phase::Starting || lobby.phase == Phase::Restarting) {
        status = lobby.message.empty() ? "Starting..." : lobby.message;
    } else if (lobby.phase == Phase::Joining) {
        status = "Joining...";
    } else if (host) {
        status = lobby.members.size() < 2 ? "Others pick LAN > Join on their device." : "Start when everyone is here.";
    } else {
        status = "Waiting for the host to start...";
    }
    if (!lobby.message.empty() && lobby.phase == Phase::Hosting) {
        status = lobby.message;
    }
    Text(c, status, 0, -height / 2 + 100, 24, 1, 0xFFF0B0FFu);
    if (lobby.phase == Phase::Starting || lobby.phase == Phase::Restarting) {
        return;
    }
    if (host) {
        ButtonPair(c, "Start", "Cancel", height, g_ui.cursor, lobby.members.size() >= 2, true);
    } else {
        Button(c, "Leave", 0, -height / 2 + 43.0f, 232, true, true, g_ui.frame);
        AddHit(0, -height / 2 + 43.0f, 240, 60, 0);
    }
}

void DrawMessage(const Canvas& c) {
    Window(c, 300);
    // Up to two lines, split at a space near the middle.
    std::string first = g_ui.message, second;
    if (first.size() > 30) {
        const size_t mid = first.rfind(' ', first.size() / 2 + 6);
        if (mid != std::string::npos) {
            second = first.substr(mid + 1);
            first.resize(mid);
        }
    }
    Text(c, first, 0, second.empty() ? 30 : 50);
    if (!second.empty()) {
        Text(c, second, 0, 15);
    }
    Button(c, "OK", 0, -150 + 43.0f, 232, true, true, g_ui.frame);
    AddHit(0, -150 + 43.0f, 240, 60, 0);
}

std::string SlotName(int slot) {
    const auto& names = NetplayStart::SessionNames();
    return slot >= 0 && slot < static_cast<int>(names.size()) ? names[slot] : "player " + std::to_string(slot + 1);
}

// What the session shows: who the game is waiting for, a desync warning, and the way out when a
// player is gone (the game can't go on without them).
void DrawSession(ImDrawList* list) {
    const NetplaySession::Status status = NetplaySession::GetStatus();
    if (!status.active) {
        return;
    }
    const GameLayout::View view = GameLayout::GameView();
    Canvas c{list, view, 1.0f};
    if (status.lostSlot >= 0) {
        // The game is frozen: read the remote directly.
        static uint32_t prevHold = ~0u;
        WiiRemoteInput::KpadSample sample;
        uint32_t hold = 0;
        if (WiiRemoteInput::ReadKpadSample(0, sample)) {
            hold = sample.hold;
        }
        const bool confirm = prevHold != ~0u && (hold & ~prevHold & (kTwo | kA)) != 0;
        prevHold = hold;
        ++g_ui.frame;
        Veil(c);
        Window(c, 300);
        Text(c, "Lost the connection to", 0, 50);
        Text(c, SlotName(status.lostSlot) + ".", 0, 15);
        Button(c, "OK", 0, -150 + 43.0f, 232, true, true, g_ui.frame);
        if (confirm) {
            NetplayStart::EndSession();
        }
        return;
    }
    std::string line;
    if (status.waitingForSlot >= 0 && status.waitedMs > 700) {
        line = "Waiting for " + SlotName(status.waitingForSlot) + "...";
    } else if (status.desyncFrame != 0) {
        line = "The game got out of sync between devices.";
    }
    if (line.empty()) {
        return;
    }
    const float y = 190.0f;
    list->AddRectFilled(ImVec2(view.centerX - 290 * view.scale, view.centerY - (y + 20) * view.scale),
                        ImVec2(view.centerX + 290 * view.scale, view.centerY - (y - 20) * view.scale),
                        IM_COL32(20, 40, 90, 180), 10 * view.scale);
    Text(c, line, 0, y, 26);
}

// ---------------------------------------------------------------- logic
void HandleEvents(const NetplayLobby::Snapshot& lobby) {
    const uint32_t ev = g_ui.events;
    g_ui.events = 0;
    const bool left = ev & kEvLeft, right = ev & kEvRight, up = ev & kEvUp, down = ev & kEvDown;
    const bool confirm = ev & kEvConfirm, back = ev & kEvBack;
    switch (g_ui.screen) {
    case Screen::Lan:
        if (left || right) g_ui.cursor ^= 1;
        if (back) {
            // Back to Couch, with the cursor still on the toggle.
            Show(Screen::None);
            g_ui.toggleFocus = true;
            g_ui.topItem = 0;
        }
        if (confirm) {
            if (g_ui.cursor == 0) {
                NetplayLobby::Browse();
                Show(Screen::Rooms);
            } else {
                // The game's own file select picks the save the room plays, then its "Select
                // Players" the room's size.
                g_ui.countHintFrames = 0;
                Show(Screen::ChooseFile);
            }
        }
        break;
    case Screen::Count:
        if (g_ui.countPicked) {
            g_ui.countPicked = false;
            const int players = GameMenus::SelectPlayersCursor() + 1;
            if (players < 2) {
                g_ui.countHintFrames = 150;
            } else {
                NetplayLobby::Host(static_cast<uint8_t>(players), GameMenus::FileSelectCursor());
                Show(Screen::Room);
            }
        }
        break;
    case Screen::Rooms: {
        const int count = static_cast<int>(lobby.rooms.size());
        if (up) g_ui.cursor = std::max(0, g_ui.cursor - 1);
        if (down) g_ui.cursor = std::min(std::max(0, count - 1), g_ui.cursor + 1);
        if (back) {
            NetplayLobby::Stop();
            Show(Screen::Lan);
        }
        if (confirm && g_ui.cursor < count && !lobby.rooms[g_ui.cursor].full) {
            NetplayLobby::Join(lobby.rooms[g_ui.cursor].id);
            Show(Screen::Room);
        }
        break;
    }
    case Screen::Room:
        if (lobby.phase == Phase::Starting || lobby.phase == Phase::Restarting) {
            break;
        }
        if (lobby.localSlot == 0 && lobby.phase == Phase::Hosting) {
            if (left || right) g_ui.cursor ^= 1;
            if (back || (confirm && g_ui.cursor == 1)) {
                // Cancel: close the room and take the game back from "Select Players" to the file
                // select, in Couch.
                NetplayLobby::Stop();
                Show(Screen::None);
                g_ui.injectButtons = kOne;
                g_ui.injectReads = 4;
            } else if (confirm && g_ui.cursor == 0 && lobby.members.size() >= 2) {
                NetplayLobby::Start();
            }
        } else if (back || confirm) {
            NetplayLobby::Stop();
            NetplayLobby::Browse();
            Show(Screen::Rooms);
        }
        break;
    case Screen::Message:
        if (confirm || back) {
            NetplayLobby::Stop();
            Show(g_ui.afterMessage);
        }
        break;
    case Screen::Settings:
        HandleSettings(left, right, up, down, confirm, back);
        break;
    case Screen::None:
        break;
    }
}

// The Couch / LAN button pressed (by the remote or a tap).
void PressToggle() {
    if (g_ui.screen == Screen::None) {
        g_ui.toggleFocus = false;
        Show(Screen::Lan);  // Couch -> LAN: Join or Create Room
    } else {
        NetplayLobby::Stop();
        Show(Screen::None);  // LAN (picking the room's save) -> Couch
        g_ui.heldFromMenu = g_ui.prevHold;
    }
}

// A tap at (lx, ly) in layout units (y up, the game's 4:3 layout space centred on the screen). The
// game's own buttons are where its layouts (fileSelectBase, fileSelectPlayer) put them.
// `gx` is x in the game's own layout units, which differ from these menus' in Fill (View::gameScaleX).
void TapAt(float lx, float ly, float gx, uint16_t scene, bool title) {
    const auto in = [&](float cx, float cy, float w, float h) {
        return std::fabs(lx - cx) <= w / 2 && std::fabs(ly - cy) <= h / 2;
    };
    const auto inGame = [&](float cx, float cy, float w, float h) {
        return std::fabs(gx - cx) <= w / 2 && std::fabs(ly - cy) <= h / 2;
    };
    if (Modal()) {
        for (const Hit& hit : g_hits) {
            if (in(hit.x, hit.y, hit.w, hit.h)) {
                if (hit.cursor >= 0) {
                    g_ui.cursor = hit.cursor;
                }
                g_ui.events |= hit.event;
                return;
            }
        }
        return;
    }
    if (g_ui.toggleShown && in(kToggleX, kToggleY, kToggleW + 16, 66)) {
        PressToggle();
        return;
    }
    if (g_ui.pauseGearShown && in(PauseGearX(GameLayout::GameView()), kPauseGearY, kGearW + 20, 72)) {
        g_ui.settingsFromPause = true;
        Show(Screen::Settings);
        return;
    }
    if (g_ui.toggleShown && g_ui.screen == Screen::None && in(kGearX, kGearY, kGearW + 16, 66)) {
        g_ui.toggleFocus = false;
        Show(Screen::Settings);
        return;
    }
    // Anywhere else is the game's: the cursor leaves the toggle so the game takes the presses.
    g_ui.toggleFocus = false;
    if (g_nav.kind != NavKind::None) {
        return;  // still playing back the last tap
    }
    if (scene == kSceneBoot || title) {
        StartNav(NavKind::Press, static_cast<int>(kTwo | kA));  // the strap screen, "Press 2 to Start"
        return;
    }
    if (scene != kSceneGameSetup) {
        return;
    }
    if (GameMenus::FileSelectWaiting()) {
        for (int file = 0; file < 3; ++file) {
            if (inGame(-190.0f + 190.0f * file, 45, 184, 196)) {
                StartNav(NavKind::File, file);
                return;
            }
        }
        if (inGame(-143, -143, 259, 92)) {
            StartNav(NavKind::File, 3);  // Free-for-All
        } else if (inGame(143, -143, 259, 92)) {
            StartNav(NavKind::File, 4);  // Coin Battle
        } else if (inGame(-265, -62, 190, 46)) {
            StartNav(NavKind::Press, static_cast<int>(kMinus));  // Erase
        } else if (inGame(265, -62, 190, 46)) {
            StartNav(NavKind::Press, static_cast<int>(kPlus));  // Copy
        }
        return;
    }
    if (GameMenus::SelectPlayersState() == GameMenus::SelectPlayers::Choosing) {
        // Measured on screen: "1" spans the width above "2", "3" and "4".
        if (inGame(0, 40, 410, 122)) {
            StartNav(NavKind::Players, 0);
            return;
        }
        for (int count = 2; count <= 4; ++count) {
            if (inGame(-120.0f + 120.0f * (count - 2), -81, 112, 104)) {
                StartNav(NavKind::Players, count - 1);
                return;
            }
        }
        // "Back (1)" sits at the bottom right, nearer the edge on a wide screen.
        if (inGame(320, -183, 260, 70)) {
            StartNav(NavKind::Press, static_cast<int>(kOne));
        }
    }
}

} // namespace

void Tap(float x, float y) {
    const uint64_t fx = static_cast<uint64_t>(std::clamp(x, 0.0f, 1.0f) * 65535.0f);
    const uint64_t fy = static_cast<uint64_t>(std::clamp(y, 0.0f, 1.0f) * 65535.0f);
    g_tap.store((1ull << 63) | (fx << 16) | fy, std::memory_order_release);
}

bool TapScreenUp() {
    return g_tapScreen.load(std::memory_order_acquire);
}

bool TakeSettingsChange(float& renderScale, bool& showFps, int& aspect) {
    if (!g_settingsChanged.exchange(false)) {
        return false;
    }
    renderScale = settings_overlay::RenderScale();
    showFps = settings_overlay::ShowFps();
    aspect = settings_overlay::Aspect();
    return true;
}

void OnSessionStall() {
    // At most ~30 presents a second: enough for the banner, cheap while nothing moves.
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(33)) {
        return;
    }
    last = now;
    UpdateAuroraAndProcessEvents();
    if (g_auroraFrameActive.load(std::memory_order_acquire)) {
        settings_overlay::Draw();
        VI_HLE_PresentFrame(/*presentedXfb=*/false, /*paceToRetrace=*/false);
    }
}

// NSMBW_LOBBY_TEST (Android: debug.nsmbw.lobbytest) drives the lobby without the menus, to test
// rooms between devices: "host" opens a room for 2 and starts it once someone joins, "join" joins
// the first room it finds. The session then starts as it does from the menus.
bool LobbyTest() {
    static const std::string mode = [] {
        const char* value = std::getenv("NSMBW_LOBBY_TEST");
        return value != nullptr ? std::string(value) : std::string();
    }();
    if (mode != "host" && mode != "join") {
        return false;
    }
    static bool begun = false;
    if (!begun) {
        begun = true;
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: %s\n", mode.c_str());
        if (mode == "host") {
            NetplayLobby::Host(2, 0);
        } else {
            NetplayLobby::Browse();
        }
        return true;
    }
    const NetplayLobby::Snapshot lobby = NetplayLobby::Get();
    if (mode == "host" && lobby.phase == Phase::Hosting && lobby.members.size() >= 2) {
        NetplayLobby::Start();
    }
    if (mode == "join" && lobby.phase == Phase::Browsing && !lobby.rooms.empty()) {
        const auto& room = lobby.rooms.front();
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: joining '%s' at %s%s\n", room.name.c_str(), room.address.c_str(),
                room.tailscale ? " (Tailscale)" : "");
        NetplayLobby::Join(room.id);
    }
    static std::string lastMessage;
    if (lobby.phase == Phase::Failed && lobby.message != lastMessage) {
        lastMessage = lobby.message;
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: failed: %s\n", lobby.message.c_str());
    }
    if (lobby.phase == Phase::Restarting && !g_ui.restarting) {
        NetplayLobby::Plan plan;
        if (NetplayLobby::TakePlan(plan)) {
            g_ui.restarting = true;
            NetplayStart::Restart(plan);
        }
    }
    return true;
}

// NSMBW_DUMP_PROFILES=1: log the object profiles whenever the set changes (finding screens).
void DumpProfiles() {
    static const bool on = [] {
        const char* value = std::getenv("NSMBW_DUMP_PROFILES");
        return value != nullptr && value[0] == '1';
    }();
    static int frame = 0;
    static std::vector<uint16_t> last;
    if (!on || ++frame % 30 != 0) {
        return;
    }
    const auto profiles = GameMenus::Profiles();
    if (profiles == last) {
        return;
    }
    std::string text;
    for (const uint16_t p : profiles) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%03X ", p);
        text += hex;
    }
    RT_LOGF(RT_TAG_RUNTIME, "profiles (scene %03X, %zu): %s\n", GameMenus::CurrentScene(), profiles.size(), text.c_str());
    last = profiles;
}

// NSMBW_DUMP_LAYOUT=<arc>;<arc>...: log every pane's position in those layouts once (finding
// where the game's own buttons are).
void DumpLayouts() {
    static bool done = false;
    const char* value = std::getenv("NSMBW_DUMP_LAYOUT");
    if (done || value == nullptr) {
        return;
    }
    done = true;
    std::string all = value;
    for (size_t pos = 0; pos < all.size();) {
        const size_t end = std::min(all.find(';', pos), all.size());
        const std::string arc = all.substr(pos, end - pos);
        pos = end + 1;
        GameLayout::Layout layout;
        if (!layout.Load(arc)) {
            RT_LOGF(RT_TAG_RUNTIME, "layout dump: can't load %s\n", arc.c_str());
            continue;
        }
        for (const auto& name : layout.PaneNames()) {
            float x = 0, y = 0, w = 0, h = 0;
            if (layout.PaneWorldRect(name, x, y, w, h)) {
                RT_LOGF(RT_TAG_RUNTIME, "layout %s %s: x %.0f y %.0f w %.0f h %.0f\n", arc.c_str(), name.c_str(), x, y, w, h);
            }
        }
    }
}

// NSMBW_TAP_SCRIPT="<ms>:<x>:<y>,..." (Android: debug.nsmbw.tapscript): taps at fractions of the
// window, <ms> after the first frame, through the same path as real ones (testing without a hand).
void ScriptedTaps() {
    struct ScriptTap { long long ms; float x, y; };
    static std::vector<ScriptTap> taps = [] {
        std::vector<ScriptTap> list;
        const char* value = std::getenv("NSMBW_TAP_SCRIPT");
        if (value == nullptr) {
            return list;
        }
        long long ms = 0;
        float x = 0, y = 0;
        for (const char* p = value; *p != '\0';) {
            if (std::sscanf(p, "%lld:%f:%f", &ms, &x, &y) == 3) {
                list.push_back({ms, x, y});
            }
            const char* comma = std::strchr(p, ',');
            if (comma == nullptr) {
                break;
            }
            p = comma + 1;
        }
        return list;
    }();
    static const auto start = std::chrono::steady_clock::now();
    static size_t next = 0;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    while (next < taps.size() && taps[next].ms <= elapsed) {
        RT_LOGF(RT_TAG_RUNTIME, "tap script: %.2f, %.2f\n", taps[next].x, taps[next].y);
        Tap(taps[next].x, taps[next].y);
        ++next;
    }
}

void Draw() {
    DumpProfiles();
    if (std::getenv("NSMBW_DUMP_STATES") != nullptr) {
        static std::string lastStates;
        std::string states = GameMenus::DumpStates();
        if (states != lastStates) {
            RT_LOGF(RT_TAG_RUNTIME, "states: %s\n", states.c_str());
            lastStates = std::move(states);
        }
    }
    DumpLayouts();
    ScriptedTaps();
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    if (NetplaySession::Active()) {
        g_tapScreen.store(false, std::memory_order_release);
        DrawSession(list);
        return;
    }
    if (LobbyTest()) {
        return;
    }

    // The game's own setup screens decide when the toggle shows and the LAN steps go away.
    const uint16_t scene = GameMenus::CurrentScene();
    const bool fileWaiting = GameMenus::FileSelectWaiting();
    const GameMenus::SelectPlayers players = GameMenus::SelectPlayersState();
    if (scene != kSceneGameSetup) {
        // Back to the title, or into the game: the next file select starts in Couch.
        if (g_ui.screen == Screen::Lan || g_ui.screen == Screen::ChooseFile || g_ui.screen == Screen::Count) {
            Show(Screen::None);
        }
    }
    // The LAN / Couch toggle: on the file select while it waits for a pick (also under LAN's windows).
    g_ui.toggleShown = scene == kSceneGameSetup && fileWaiting && g_ui.screen != Screen::Count;
    if (!g_ui.toggleShown) {
        g_ui.toggleFocus = false;
    }
    if (g_ui.toggleFocus && !Modal()) {
        const uint32_t ev = g_ui.events;
        g_ui.events = 0;
        if (ev & (kEvDown | kEvBack)) {
            // Back down to the files; the press that left the toggle stays with us.
            g_ui.toggleFocus = false;
            g_ui.heldFromMenu = g_ui.prevHold;
        } else if (ev & kEvRight) {
            g_ui.topItem = 1;
        } else if (ev & kEvLeft) {
            g_ui.topItem = 0;
        } else if ((ev & kEvConfirm) && g_ui.topItem == 1) {
            g_ui.toggleFocus = false;
            Show(Screen::Settings);
        } else if (ev & kEvConfirm) {
            PressToggle();
        }
    }
    // Create Room: picking a file leads to the game's "Select Players", which sizes the room; Back
    // there returns to the file pick, and Free-for-All or Coin Battle leave room-making behind.
    if (g_ui.screen == Screen::ChooseFile && players == GameMenus::SelectPlayers::Choosing) {
        Show(Screen::Count);
    } else if (g_ui.screen == Screen::Count && players == GameMenus::SelectPlayers::Hidden && fileWaiting) {
        Show(Screen::ChooseFile);
    } else if ((g_ui.screen == Screen::ChooseFile || g_ui.screen == Screen::Count) &&
               players == GameMenus::SelectPlayers::Other) {
        Show(Screen::None);
    }
    // The pause menus' gear: looked for every few frames (walking the object tree is not free).
    static int pauseCheck = 0;
    if (++pauseCheck % 6 == 0) {
        g_ui.pauseGearShown = (g_ui.screen == Screen::None || g_ui.settingsFromPause) &&
                              (scene == kSceneWorldMap || scene == kSceneStage) && GameMenus::PauseMenuOpen();
    }
    // Touch (the Android app's Tap) and mouse clicks on the menus.
    static int titleCheck = 0;
    static bool title = false;
    if (++titleCheck % 10 == 0) {
        title = scene == kSceneStage && GameMenus::HasProfile(kProfileOpeningTitle);
    }
    g_tapScreen.store(Modal() || g_ui.toggleShown || scene == kSceneBoot || title ||
                          (scene == kSceneGameSetup && players == GameMenus::SelectPlayers::Choosing),
                      std::memory_order_release);
    {
        const ImGuiIO& io = ImGui::GetIO();
        float tapX = -1.0f, tapY = -1.0f;
        const uint64_t tap = g_tap.exchange(0, std::memory_order_acq_rel);
        if ((tap >> 63) != 0) {
            tapX = static_cast<float>((tap >> 16) & 0xFFFF) / 65535.0f * io.DisplaySize.x;
            tapY = static_cast<float>(tap & 0xFFFF) / 65535.0f * io.DisplaySize.y;
        } else if (io.MouseClicked[0] && !io.WantCaptureMouse) {
            tapX = io.MousePos.x;
            tapY = io.MousePos.y;
        }
        if (tapX >= 0.0f) {
            const GameLayout::View view = GameLayout::GameView();
            TapAt((tapX - view.centerX) / view.scale, (view.centerY - tapY) / view.scale,
                  (tapX - view.centerX) / view.gameScaleX, scene, title);
        }
    }

    if (g_ui.screen == Screen::None) {
        g_ui.events = 0;  // presses on the game's own screens are the game's
        if (g_ui.toggleShown) {
            ++g_ui.frame;
            DrawToggle(Canvas{list, GameLayout::GameView(), 1.0f});
        }
        if (g_ui.pauseGearShown) {
            ++g_ui.frame;
            DrawPauseGear(Canvas{list, GameLayout::GameView(), 1.0f});
        }
        return;
    }

    const NetplayLobby::Snapshot lobby = NetplayLobby::Get();
    if (lobby.phase == Phase::Failed && g_ui.screen != Screen::Message) {
        ShowMessage(lobby.message.empty() ? "Something went wrong." : lobby.message, Screen::Lan);
    }
    if (lobby.phase == Phase::Restarting && !g_ui.restarting) {
        NetplayLobby::Plan plan;
        if (NetplayLobby::TakePlan(plan)) {
            g_ui.restarting = true;
            if (!NetplayStart::Restart(plan)) {
                g_ui.restarting = false;
                NetplayLobby::Stop();
                ShowMessage("Couldn't start the session on this device.", Screen::Lan);
            }
        }
    }
    HandleEvents(lobby);
    if (g_ui.countHintFrames > 0) {
        --g_ui.countHintFrames;
    }

    GetAssets();
    ++g_ui.frame;
    Canvas c{list, GameLayout::GameView(), std::min(1.0f, g_ui.frame / 8.0f)};
    g_hits.clear();  // the screen below registers its buttons as it draws them
    if (g_ui.toggleShown) {
        DrawToggle(Canvas{list, c.view, 1.0f});  // under LAN's windows and their veil
    }
    switch (g_ui.screen) {
    case Screen::ChooseFile:
        DrawChooseFileHint(c);
        break;
    case Screen::Count:
        DrawCountHint(c);
        break;
    case Screen::Lan:
        Veil(c);
        DrawLan(c);
        break;
    case Screen::Rooms:
        Veil(c);
        DrawRooms(c, lobby);
        break;
    case Screen::Room:
        Veil(c);
        DrawRoom(c, lobby);
        break;
    case Screen::Message:
        Veil(c);
        DrawMessage(c);
        break;
    case Screen::Settings:
        Veil(c);
        DrawSettings(c);
        break;
    case Screen::None:
        break;
    }
}

void FilterGameInput(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (NetplaySession::Active()) {
        return;
    }
    if (chan == 0) {
        // A tap on the game's own screens plays back as presses of the first remote.
        if (!Modal()) {
            sample.hold |= NavButtons();
        }
        // Menu events from the first remote: screen directions for a sideways remote, the D-pad's
        // own for one held upright with a Nunchuk.
        const uint32_t hold = sample.hold;
        const uint32_t pressed = hold & ~g_ui.prevHold;
        g_ui.prevHold = hold;
        const bool upright = sample.hasNunchuk;
        if (pressed & (upright ? kLeft : kUp)) g_ui.events |= kEvLeft;
        if (pressed & (upright ? kRight : kDown)) g_ui.events |= kEvRight;
        if (pressed & (upright ? kUp : kRight)) g_ui.events |= kEvUp;
        if (pressed & (upright ? kDown : kLeft)) g_ui.events |= kEvDown;
        if (pressed & (kTwo | kA)) g_ui.events |= kEvConfirm;
        if (pressed & (kOne | kB)) g_ui.events |= kEvBack;
        if ((pressed & kPlus) && g_ui.screen == Screen::Settings) g_ui.events |= kEvBack;  // Esc / + closes it

        // Up from any of the three files (also in Create Room's file pick) moves onto the LAN /
        // Couch toggle above them; the game never sees that press, so its cursor stays put.
        const uint32_t screenUp = upright ? kUp : kRight;
        const int fileCursor = GameMenus::FileSelectRawCursor();
        if (g_ui.toggleShown && !g_ui.toggleFocus && !Modal() && (pressed & screenUp) != 0 && fileCursor >= 0 &&
            fileCursor <= 2) {
            g_ui.toggleFocus = true;
            g_ui.topItem = 0;
            g_ui.events &= ~kEvUp;
            g_ui.heldFromMenu |= screenUp;
        }
        // Create Room: the game's own screen picks the number; its confirm press is ours.
        if (g_ui.screen == Screen::Count && (pressed & (kTwo | kA)) != 0 &&
            GameMenus::SelectPlayersState() == GameMenus::SelectPlayers::Choosing) {
            g_ui.countPicked = true;
            g_ui.swallowConfirm = true;
        }
        if (g_ui.swallowConfirm) {
            if ((hold & (kTwo | kA)) == 0) {
                g_ui.swallowConfirm = false;
            }
            sample.hold &= ~(kTwo | kA);
        }
        g_ui.heldFromMenu &= hold;
        sample.hold &= ~g_ui.heldFromMenu;
        if (g_ui.injectReads > 0 && !Modal()) {
            --g_ui.injectReads;
            sample.hold |= g_ui.injectButtons;
        }
    }
    if (Modal() || g_ui.toggleFocus) {
        // The game sees the remote connected and at rest.
        sample.hold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clHold = 0;
    }
}

} // namespace NetplayUi
