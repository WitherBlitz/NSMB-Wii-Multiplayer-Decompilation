#pragma once

#include <cstdint>

// Where New Super Mario Bros. Wii's own setup menus are (the GAME_SETUP scene's file select and
// "Select Players" screens), read from its objects. The object tree and state IDs sit at fixed
// addresses in the game's code and its pre-linked RELs.
namespace GameMenus {

enum class SelectPlayers {
    Hidden,     // not on screen
    Appearing,  // sliding in
    Choosing,   // story mode: the player picks how many play (StartMemberSelect)
    Leaving,    // a number was picked, or Back
    Other,      // the Free-for-All / Coin Battle variant
};
SelectPlayers SelectPlayersState();
// True while "Select Players" waits for input (not animating the cursor).
bool SelectPlayersSettled();
// The highlighted button on "Select Players": 0 = 1 player .. 3 = 4 players.
int SelectPlayersCursor();

// The file select: true while it waits for a pick; the highlighted file (0-2).
bool FileSelectWaiting();
int FileSelectCursor();

// dScene_c::m_nowScene (0x003 world map, 0x005 stage/title, 0x00A game setup, ...).
uint16_t CurrentScene();

} // namespace GameMenus
