// Entities.h - everything that lives in the world and is not a tile: the player,
// the companion, NPCs, chests, interactive props and collectibles.
// Entities draw themselves and contribute lights; the World owns them and sorts
// them by depth so they overlap correctly.
#pragma once
#include <string>
#include <vector>

#include "Atlas.h"
#include "Common.h"
#include "Renderer.h"

namespace bd {

class World;

// A point light collected each frame and rendered into the light map.
struct Light {
    vec2 pos;    // world position of the centre
    float radius;
    vec4 color;  // rgb = colour, a = strength
};

enum class EType { Player, Companion, NPC, Chest, Prop, Collectible };

enum class PropKind {
    Sign,
    Lever,
    Gate,
    Door,
    Door2,
    Pedestal,
    GlowFlower,
    Secret,
    Throne,
    Balloon,
    Deco
};

enum class ItemKind { Shard, KeyForest, KeyGarden, KeyGolden };

// ================================================================ Entity
// Common base class. `pos` is the position of the entity's feet in world pixels.
struct Entity {
    EType type = EType::Prop;
    vec2 pos{0, 0};
    bool removed = false; // set to true to have the World delete it after update()

    virtual ~Entity() = default;
    virtual void update(float dt, World &w) {
        (void)dt;
        (void)w;
    }
    virtual void draw(Renderer &r, const Atlas &a, float time) const = 0;
    virtual void addLights(std::vector<Light> &out, float time) const {
        (void)out;
        (void)time;
    }
    // Value used for depth sorting. Larger = closer to the viewer = drawn later.
    virtual float sortY() const {
        return pos.y;
    }
};

// ================================================================ Player
struct Player : Entity {
    vec2 vel{0, 0};
    int facing = 0; // 0 down, 1 up, 2 left, 3 right
    bool moving = false;
    bool sprinting = false;
    float walkT = 0;    // advances while walking; its integer part picks the frame
    float idleT = 0;    // seconds spent standing still (drives the idle bob)
    int lastFrame = 0;
    int stepEvent = 0;  // set to 1 when a footstep should play; the game clears it

    // Throne sitting. The game drives these while the player is seated.
    float sitK = 0;     // 0 standing .. 1 fully seated (also used while hopping on/off)
    float hop = 0;      // vertical lift in world pixels (arc while getting on/off)
    float sitT = 0;     // seconds spent seated (drives the crown pop and sparkles)

    Player() {
        type = EType::Player;
    }

    // Walk-cycle frame 0..3 (0 when standing still).
    int frame() const;
    // Move with smoothing and per-axis wall sliding.
    void move(float dt, vec2 dir, bool sprint, const World &w);

    // Collision box around the feet, in world pixels.
    static Rect box(vec2 p) {
        return Rect{p.x - 5.f, p.y - 5.f, 10.f, 5.f};
    }

    void draw(Renderer &r, const Atlas &a, float time) const override;
    void addLights(std::vector<Light> &out, float time) const override;

  private:
    void drawSeated(Renderer &r, const Atlas &a, float time) const;
};

// ================================================================ Companion
// A small glowing firefly that floats near the player.
struct Companion : Entity {
    float t = 0;
    float speechT = 0; // > 0 while the companion is "speaking" (the UI can use this)

    Companion() {
        type = EType::Companion;
    }

    void follow(float dt, const Player &p);
    void draw(Renderer &r, const Atlas &a, float time) const override;
    void addLights(std::vector<Light> &out, float time) const override;
};

// ================================================================ NPC
struct NPC : Entity {
    std::string id;
    std::string name;
    std::string sprite; // atlas character set: "oak", "frog", "moth", "ghost", ...
    int facing = 0;
    std::vector<std::string> lines;       // first conversation
    std::vector<std::string> repeatLines; // later conversations
    float phase = 0;                      // desynchronises idle animations
    bool talked = false;                  // set by the game once the first chat is done

    NPC() {
        type = EType::NPC;
    }

    void draw(Renderer &r, const Atlas &a, float time) const override;
    void addLights(std::vector<Light> &out, float time) const override;
};

// ================================================================ Chest
struct Chest : Entity {
    int id = 0;
    bool isFinal = false; // the golden chest at the end
    bool sealed = false;  // shown with a purple glow until the player is allowed to open it
    bool opening = false;
    bool opened = false;
    float animT = 0;      // 0..1 progress of the opening animation

    Chest() {
        type = EType::Chest;
    }

    // Start the opening animation.
    void open() {
        if (!opened && !opening) {
            opening = true;
            animT = 0;
        }
    }
    // Skip the animation (used when restoring a saved game).
    void setOpenInstant() {
        opening = false;
        opened = true;
        animT = 1.f;
    }

    void update(float dt, World &w) override;
    void draw(Renderer &r, const Atlas &a, float time) const override;
    void addLights(std::vector<Light> &out, float time) const override;
};

// ================================================================ Prop
// Interactive and decorative objects: signs, levers, gates, doors, puzzle flowers,
// hidden passages, balloons and large scenery pieces.
struct Prop : Entity {
    PropKind kind = PropKind::Sign;
    std::string id;
    std::string text;    // sign text
    std::string sprite;  // atlas sprite name for PropKind::Deco
    std::string needKey; // key required to open (gates and doors); empty = none

    int tx = 0, ty = 0; // tile origin (gates, doors, secrets)
    int tw = 1, th = 1; // size in tiles
    int index = 0;      // glow-flower number / balloon colour / decoration variant
    bool open = false;
    bool lit = false;   // glow flower is currently glowing
    bool canInteract = true;
    float phase = 0;

    // Optional light: 0 none, 1 warm, 2 violet, 3 cool blue.
    int lightKind = 0;
    float lightRadius = 0;

    // Birthday cake: > 0 while the candles are blown out (counts up, then relights).
    float blowT = 0;
    bool relit = false; // set for one frame when the candles relight; the game clears it

    Prop() {
        type = EType::Prop;
    }

    // Short verb shown to the player ("Read", "Pull", "Unlock", ...). Empty = hide.
    std::string prompt() const;
    float interactRadius() const;

    void update(float dt, World &w) override;

    float sortY() const override {
        // Tile-sized structures are drawn from their top-left tile, so sort them by
        // their bottom edge rather than their centre.
        if (kind == PropKind::Gate || kind == PropKind::Door || kind == PropKind::Door2)
            return (float)((ty + th) * TILE) - 0.5f;
        return pos.y;
    }

    void draw(Renderer &r, const Atlas &a, float time) const override;
    void addLights(std::vector<Light> &out, float time) const override;
};

// ================================================================ Collectible
struct Collectible : Entity {
    ItemKind kind = ItemKind::Shard;
    std::string id;
    float phase = 0;

    Collectible() {
        type = EType::Collectible;
    }

    void draw(Renderer &r, const Atlas &a, float time) const override;
    void addLights(std::vector<Light> &out, float time) const override;
};

} // namespace bd
