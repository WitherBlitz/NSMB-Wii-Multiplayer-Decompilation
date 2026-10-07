#pragma once

#include "wii_remote_input.h"

#include <cstdint>

// The network-play menus, drawn with the game's own window and font graphics (netplay/game_layout.h):
//   - "LAN" or "Couch" when the game is about to ask how many people are playing;
//   - for LAN: "Join" or "Create Room"; Create Room goes on to the game's own player-count screen
//     and then the room, where the host starts or cancels it; Join lists the rooms found on the LAN
//     and over Tailscale;
//   - starting a room restarts every device into the network session.
namespace NetplayUi {

// Once per presented frame, inside the ImGui frame (settings_overlay::Draw).
void Draw();

// Every Wii Remote sample the game reads in normal play passes through here: while a menu is up
// the game sees the remote at rest (and the menu takes the buttons), and in a room's player-count
// step the confirm press is taken from the game.
void FilterGameInput(uint32_t chan, WiiRemoteInput::KpadSample& sample);

} // namespace NetplayUi
