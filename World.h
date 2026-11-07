// World.h - the tile map, the entities standing on it, and the code that lays out
// the whole adventure. Gameplay systems talk to the World; the Renderer only
// receives the sprites and lights the World asks it to draw.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Atlas.h"
#include "Common.h"
#include "Config.h"
#include "Entities.h"
#include "Renderer.h"

namespace bd {

// Flat surfaces. The order is also the order of the sprite tables in World.cpp.
enum Ground : uint8_t {
    G_GRASS,
    G_SAND,
    G_MOSS,
    G_DIRT,
    G_STONE,
    G_PLAZA,
    G_TILE,
    G_WOOD,
    G_CARPET,
    G_WATER,
    G_BRIDGE_H,
    G_BRIDGE_V,
    G_GLOWSTONE,
    G_COUNT
};

// Things that stand on a tile. Everything except O_NONE and O_REEDS blocks movement.
enum Obj : uint8_t {
    O_NONE,
    O_BLOCKER, // invisible solid tile (under chests, NPCs, gates, ...)
    O_TREE,
    O_TREE2,
    O_PINE,
    O_GTREE,
    O_GTREE2,
    O_BUSH,
    O_BUSH2,
    O_BUSH3,
    O_FBUSH,
    O_ROCK,
    O_ROCK2,
    O_STUMP,
    O_BIGMUSH,
    O_CRYSTAL0,
    O_CRYSTAL1,
    O_PILLAR,
    O_LAMP,
    O_REEDS,
    O_HEDGE,
    O_WALL,
    O_WALL_TORCH,
    O_WALL_BOOK,
    O_WALL_PAINT,
    O_WALL_WINDOW,
    O_SECRET_BOOK, // wall bookcase that hides a passage
    O_SECRET_BUSH, // bush that hides a passage
    O_COUNT
};

// Flat decoration drawn on top of the ground.
enum Decor : uint8_t {
    D_NONE,
    D_TUFT0,
    D_TUFT1,
    D_FLOWER_R,
    D_FLOWER_Y,
    D_FLOWER_B,
    D_FLOWER_P,
    D_FLOWER_W,
    D_PEBBLES,
    D_MUSH_R,
    D_MUSH_B,
    D_LEAF,
    D_LILY,
    D_COUNT
};

// A place on the map where a treasure chest can stand.
struct ChestSlot {
    AreaId area;
    int tx, ty;
    std::string secretId; // non-empty = hidden behind that secret passage
};

class World {
  public:
    static constexpr int W = 120; // map size in tiles
    static constexpr int H = 96;

    // Connect tile and object types to atlas sprites. Call once after Atlas::build().
    void init(const Atlas &a);
    // Build the map and place every entity, restoring progress from the state.
    void generate(const Config &cfg, GameState &st);
    void update(float dt);
    // Draw ground, decoration, objects and entities for the area around the camera.
    void draw(Renderer &r, const Atlas &a, vec2 cam, float zoom, float time, vec2 playerPos) const;
    // Gather every light source near the camera for the light map.
    void collectLights(const Atlas &a, vec2 cam, vec2 view, float time, vec2 playerPos,
                       std::vector<Light> &out) const;

    // ---- tiles
    bool inBounds(int tx, int ty) const {
        return tx >= 0 && ty >= 0 && tx < W && ty < H;
    }
    int idx(int tx, int ty) const {
        return ty * W + tx;
    }
    Ground ground(int tx, int ty) const {
        return inBounds(tx, ty) ? (Ground)ground_[idx(tx, ty)] : G_GRASS;
    }
    Obj obj(int tx, int ty) const {
        return inBounds(tx, ty) ? (Obj)obj_[idx(tx, ty)] : O_NONE;
    }
    AreaId areaAt(int tx, int ty) const {
        return inBounds(tx, ty) ? (AreaId)area_[idx(tx, ty)] : AreaId::Forest;
    }
    AreaId areaAtPos(vec2 p) const {
        return areaAt((int)std::floor(p.x / TILE), (int)std::floor(p.y / TILE));
    }
    void setGround(int tx, int ty, Ground g) {
        if (inBounds(tx, ty))
            ground_[idx(tx, ty)] = g;
    }
    void setObj(int tx, int ty, Obj o) {
        if (inBounds(tx, ty))
            obj_[idx(tx, ty)] = o;
    }

    // ---- collision and reachability
    bool solidTile(int tx, int ty) const;
    bool blocked(const Rect &box) const;
    bool walkableBody(int tx, int ty) const;
    // Mark every tile reachable on foot from a world position (1 = reachable).
    std::vector<uint8_t> floodFrom(vec2 p, bool ignored = false) const;

    // ---- entities
    Entity *add(std::unique_ptr<Entity> e);
    const std::vector<std::unique_ptr<Entity>> &entities() const {
        return ents_;
    }
    std::vector<std::unique_ptr<Entity>> &entities() {
        return ents_;
    }
    Chest *chestById(int id);
    Prop *propById(const std::string &id);
    void addCollectible(ItemKind k, const std::string &id, int tx, int ty);
    Player *player() const {
        return player_;
    }
    Companion *pip() const {
        return pip_;
    }

    // ---- puzzle changes
    void openGate(Prop &p, bool instant);
    void extendBridge();
    bool bridgeExtended() const {
        return bridge_;
    }

    vec2 start() const {
        return start_;
    }
    float time() const {
        return time_;
    }
    const std::vector<ChestSlot> &slots() const {
        return slots_;
    }

    // The order in which the four glow flowers must be touched (shuffled by seed).
    std::vector<int> flowerOrder;

  private:
    void build(const Config &cfg, GameState &st);
    void placeEntities(const Config &cfg, GameState &st);

    const Atlas *atlas_ = nullptr;

    // Per-tile layers, all W*H in row-major order.
    std::vector<uint8_t> ground_; // Ground
    std::vector<uint8_t> obj_;    // Obj
    std::vector<uint8_t> decor_;  // Decor
    std::vector<uint8_t> var_;    // random byte per tile for visual variety
    std::vector<uint8_t> area_;   // AreaId
    std::vector<uint8_t> keep_;   // 1 = scenery scatter must leave this tile alone

    // Sprite lookup tables filled by init().
    int objSpr_[O_COUNT] = {};
    int groundSpr_[G_COUNT][4] = {};
    int pathSpr_[16] = {};
    int carpetSpr_[16] = {};
    int waterSpr_[16][4] = {};
    int decorSpr_[D_COUNT] = {};

    std::vector<std::unique_ptr<Entity>> ents_;
    std::vector<ChestSlot> slots_;
    std::unordered_map<int, int> slotOf_; // chest id -> index into slots_
    ivec2 mazeShard_{0, 0};               // tile of the shard hidden in the hedge maze

    Player *player_ = nullptr;
    Companion *pip_ = nullptr;
    vec2 start_{0, 0};
    bool bridge_ = false;
    float time_ = 0;

    friend struct LevelBuilder; // the map-painting helper inside World.cpp
};

} // namespace bd
