#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Network play, the lobby: rooms before a session starts. A host opens a room on UDP port 52130;
// other devices find it by broadcasting on every local network and by asking known Tailscale
// devices directly (the PC's `tailscale status`, hosts this device has played with before, and
// [netplay] hosts in Config.toml), then join it. Starting the room hands every member the host's
// save, waits until all of them have stored it, and tells everyone to restart into the session
// together (netplay_start.h).
namespace NetplayLobby {

enum class Phase {
    Off,
    Hosting,     // a room is open
    Browsing,    // looking for rooms
    Joining,     // asked a host to join
    InRoom,      // a member of someone's room
    Starting,    // host: handing out the save and waiting for everyone; member: receiving it
    Restarting,  // everyone is ready: restart now
    Failed,
};

struct Member {
    uint8_t slot = 0;
    std::string name;
};

struct Room {
    uint64_t id = 0;
    std::string name;
    std::string host;
    uint8_t maxPlayers = 0;
    uint8_t players = 0;
    std::string address;  // ip:port
    bool tailscale = false;
    bool full = false;
};

struct Snapshot {
    Phase phase = Phase::Off;
    std::string message;  // what went wrong, or what is happening
    std::string roomName;
    uint8_t maxPlayers = 0;
    uint8_t localSlot = 0;
    std::vector<Member> members;  // slot order
    std::vector<Room> rooms;      // while browsing, newest first
};
Snapshot Get();

// Opens a room for `maxPlayers` (2-4). `saveFile` is the save slot (0-2) the host picked.
void Host(uint8_t maxPlayers, int saveFile);
void Browse();
void Join(uint64_t roomId);
// Host: start the session with everyone in the room.
void Start();
// Leave the room, close it, or stop looking.
void Stop();

// What a restart into the session needs; filled once the phase reaches Restarting.
struct Plan {
    uint64_t sessionId = 0;
    uint8_t localSlot = 0;
    uint8_t players = 0;
    uint32_t inputDelay = 3;
    std::string host;  // clients: the host's ip:port as this device reached it
    int saveFile = 0;
    std::vector<std::string> names;
    std::map<std::string, std::vector<uint8_t>> nandFiles;  // NAND path -> bytes
};
bool TakePlan(Plan& out);

} // namespace NetplayLobby
