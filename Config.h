// Config.h - what the person customises (birthday_config.json) and what the game
// remembers between sessions (the save file).
#pragma once
#include <set>
#include <string>
#include <vector>

#include "Common.h"

namespace bd {

// The five regions of the adventure, in story order. World code relies on this
// order when it moves a chest from a full area to the nearest one with space.
enum class AreaId : uint8_t { Forest = 0, Lake, Garden, Castle, Final };

inline const char *areaKey(AreaId a) {
    switch (a) {
    case AreaId::Forest:
        return "forest";
    case AreaId::Lake:
        return "lake";
    case AreaId::Garden:
        return "garden";
    case AreaId::Castle:
        return "castle";
    case AreaId::Final:
        return "final";
    }
    return "forest";
}
inline const char *areaTitle(AreaId a) {
    switch (a) {
    case AreaId::Forest:
        return "The Whispering Forest";
    case AreaId::Lake:
        return "The Mirror Lake";
    case AreaId::Garden:
        return "The Starlight Garden";
    case AreaId::Castle:
        return "The Memory Castle";
    case AreaId::Final:
        return "The Birthday Plateau";
    }
    return "";
}
// Parse "forest", "lake", ... (case-insensitive). Returns false if unknown.
bool parseArea(const std::string &s, AreaId &out);

// One treasure chest and the surprise inside it.
struct ChestDef {
    int id = 0;
    AreaId area = AreaId::Forest;
    bool final = false;     // the golden chest at the very end
    std::string title;      // heading shown when the chest opens
    std::string message;    // the personal message
    std::string photo;      // optional image path, relative to the game folder
};

// Everything a person can change without touching code.
struct Config {
    std::string name = "Friend";          // replaces {name} in dialogue
    std::string companion = "Pip";        // name of the firefly companion
    int age = 0;                          // 0 = do not mention an age
    std::string title = "Birthday Adventure";
    std::string finalMessage = "Happy Birthday!";
    bool guided = true;                   // false = every gate and door starts open
    std::vector<ChestDef> chests;

    // Load birthday_config.json from the game folder. A missing or broken file is
    // not fatal: defaults are used and a warning is logged. Returns true when a
    // file was read successfully.
    bool load(const std::string &path);
    // Sample content used when no config file exists.
    void fillDefaults();
};

// Progress through the adventure. Saved to disk and handed to World::generate so
// that opened gates, collected items and the player's position are restored.
struct GameState {
    uint32_t seed = 1;                 // drives the shuffled flower order
    vec2 playerPos{0, 0};              // (0,0) = start of the game
    int facing = 0;
    int flowerProgress = 0;            // flowers touched correctly so far
    std::set<std::string> shards;      // memory shards collected
    std::set<std::string> keys;        // keys currently held
    std::set<int> chestsOpened;
    std::set<std::string> flags;       // generic story/puzzle flags
    double playTime = 0;               // seconds

    bool flag(const std::string &f) const {
        return flags.count(f) > 0;
    }
    void setFlag(const std::string &f) {
        flags.insert(f);
    }
    void clearFlag(const std::string &f) {
        flags.erase(f);
    }

    // Plain-text save format (one "key value" per line), written atomically.
    bool save(const std::string &path) const;
    bool load(const std::string &path);
    void reset(uint32_t newSeed);
};

} // namespace bd
