#include "netplay/game_menus.h"

#include "memory.h"

namespace GameMenus {
namespace {

// fManager_c::m_connectManage (a cTreeMg_c): the root of every object's connect node. A node is
// {parent, child, prev, next, owner}; the owner is an fBase_c whose profile name is at +8.
constexpr uint32_t kTreeRoot = 0x80377A10u;
constexpr uint16_t kProfileSelectPlayer = 0x2BC;
constexpr uint16_t kProfileFileSelect = 0x2D5;

// Current state IDs (sStateMgr's current pointer) and the fields read from each object.
constexpr uint32_t kSelectPlayerState = 0x9C;
constexpr uint32_t kSelectPlayerCursor = 0x26C;
constexpr uint32_t kFileSelectState = 0x238;
constexpr uint32_t kFileSelectCursor = 0x2A4;

// dSelectPlayer_c::StateID_* (d_basesNP .bss, fixed by the pre-linked layout).
constexpr uint32_t kSpStartWait = 0x809953C0u;
constexpr uint32_t kSpOnStage = 0x80995400u;
constexpr uint32_t kSpInfoOnStage = 0x80995440u;
constexpr uint32_t kSpButtonChange = 0x80995480u;
constexpr uint32_t kSpSelect = 0x809954C0u;
constexpr uint32_t kSpMultiSelect = 0x80995500u;
constexpr uint32_t kSpButtonAnime = 0x80995540u;
constexpr uint32_t kSpExit = 0x80995580u;
// dFileSelect_c::StateID_SelectWait.
constexpr uint32_t kFsSelectWait = 0x80992050u;

uint32_t Find(uint16_t profile) {
    uint32_t node = 0;
    if (!Memory::TryRead32(kTreeRoot, node)) {
        return 0;
    }
    for (int guard = 0; node != 0 && guard < 4096; ++guard) {
        uint32_t owner = 0;
        uint32_t word = 0;
        if (Memory::TryRead32(node + 0x10, owner) && owner != 0 && Memory::TryRead32(owner + 8, word) &&
            static_cast<uint16_t>(word >> 16) == profile) {
            return owner;
        }
        uint32_t child = 0;
        Memory::TryRead32(node + 4, child);
        if (child != 0) {
            node = child;
            continue;
        }
        while (node != 0) {
            uint32_t next = 0;
            Memory::TryRead32(node + 0xC, next);
            if (next != 0) {
                node = next;
                break;
            }
            uint32_t parent = 0;
            Memory::TryRead32(node, parent);
            node = parent;
        }
    }
    return 0;
}

} // namespace

SelectPlayers SelectPlayersState() {
    const uint32_t object = Find(kProfileSelectPlayer);
    uint32_t state = 0;
    if (object == 0 || !Memory::TryRead32(object + kSelectPlayerState, state)) {
        return SelectPlayers::Hidden;
    }
    switch (state) {
    case kSpOnStage:
    case kSpInfoOnStage:
        return SelectPlayers::Appearing;
    case kSpSelect:
    case kSpButtonChange:
        return SelectPlayers::Choosing;
    case kSpButtonAnime:
    case kSpExit:
        return SelectPlayers::Leaving;
    case kSpMultiSelect:
        return SelectPlayers::Other;
    case kSpStartWait:
    default:
        return SelectPlayers::Hidden;
    }
}

bool SelectPlayersSettled() {
    const uint32_t object = Find(kProfileSelectPlayer);
    uint32_t state = 0;
    return object != 0 && Memory::TryRead32(object + kSelectPlayerState, state) && state == kSpSelect;
}

int SelectPlayersCursor() {
    const uint32_t object = Find(kProfileSelectPlayer);
    uint32_t cursor = 0;
    if (object == 0 || !Memory::TryRead32(object + kSelectPlayerCursor, cursor) || cursor > 3) {
        return 0;
    }
    return static_cast<int>(cursor);
}

bool FileSelectWaiting() {
    const uint32_t object = Find(kProfileFileSelect);
    uint32_t state = 0;
    return object != 0 && Memory::TryRead32(object + kFileSelectState, state) && state == kFsSelectWait;
}

int FileSelectCursor() {
    const uint32_t object = Find(kProfileFileSelect);
    uint32_t cursor = 0;
    if (object == 0 || !Memory::TryRead32(object + kFileSelectCursor, cursor) || cursor > 2) {
        return 0;
    }
    return static_cast<int>(cursor);
}

uint16_t CurrentScene() {
    uint32_t word = 0;
    return Memory::TryRead32(0x80428730u, word) ? static_cast<uint16_t>(word & 0xFFFFu) : 0;
}

} // namespace GameMenus
