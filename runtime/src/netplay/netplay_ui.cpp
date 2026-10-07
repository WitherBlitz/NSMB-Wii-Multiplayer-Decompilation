#include "netplay_ui.h"

#include "netplay/game_layout.h"
#include "netplay/game_menus.h"
#include "netplay_lobby.h"
#include "netplay_session.h"
#include "netplay_start.h"
#include "runtime_log.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace NetplayUi {
namespace {

using GameLayout::Utf16;
using Phase = NetplayLobby::Phase;

// WPAD_BUTTON_* bits.
constexpr uint32_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kTwo = 0x0100, kOne = 0x0200,
                   kB = 0x0400, kA = 0x0800;

enum class Screen { None, Mode, Lan, Count, Rooms, Room, Message };

enum Event : uint32_t {
    kEvLeft = 1, kEvRight = 2, kEvUp = 4, kEvDown = 8, kEvConfirm = 16, kEvBack = 32,
};

struct Ui {
    Screen screen = Screen::None;
    int cursor = 0;
    bool armed = true;           // show the mode menu the next time "Select Players" comes up
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
};

Ui g_ui;

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

bool IsModal(Screen screen) {
    return screen != Screen::None && screen != Screen::Count;
}

const char* ScreenName(Screen screen) {
    switch (screen) {
    case Screen::None: return "none";
    case Screen::Mode: return "LAN or Couch";
    case Screen::Lan: return "Join or Create Room";
    case Screen::Count: return "room size (game screen)";
    case Screen::Rooms: return "rooms";
    case Screen::Room: return "room";
    case Screen::Message: return "message";
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
            int frame) {
    Assets& a = GetAssets();
    GameLayout::DrawParams p;
    p.alpha = c.alpha * (enabled ? 1.0f : 0.55f);
    p.offsetX = x;
    p.offsetY = y + 118.0f;  // the pane is authored at (0, -118)
    p.scale = selected ? 1.0f + 0.04f * Pulse(frame) : 0.94f;
    p.paneScale["P_centerBase_00"] = 1.0f;  // authored at 0: it grows in through an animation
    p.paneSize["P_centerBase_00"] = {w, 53.0f};
    p.paneSize["W_button_02"] = {w + 5.0f, 63.0f};
    p.paneSize["P_shadow_02"] = {w, 53.0f};
    p.visible["T_center_00"] = false;
    p.visible["T_center_01"] = false;
    if (!enabled) {
        p.vertexTint["P_centerBase_00"] = 0xB0B0B0FFu;
    }
    a.yesNo.Draw(c.list, c.view, "P_centerBase_00", p);
    const float scale = p.scale;
    Canvas t = c;
    t.alpha = p.alpha;
    Text(t, label, x, y, 30.0f * scale);
    if (selected) {
        Cursor(c, x, y, w * scale + 6, 53 * scale + 6, frame);
    }
}

// Two side-by-side buttons at the bottom of a window `height` tall.
void ButtonPair(const Canvas& c, const std::string& left, const std::string& right, float height, int cursor,
                bool leftEnabled = true, bool rightEnabled = true) {
    const float y = -height / 2 + 43.0f;
    Button(c, left, -126, y, 232, cursor == 0, leftEnabled, g_ui.frame);
    Button(c, right, 126, y, 232, cursor == 1, rightEnabled, g_ui.frame);
}

std::string PlayerLine(uint8_t slot, const std::string& name, bool you) {
    return std::to_string(slot + 1) + "P  " + name + (you ? "  (you)" : "");
}

// ---------------------------------------------------------------- screens
void DrawMode(const Canvas& c) {
    Window(c, 322);
    Title(c, "LAN or Couch?", 0, 112, 40);
    Text(c, "Play on this device, or with other", 0, 40);
    Text(c, "devices on your network?", 0, 5);
    ButtonPair(c, "LAN", "Couch", 322, g_ui.cursor);
}

void DrawLan(const Canvas& c) {
    Window(c, 322);
    Title(c, "LAN", 0, 112, 40);
    Text(c, "Join a room on another device,", 0, 40);
    Text(c, "or create one here.", 0, 5);
    ButtonPair(c, "Join", "Create Room", 322, g_ui.cursor);
}

void DrawCountHint(const Canvas& c) {
    // A banner over the game's own "Select Players" screen.
    const float y = -142.0f;  // between the number buttons and the game's Back button
    c.list->AddRectFilled(
        ImVec2(c.view.centerX - 270 * c.view.scale, c.view.centerY - (y + 19) * c.view.scale),
        ImVec2(c.view.centerX + 270 * c.view.scale, c.view.centerY - (y - 19) * c.view.scale),
        IM_COL32(20, 40, 90, static_cast<int>(170 * c.alpha)), 10 * c.view.scale);
    Text(c, g_ui.countHintFrames > 0 ? "A LAN room needs 2 to 4 players." : "How many players can join your room?",
         0, y, 28, 1, g_ui.countHintFrames > 0 ? 0xFFE040FFu : 0xFFFFFFFFu);
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
    }
    Text(c, "(1) Back", 225, -height / 2 + 22, 22, 2);
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
}

// What the session shows: who the game is waiting for, and a desync warning.
void DrawSession(ImDrawList* list) {
    const NetplaySession::Status status = NetplaySession::GetStatus();
    if (!status.active) {
        return;
    }
    const GameLayout::View view = GameLayout::GameView();
    Canvas c{list, view, 1.0f};
    std::string line;
    if (status.waitingForSlot >= 0 && status.waitedMs > 700) {
        const auto& names = NetplayStart::SessionNames();
        const std::string who = status.waitingForSlot < static_cast<int>(names.size())
                                    ? names[status.waitingForSlot]
                                    : "player " + std::to_string(status.waitingForSlot + 1);
        line = "Waiting for " + who + "...";
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
    case Screen::Mode:
        if (left || right) g_ui.cursor ^= 1;
        if (confirm) {
            if (g_ui.cursor == 0) {
                Show(Screen::Lan);
            } else {
                Show(Screen::None);  // Couch: the game's own screen
            }
        }
        break;
    case Screen::Lan:
        if (left || right) g_ui.cursor ^= 1;
        if (back) Show(Screen::Mode);
        if (confirm) {
            if (g_ui.cursor == 0) {
                NetplayLobby::Browse();
                Show(Screen::Rooms);
            } else {
                g_ui.countHintFrames = 0;
                Show(Screen::Count);
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
                NetplayLobby::Stop();
                Show(Screen::Mode);
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
    case Screen::None:
        break;
    }
}

} // namespace

void Draw() {
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    if (NetplaySession::Active()) {
        DrawSession(list);
        return;
    }

    // The game's "Select Players" screen decides when the menus come up and go away.
    const GameMenus::SelectPlayers players = GameMenus::SelectPlayersState();
    static GameMenus::SelectPlayers lastPlayers = GameMenus::SelectPlayers::Hidden;
    if (players != lastPlayers) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay ui: Select Players state %d -> %d\n", static_cast<int>(lastPlayers),
                static_cast<int>(players));
        lastPlayers = players;
    }
    if (players == GameMenus::SelectPlayers::Hidden || players == GameMenus::SelectPlayers::Other) {
        if (g_ui.screen == Screen::Count || g_ui.screen == Screen::Mode || g_ui.screen == Screen::Lan) {
            Show(Screen::None);  // backed out to the file select
        }
        if (g_ui.screen == Screen::None) {
            g_ui.armed = true;
        }
    } else if (players == GameMenus::SelectPlayers::Choosing && g_ui.armed && g_ui.screen == Screen::None) {
        g_ui.armed = false;
        Show(Screen::Mode);
    }
    if (g_ui.screen == Screen::None) {
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
    switch (g_ui.screen) {
    case Screen::Count:
        DrawCountHint(c);
        break;
    case Screen::Mode:
        Veil(c);
        DrawMode(c);
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
    case Screen::None:
        break;
    }
}

void FilterGameInput(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (NetplaySession::Active()) {
        return;
    }
    if (chan == 0) {
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

        // Back on the first menu goes back in the game too: let the press through and close.
        if (g_ui.screen == Screen::Mode && (pressed & kOne) != 0) {
            Show(Screen::None);
            return;
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
    }
    if (Modal()) {
        // The game sees the remote connected and at rest.
        sample.hold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clHold = 0;
    }
}

} // namespace NetplayUi
