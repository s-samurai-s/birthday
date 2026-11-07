#include "World.h"

#include <queue>

namespace bd {

namespace {
bool isWallish(Obj o) {
    return o == O_WALL || o == O_WALL_TORCH || o == O_WALL_BOOK || o == O_WALL_PAINT ||
           o == O_WALL_WINDOW || o == O_SECRET_BOOK;
}
float hf(int x, int y, uint32_t seed = 0) {
    return (hash2(x, y, seed) & 0xFFFF) / 65535.f;
}
} // namespace

// ============================================================ init
// Connect the logical tile/object types to the matching sprites in the atlas. The
// world stores compact IDs while the renderer only needs the sprite references.
void World::init(const Atlas &a) {
    atlas_ = &a;
    auto id = [&](const std::string &n) { return a.id(n); };
    for (int i = 0; i < O_COUNT; i++)
        objSpr_[i] = 0;
    objSpr_[O_TREE] = id("tree0");
    objSpr_[O_TREE2] = id("tree1");
    objSpr_[O_PINE] = id("pine0");
    objSpr_[O_GTREE] = id("gtree0");
    objSpr_[O_GTREE2] = id("gtree1");
    objSpr_[O_BUSH] = id("bush0");
    objSpr_[O_BUSH2] = id("bush1");
    objSpr_[O_BUSH3] = id("bush2");
    objSpr_[O_FBUSH] = id("fbush");
    objSpr_[O_ROCK] = id("rock0");
    objSpr_[O_ROCK2] = id("rock1");
    objSpr_[O_STUMP] = id("stump");
    objSpr_[O_BIGMUSH] = id("bigmush");
    objSpr_[O_CRYSTAL0] = id("crystal0");
    objSpr_[O_CRYSTAL1] = id("crystal1");
    objSpr_[O_PILLAR] = id("pillar");
    objSpr_[O_LAMP] = id("lamp");
    objSpr_[O_REEDS] = id("reeds0");
    objSpr_[O_WALL_TORCH] = id("wall_torch");
    objSpr_[O_WALL_BOOK] = id("wall_book");
    objSpr_[O_WALL_PAINT] = id("wall_paint0");
    objSpr_[O_WALL_WINDOW] = id("wall_window");
    objSpr_[O_HEDGE] = id("hedge");
    objSpr_[O_SECRET_BOOK] = id("wall_book");
    objSpr_[O_SECRET_BUSH] = id("bush2");
    objSpr_[O_WALL] = id("wall_face");
    const char *gn[G_COUNT] = {"grass",    "sand",     "moss",     "path_0",   "stone",
                               "plaza",    "tile_a",   "wood",     "carpet_0", "water_15_0",
                               "bridge_h", "bridge_v", "glowstone"};
    const int gv[G_COUNT] = {4, 2, 3, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1};
    for (int g = 0; g < G_COUNT; g++)
        for (int v = 0; v < 4; v++) {
            std::string n = gn[g];
            if (g == G_GRASS || g == G_SAND || g == G_MOSS || g == G_STONE || g == G_PLAZA ||
                g == G_WOOD)
                n += std::to_string(v % gv[g]);
            groundSpr_[g][v] = id(n);
        }
    for (int m = 0; m < 16; m++) {
        pathSpr_[m] = id("path_" + std::to_string(m));
        carpetSpr_[m] = id("carpet_" + std::to_string(m));
        for (int f = 0; f < 4; f++)
            waterSpr_[m][f] = id("water_" + std::to_string(m) + "_" + std::to_string(f));
    }
    const char *dn[D_COUNT] = {"",         "tuft0",    "tuft1",    "flower_r", "flower_y",
                               "flower_b", "flower_p", "flower_w", "pebbles",  "mush_r",
                               "mush_b",   "leaf",     "lily"};
    for (int d = 0; d < D_COUNT; d++)
        decorSpr_[d] = d ? id(dn[d]) : 0;
}

// ============================================================ helpers
// Small world helpers live here so collision, path checks and entity placement all
// use the same rules instead of each system making its own assumptions.
Entity *World::add(std::unique_ptr<Entity> e) {
    ents_.push_back(std::move(e));
    return ents_.back().get();
}
Chest *World::chestById(int id) {
    for (auto &e : ents_)
        if (e->type == EType::Chest) {
            auto *c = static_cast<Chest *>(e.get());
            if (c->id == id)
                return c;
        }
    return nullptr;
}
Prop *World::propById(const std::string &id) {
    for (auto &e : ents_)
        if (e->type == EType::Prop) {
            auto *p = static_cast<Prop *>(e.get());
            if (p->id == id)
                return p;
        }
    return nullptr;
}
void World::addCollectible(ItemKind k, const std::string &id, int tx, int ty) {
    auto c = std::make_unique<Collectible>();
    c->kind = k;
    c->id = id;
    c->pos = vec2((tx + 0.5f) * TILE, (ty + 1.f) * TILE);
    c->phase = hf(tx, ty) * 6.f;
    add(std::move(c));
}

bool World::solidTile(int tx, int ty) const {
    if (!inBounds(tx, ty))
        return true;
    if (ground_[idx(tx, ty)] == G_WATER)
        return true;
    Obj o = (Obj)obj_[idx(tx, ty)];
    return !(o == O_NONE || o == O_REEDS);
}
// Check every tile touched by the rectangle, not just the tile under its centre.
// The tiny subtraction on the right and bottom edges keeps an object that ends
// exactly on a tile boundary from accidentally colliding with the next tile.
bool World::blocked(const Rect &b) const {
    int x0 = (int)std::floor(b.x / TILE), x1 = (int)std::floor((b.x + b.w - 0.001f) / TILE);
    int y0 = (int)std::floor(b.y / TILE), y1 = (int)std::floor((b.y + b.h - 0.001f) / TILE);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (solidTile(x, y))
                return true;
    return false;
}
bool World::walkableBody(int tx, int ty) const {
    return !solidTile(tx, ty);
}

// Flood-fill the walkable tiles connected to a starting position. This is useful
// when the level builder needs to know which parts of the map can actually be reached.
std::vector<uint8_t> World::floodFrom(vec2 p, bool) const {
    std::vector<uint8_t> seen((size_t)W * H, 0);
    int sx = (int)std::floor(p.x / TILE), sy = (int)std::floor(p.y / TILE);
    if (!inBounds(sx, sy) || solidTile(sx, sy))
        return seen;
    std::queue<ivec2> q;
    q.push({sx, sy});
    seen[idx(sx, sy)] = 1;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while (!q.empty()) {
        ivec2 c = q.front();
        q.pop();
        for (int i = 0; i < 4; i++) {
            int nx = c.x + dx[i], ny = c.y + dy[i];
            if (!inBounds(nx, ny) || seen[idx(nx, ny)] || solidTile(nx, ny))
                continue;
            seen[idx(nx, ny)] = 1;
            q.push({nx, ny});
        }
    }
    return seen;
}

void World::openGate(Prop &p, bool instant) {
    p.open = true;
    p.canInteract = false;
    for (int y = p.ty; y < p.ty + p.th; y++)
        for (int x = p.tx; x < p.tx + p.tw; x++) {
            Obj o = obj(x, y);
            if (o == O_BLOCKER || o == O_SECRET_BUSH || o == O_SECRET_BOOK)
                setObj(x, y, O_NONE);
        }
    (void)instant;
}
void World::extendBridge() {
    bridge_ = true;
    for (int y = 56; y <= 58; y++)
        for (int x = 59; x <= 60; x++)
            setGround(x, y, G_BRIDGE_V);
}

// ============================================================ level builder
// LevelBuilder is a small set of map-painting tools. Instead of writing hundreds of
// individual tile assignments, the level can be described with paths, clearings,
// walls and decorated areas that are easier to read and adjust.
struct LevelBuilder {
    World &w;
    explicit LevelBuilder(World &world) : w(world) {}

    void keep(int x0, int y0, int x1, int y1) {
        for (int y = std::max(0, y0); y <= std::min(World::H - 1, y1); y++)
            for (int x = std::max(0, x0); x <= std::min(World::W - 1, x1); x++)
                w.keep_[w.idx(x, y)] = 1;
    }
    void fillG(int x0, int y0, int x1, int y1, Ground g) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                w.setGround(x, y, g);
    }
    void fillO(int x0, int y0, int x1, int y1, Obj o) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                w.setObj(x, y, o);
    }
    // axis aligned 2-wide paths
    void hline(int x0, int x1, int y, Ground g = G_DIRT, int th = 2) {
        for (int x = std::min(x0, x1); x <= std::max(x0, x1); x++)
            for (int k = 0; k < th; k++) {
                w.setGround(x, y + k, g);
                w.setObj(x, y + k, O_NONE);
                w.decor_[w.idx(x, y + k)] = 0;
            }
        keep(std::min(x0, x1) - 1, y - 1, std::max(x0, x1) + 1, y + th);
    }
    void vline(int y0, int y1, int x, Ground g = G_DIRT, int th = 2) {
        for (int y = std::min(y0, y1); y <= std::max(y0, y1); y++)
            for (int k = 0; k < th; k++) {
                w.setGround(x + k, y, g);
                w.setObj(x + k, y, O_NONE);
                w.decor_[w.idx(x + k, y)] = 0;
            }
        keep(x - 1, std::min(y0, y1) - 1, x + th, std::max(y0, y1) + 1);
    }
    template <class F> void disc(int cx, int cy, float r, F fn) {
        for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
            for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
                float dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= r * r)
                    fn(x, y);
            }
    }
    void clearing(int cx, int cy, float r, Ground g) {
        disc(cx, cy, r, [&](int x, int y) {
            if (w.inBounds(x, y)) {
                w.setGround(x, y, g);
                w.setObj(x, y, O_NONE);
            }
        });
        keep(cx - (int)r - 1, cy - (int)r - 1, cx + (int)r + 1, cy + (int)r + 1);
    }
    void secretBlock(int tx, int ty, int tw, int th, Obj o, const std::string &id) {
        fillO(tx, ty, tx + tw - 1, ty + th - 1, o);
        auto p = std::make_unique<Prop>();
        p->kind = PropKind::Secret;
        p->id = id;
        p->tx = tx;
        p->ty = ty;
        p->tw = tw;
        p->th = th;
        p->pos = vec2((tx + tw * 0.5f) * TILE, (ty + th * 0.5f) * TILE + 6);
        p->phase = hf(tx, ty) * 3.f;
        w.add(std::move(p));
    }
    // Ring a rectangle with a two-tile-thick wall of trees so the only way in is the
    // secret bush the caller places on the wall afterwards. Water and bridges are left
    // alone (water already blocks the player).
    template <class F> void sanctum(int x0, int y0, int x1, int y1, F pick) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                if (!w.inBounds(x, y))
                    continue;
                if (!(x < x0 + 2 || x > x1 - 2 || y < y0 + 2 || y > y1 - 2))
                    continue;
                Ground g = w.ground(x, y);
                if (g == G_WATER || g == G_BRIDGE_H || g == G_BRIDGE_V || g == G_GLOWSTONE)
                    continue;
                w.setObj(x, y, pick(hash2(x, y, 91)));
                w.keep_[w.idx(x, y)] = 1;
            }
    }
    Prop *prop(PropKind k, const std::string &id, int tx, int ty, bool solid = false) {
        auto p = std::make_unique<Prop>();
        p->kind = k;
        p->id = id;
        p->tx = tx;
        p->ty = ty;
        p->pos = vec2((tx + 0.5f) * TILE, (ty + 1.f) * TILE);
        if (solid)
            w.setObj(tx, ty, O_BLOCKER);
        return static_cast<Prop *>(w.add(std::move(p)));
    }
    Prop *gate(PropKind k, const std::string &id, int tx, int ty, int tw, int th,
               const std::string &key) {
        auto p = std::make_unique<Prop>();
        p->kind = k;
        p->id = id;
        p->tx = tx;
        p->ty = ty;
        p->tw = tw;
        p->th = th;
        p->needKey = key;
        p->pos = vec2((tx + tw * 0.5f) * TILE, (ty + th * 0.5f) * TILE);
        fillO(tx, ty, tx + tw - 1, ty + th - 1, O_BLOCKER);
        return static_cast<Prop *>(w.add(std::move(p)));
    }
    Prop *deco(const std::string &sprite, float cx, float baseY, int bx0, int by0, int bx1,
               int by1) {
        auto p = std::make_unique<Prop>();
        p->kind = PropKind::Deco;
        p->sprite = sprite;
        p->pos = vec2(cx, baseY);
        p->canInteract = false;
        fillO(bx0, by0, bx1, by1, O_BLOCKER);
        return static_cast<Prop *>(w.add(std::move(p)));
    }
    void npc(const std::string &id, const std::string &name, const std::string &sprite, int tx,
             int ty, int facing, std::vector<std::string> lines, std::vector<std::string> repeat) {
        auto n = std::make_unique<NPC>();
        n->id = id;
        n->name = name;
        n->sprite = sprite;
        n->facing = facing;
        n->lines = std::move(lines);
        n->repeatLines = std::move(repeat);
        n->pos = vec2((tx + 0.5f) * TILE, (ty + 1.f) * TILE);
        n->phase = hf(tx, ty) * 5.f;
        w.setObj(tx, ty, O_BLOCKER);
        w.add(std::move(n));
    }
    void sign(const std::string &id, int tx, int ty, const std::string &text) {
        prop(PropKind::Sign, id, tx, ty, true)->text = text;
    }
    void slot(AreaId a, int tx, int ty, const std::string &secret = "") {
        w.slots_.push_back({a, tx, ty, secret});
        keep(tx - 1, ty - 1, tx + 1, ty + 1);
        w.setObj(tx, ty, O_BLOCKER); // chest tile is solid
    }

    template <class F>
    void scatter(int x0, int y0, int x1, int y1, float density, uint32_t seed, F pick) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                if (!w.inBounds(x, y) || w.keep_[w.idx(x, y)] || w.obj_[w.idx(x, y)] != O_NONE)
                    continue;
                Ground g = w.ground(x, y);
                if (g == G_WATER || g == G_DIRT || g == G_BRIDGE_H || g == G_BRIDGE_V ||
                    g == G_GLOWSTONE)
                    continue;
                if (hf(x, y, seed) < density) {
                    Obj o = pick(hash2(x, y, seed + 7));
                    if (o != O_NONE)
                        w.setObj(x, y, o);
                }
            }
    }
    template <class F> void decorate(int x0, int y0, int x1, int y1, uint32_t seed, F pick) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                if (!w.inBounds(x, y) || w.solidTile(x, y) || w.decor_[w.idx(x, y)])
                    continue;
                Decor d = pick(x, y, hf(x, y, seed), w.ground(x, y));
                if (d != D_NONE)
                    w.decor_[w.idx(x, y)] = d;
            }
    }
};

// Lay out the complete adventure map. The areas are built in a predictable order
// so paths, puzzle spaces and landmarks line up with the entity placements later on.
void World::build(const Config &cfg, GameState &st) {
    (void)cfg;
    ground_.assign((size_t)W * H, G_GRASS);
    obj_.assign((size_t)W * H, O_NONE);
    decor_.assign((size_t)W * H, D_NONE);
    var_.assign((size_t)W * H, 0);
    area_.assign((size_t)W * H, 0);
    keep_.assign((size_t)W * H, 0);
    ents_.clear();
    slots_.clear();
    player_ = nullptr;
    pip_ = nullptr;
    bridge_ = false;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            var_[idx(x, y)] = (uint8_t)(hash2(x, y, 5) & 255);
            AreaId a = (x < 80)
                           ? (y < 56 ? AreaId::Garden : (x < 40 ? AreaId::Forest : AreaId::Lake))
                           : (y < 31 ? AreaId::Final : AreaId::Castle);
            area_[idx(x, y)] = (uint8_t)a;
        }
    LevelBuilder L(*this);
    // ------------------------------------------------ base ground and solid
    // surroundings
    auto playable = [&](int x, int y) {
        if (x >= 4 && x <= 39 && y >= 58 && y <= 93)
            return true; // forest
        if (x >= 41 && x <= 78 && y >= 59 && y <= 93)
            return true; // lake
        if (x >= 24 && x <= 78 && y >= 12 && y <= 54)
            return true; // garden
        if (x >= 59 && x <= 60 && y >= 55 && y <= 58)
            return true; // bridge landing
        if (x >= 81 && x <= 118 && y >= 33 && y <= 93)
            return true; // castle compound
        if (x >= 83 && x <= 116 && y >= 4 && y <= 28)
            return true; // final plateau
        if (x >= 99 && x <= 102 && y >= 29 && y <= 31)
            return true; // stairs between castle and plateau
        if (x == 40 && y >= 76 && y <= 77)
            return true; // forest gate
        if (x == 80 && y >= 50 && y <= 51)
            return true; // castle gate
        if (x >= 41 && x <= 78 && y >= 56 && y <= 58)
            return true; // river
        return false;
    };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            AreaId a = areaAt(x, y);
            Ground g = G_GRASS;
            if (a == AreaId::Garden)
                g = G_MOSS;
            else if (a == AreaId::Castle)
                g = G_STONE;
            else if (a == AreaId::Final)
                g = G_GRASS;
            ground_[idx(x, y)] = g;
            if (!playable(x, y)) {
                uint32_t h = hash2(x, y, 3);
                Obj o = O_TREE;
                switch (a) {
                case AreaId::Forest:
                    o = (h % 5 == 0) ? O_PINE : (h % 2 ? O_TREE : O_TREE2);
                    break;
                case AreaId::Lake:
                    o = (h % 3 == 0) ? O_PINE : (h % 2 ? O_TREE : O_TREE2);
                    break;
                case AreaId::Garden:
                    o = (h & 1) ? O_GTREE : O_GTREE2;
                    break;
                case AreaId::Castle:
                    o = O_WALL;
                    break;
                case AreaId::Final:
                    o = (h % 3 == 0) ? O_PINE : ((h & 1) ? O_GTREE : O_GTREE2);
                    break;
                default:
                    break;
                }
                obj_[idx(x, y)] = o;
            }
        }
    // river between lake and garden
    L.fillG(41, 56, 78, 58, G_WATER);
    L.fillO(41, 56, 78, 58, O_NONE);
    for (int x = 41; x <= 78; x++) {
        for (int y = 56; y <= 58; y++)
            keep_[idx(x, y)] = 1;
    }

    // ================================================== FOREST
    {
        L.clearing(10, 88, 5.5f, G_GRASS);
        L.hline(8, 25, 88);
        L.vline(74, 89, 24);
        L.hline(24, 39, 76);
        L.hline(24, 29, 86);
        L.clearing(31, 86, 4.f, G_GRASS);
        L.vline(77, 88, 8);
        L.clearing(7, 76, 3.f, G_GRASS);
        L.hline(10, 23, 74);
        L.vline(66, 74, 10);
        L.clearing(10, 66, 6.f, G_GRASS);
        L.vline(66, 76, 32);
        L.clearing(32, 63, 4.f, G_GRASS);
        L.keep(38, 75, 40, 78);
        L.fillO(39, 76, 39, 77, O_NONE);
        // chests & secrets
        L.slot(AreaId::Forest, 31, 86);
        L.slot(AreaId::Forest, 7, 76);
        L.slot(AreaId::Forest, 32, 63, "secret_forest_chest");
        auto forestTree = [](uint32_t h) { return h % 5 == 0 ? O_PINE : (h % 2 ? O_TREE : O_TREE2); };
        L.sanctum(26, 56, 39, 71, forestTree); // chest alcove, only entrance is the bush
        L.secretBlock(32, 70, 2, 2, O_SECRET_BUSH, "secret_forest_chest");
        L.sanctum(2, 56, 18, 72, forestTree); // fairy ring, only entrance is the bush
        L.secretBlock(10, 71, 2, 2, O_SECRET_BUSH, "secret_forest_key");
        // fairy ring of mushrooms around the pedestal
        for (int i = 0; i < 12; i++) {
            float a = i * PI / 6.f;
            int x = (int)std::lround(10 + std::cos(a) * 3.2f),
                y = (int)std::lround(66 + std::sin(a) * 3.2f);
            decor_[idx(x, y)] = (i & 1) ? D_MUSH_B : D_MUSH_R;
        }
        // scatter
        L.scatter(4, 58, 39, 93, 0.13f, 11, [](uint32_t h) {
            uint32_t k = h % 20;
            return k < 7    ? O_TREE
                   : k < 10 ? O_TREE2
                   : k < 13 ? O_PINE
                   : k < 15 ? O_BUSH
                   : k < 16 ? O_BUSH2
                   : k < 17 ? O_ROCK
                   : k < 18 ? O_STUMP
                   : k < 19 ? O_BIGMUSH
                            : O_BUSH3;
        });
        L.decorate(4, 58, 39, 93, 21, [](int, int, float r, Ground g) {
            if (g != G_GRASS)
                return D_NONE;
            if (r < 0.16f)
                return D_TUFT0;
            if (r < 0.26f)
                return D_TUFT1;
            if (r < 0.30f)
                return D_FLOWER_Y;
            if (r < 0.33f)
                return D_FLOWER_W;
            if (r < 0.355f)
                return D_FLOWER_B;
            if (r < 0.375f)
                return D_FLOWER_P;
            if (r < 0.385f)
                return D_PEBBLES;
            if (r < 0.40f)
                return D_MUSH_R;
            if (r < 0.41f)
                return D_MUSH_B;
            if (r < 0.42f)
                return D_LEAF;
            return D_NONE;
        });
        L.sign("sign_move", 12, 90,
               "Move with WASD or the arrow keys.\nHold SHIFT to run.\nPress E "
               "near things to interact.");
        L.sign("sign_chest", 28, 88,
               "Treasure chests hide photographs and letters.\nStand next to one "
               "and press E to open it.");
        L.sign("sign_secret", 20, 76,
               "Some bushes rustle when you pass.\nIf a trail ends at a suspicious "
               "bush... press E to search it.");
        L.npc("npc_oak", "Old Oak", "oak", 21, 82, 3,
              {"Well, hello there, {name}! I am the Old Oak. I have watched over "
               "this forest for a very long time.",
               "Someone has hidden memories in treasure chests all over this "
               "world. Every one of them is meant for you.",
               "To leave the forest you will need the Forest Key. It rests where "
               "the mushrooms dance in a ring, north-west of here.",
               "The trail there is blocked by a bush that rustles. Press E on "
               "suspicious plants to search them!"},
              {"The Forest Key sleeps in the fairy ring, north-west. Search the "
               "rustling bush on the trail."});
        L.prop(PropKind::Pedestal, "pedestal_forest", 10, 66, true);
        L.gate(PropKind::Gate, "forest_gate", 40, 76, 1, 2, "forest_key");
    }

    // ================================================== LAKE
    {
        // water body
        for (int y = 59; y <= 93; y++)
            for (int x = 41; x <= 78; x++) {
                float dx = (x - 63) / 13.f, dy = (y - 76) / 11.f;
                float d = dx * dx + dy * dy;
                if (d <= 1.f) {
                    ground_[idx(x, y)] = G_WATER;
                    obj_[idx(x, y)] = O_NONE;
                } else if (d <= 1.35f)
                    ground_[idx(x, y)] = G_SAND;
            }
        L.disc(63, 76, 3.9f, [&](int x, int y) { ground_[idx(x, y)] = G_SAND; });
        L.disc(63, 76, 2.6f, [&](int x, int y) { ground_[idx(x, y)] = G_GRASS; });
        L.keep(59, 72, 68, 80);
        L.hline(41, 47, 76);
        L.vline(61, 91, 46);
        L.hline(46, 60, 62);
        L.vline(59, 62, 59);
        L.hline(46, 73, 90);
        L.hline(60, 69, 62);
        L.clearing(75, 62, 3.f, G_GRASS);
        L.clearing(74, 91, 3.f, G_GRASS);
        for (int x = 50; x <= 59; x++) {
            ground_[idx(x, 76)] = G_GLOWSTONE;
            keep_[idx(x, 76)] = 1;
        }
        for (int x = 67; x <= 75; x++) {
            ground_[idx(x, 76)] = G_GLOWSTONE;
            keep_[idx(x, 76)] = 1;
        }
        L.slot(AreaId::Lake, 74, 90);
        L.slot(AreaId::Lake, 63, 76);
        L.slot(AreaId::Lake, 75, 62, "secret_lake_cove");
        L.sanctum(70, 57, 79, 67, [](uint32_t h) { return h % 3 == 0 ? O_PINE : (h % 2 ? O_TREE : O_TREE2); });
        L.secretBlock(70, 62, 2, 2, O_SECRET_BUSH, "secret_lake_cove");
        L.prop(PropKind::Lever, "bridge_lever", 62, 60, true);
        L.sign("sign_lake", 44, 74,
               "THE CRYSTAL LAKE\nThe glowing stones in the water are safe to walk "
               "on.");
        L.npc("npc_frog", "Fergus the Fisher", "frog", 44, 69, 3,
              {"Ribbit! Fergus the fisher, at your service. Welcome to Crystal "
               "Lake, {name}!",
               "See the glowing stones in the water? Walk close and they light up. "
               "They lead to a little island.",
               "And that lever by the northern bridge? It has a little socket "
               "and will not budge on its own. The island shard might fit it."},
              {"Lever by the northern path. It wants the shard from the island. "
               "Ribbit."});
        L.scatter(41, 59, 78, 93, 0.09f, 31, [&](uint32_t h) {
            uint32_t k = h % 14;
            return k < 4    ? O_PINE
                   : k < 7  ? O_TREE
                   : k < 9  ? O_BUSH
                   : k < 10 ? O_ROCK
                   : k < 11 ? O_ROCK2
                   : k < 12 ? O_CRYSTAL0
                   : k < 13 ? O_CRYSTAL1
                            : O_BUSH2;
        });
        for (int y = 59; y <= 93; y++)
            for (int x = 41; x <= 78; x++) // reeds & crystals on the beach
                if (ground_[idx(x, y)] == G_SAND && !keep_[idx(x, y)] &&
                    obj_[idx(x, y)] == O_NONE) {
                    float r = hf(x, y, 41);
                    if (r < 0.10f)
                        obj_[idx(x, y)] = O_REEDS;
                    else if (r < 0.125f)
                        obj_[idx(x, y)] = (r < 0.112f) ? O_CRYSTAL0 : O_CRYSTAL1;
                }
        L.decorate(41, 59, 78, 93, 22, [](int, int, float r, Ground g) {
            if (g == G_WATER)
                return r < 0.05f ? D_LILY : D_NONE;
            if (g == G_SAND)
                return r < 0.05f ? D_PEBBLES : D_NONE;
            if (g != G_GRASS)
                return D_NONE;
            if (r < 0.16f)
                return D_TUFT0;
            if (r < 0.26f)
                return D_TUFT1;
            if (r < 0.30f)
                return D_FLOWER_B;
            if (r < 0.33f)
                return D_FLOWER_W;
            if (r < 0.35f)
                return D_FLOWER_P;
            return D_NONE;
        });
    }

    // ================================================== GARDEN
    {
        L.vline(46, 55, 59);
        L.hline(45, 60, 46);
        L.clearing(45, 33, 8.5f, G_PLAZA);
        L.vline(40, 47, 45);
        L.hline(60, 79, 50);
        L.hline(31, 45, 24);
        L.vline(15, 27, 40);
        L.clearing(40, 15, 3.5f, G_MOSS);
        L.clearing(30, 22, 5.f, G_MOSS);
        L.vline(31, 50, 63);
        L.fillO(79, 50, 79, 51, O_NONE);
        // maze 15x15 at (62,16)
        const int mx = 62, my = 16;
        for (int y = 0; y < 15; y++)
            for (int x = 0; x < 15; x++) {
                obj_[idx(mx + x, my + y)] = O_HEDGE;
                ground_[idx(mx + x, my + y)] = G_MOSS;
                keep_[idx(mx + x, my + y)] = 1;
            }
        {
            Rng rng(1234);
            bool vis[7][7] = {};
            std::vector<ivec2> stack{{0, 6}};
            vis[6][0] = true;
            obj_[idx(mx + 1, my + 13)] = O_NONE;
            while (!stack.empty()) {
                ivec2 c = stack.back();
                std::vector<ivec2> nb;
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int i = 0; i < 4; i++) {
                    int nx = c.x + dx[i], ny = c.y + dy[i];
                    if (nx >= 0 && ny >= 0 && nx < 7 && ny < 7 && !vis[ny][nx])
                        nb.push_back({nx, ny});
                }
                if (nb.empty()) {
                    stack.pop_back();
                    continue;
                }
                ivec2 n = nb[rng.i(0, (int)nb.size() - 1)];
                vis[n.y][n.x] = true;
                obj_[idx(mx + 1 + 2 * n.x, my + 1 + 2 * n.y)] = O_NONE;
                obj_[idx(mx + 1 + c.x + n.x, my + 1 + c.y + n.y)] = O_NONE;
                stack.push_back(n);
            }
            obj_[idx(mx + 1, my + 14)] = O_NONE; // entrance
            // BFS over cells to find the farthest dead ends
            int dist[15][15];
            for (auto &r : dist)
                for (int &v : r)
                    v = -1;
            std::queue<ivec2> q;
            q.push({1, 13});
            dist[13][1] = 0;
            std::vector<std::pair<int, ivec2>> cells;
            while (!q.empty()) {
                ivec2 c = q.front();
                q.pop();
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int i = 0; i < 4; i++) {
                    int nx = c.x + dx[i], ny = c.y + dy[i];
                    if (nx < 0 || ny < 0 || nx > 14 || ny > 14 || dist[ny][nx] >= 0 ||
                        obj_[idx(mx + nx, my + ny)] != O_NONE)
                        continue;
                    dist[ny][nx] = dist[c.y][c.x] + 1;
                    q.push({nx, ny});
                }
            }
            for (int j = 0; j < 7; j++)
                for (int i = 0; i < 7; i++)
                    cells.push_back({dist[1 + 2 * j][1 + 2 * i], {1 + 2 * i, 1 + 2 * j}});
            std::sort(cells.begin(), cells.end(),
                      [](auto &a, auto &b) { return a.first > b.first; });
            ivec2 c1 = cells[0].second, c2 = cells[1].second;
            L.slot(AreaId::Garden, mx + c1.x, my + c1.y);
            mazeShard_ = c2;
            mazeShard_ += ivec2(mx, my);
        }
        L.slot(AreaId::Garden, 30, 25);
        L.slot(AreaId::Garden, 40, 15, "secret_garden_glade");
        L.sanctum(35, 10, 46, 21, [](uint32_t h) { return (h & 1) ? O_GTREE : O_GTREE2; });
        L.secretBlock(40, 20, 2, 2, O_SECRET_BUSH, "secret_garden_glade");
        auto deco = L.deco("fountain", 45 * 16 + 8.f, 34 * 16.f, 44, 32, 46, 33);
        deco->lightKind = 3;
        deco->lightRadius = 70;
        auto gt = L.deco("greattree", 30 * 16.f + 8.f, 21 * 16.f, 29, 19, 30, 20);
        gt->lightKind = 2;
        gt->lightRadius = 90;
        // flower puzzle
        const ivec2 fp[4] = {{39, 33}, {45, 27}, {51, 33}, {45, 39}};
        for (int i = 0; i < 4; i++) {
            auto p = std::make_unique<Prop>();
            p->kind = PropKind::GlowFlower;
            p->id = "flower_" + std::to_string(i);
            p->index = i;
            p->pos = vec2((fp[i].x + 0.5f) * TILE, (fp[i].y + 1.f) * TILE);
            p->phase = (float)i;
            add(std::move(p));
        }
        L.sign("sign_flowers", 66, 48, "");
        L.sign("sign_garden", 58, 52,
               "THE STARLIGHT GARDEN\nThe plaza fountain lies north-west. East "
               "road: the Memory Castle.");
        L.npc("npc_moth", "Madame Moth", "moth", 41, 36, 3,
              {"Welcome to the Starlight Garden, {name}. My flowers only bloom for "
               "the right melody.",
               "The old sign along the eastern road remembers the order, but it "
               "speaks in riddles. Touch the four flowers around the fountain "
               "in that order.",
               "When they all shine, the fountain will give you a key. A key for "
               "the castle gate, far to the east."},
              {"Read the riddle on the eastern sign, then touch the glowing "
               "flowers in order."});
        for (int y = 12; y <= 54; y++)
            for (int x = 24; x <= 78; x++) { // night garden glow: ring of lamps along paths
                (void)x;
                (void)y;
            }
        for (ivec2 lp : {ivec2(58, 53), ivec2(61, 53), ivec2(58, 48), ivec2(61, 44), ivec2(47, 44),
                         ivec2(44, 44), ivec2(70, 49), ivec2(70, 52), ivec2(56, 25), ivec2(34, 26),
                         ivec2(62, 40)})
            if (!keep_[idx(lp.x, lp.y)] || obj_[idx(lp.x, lp.y)] == O_NONE) {
                if (ground_[idx(lp.x, lp.y)] != G_DIRT)
                    obj_[idx(lp.x, lp.y)] = O_LAMP;
            }
        L.scatter(24, 12, 78, 54, 0.10f, 51, [](uint32_t h) {
            uint32_t k = h % 12;
            return k < 4    ? O_GTREE
                   : k < 6  ? O_GTREE2
                   : k < 9  ? O_FBUSH
                   : k < 10 ? O_ROCK
                   : k < 11 ? O_CRYSTAL1
                            : O_BUSH3;
        });
        L.decorate(24, 12, 78, 54, 23, [](int, int, float r, Ground g) {
            if (g != G_MOSS && g != G_PLAZA)
                return D_NONE;
            if (g == G_PLAZA)
                return r < 0.03f ? D_FLOWER_W : D_NONE;
            if (r < 0.10f)
                return D_TUFT1;
            if (r < 0.20f)
                return D_FLOWER_P;
            if (r < 0.28f)
                return D_FLOWER_W;
            if (r < 0.34f)
                return D_FLOWER_B;
            if (r < 0.38f)
                return D_FLOWER_Y;
            if (r < 0.42f)
                return D_MUSH_B;
            if (r < 0.44f)
                return D_PEBBLES;
            return D_NONE;
        });
    }

    // ================================================== CASTLE
    {
        // courtyard: grass pockets, hedges, avenue
        L.fillG(81, 33, 118, 93, G_STONE);
        L.fillG(81, 61, 99, 93, G_STONE);
        L.fillG(104, 78, 117, 92, G_GRASS);
        L.fillG(82, 62, 83, 92, G_GRASS);
        L.fillG(86, 78, 95, 92, G_GRASS);
        L.fillO(104, 77, 117, 77, O_HEDGE);
        L.fillO(103, 78, 103, 92, O_HEDGE);
        L.fillO(104, 93, 117, 93, O_HEDGE);
        L.fillO(103, 77, 103, 77, O_HEDGE);
        L.fillO(85, 64, 94, 65, O_HEDGE);
        L.fillO(106, 64, 116, 65, O_HEDGE);
        L.fillO(103, 85, 103, 86, O_NONE); // opening into the garden room
        L.keep(81, 33, 118, 93);
        for (int y = 61; y <= 93; y++) {
            for (int x : {98, 103})
                if (obj_[idx(x, y)] == O_NONE && (y % 6 == 2))
                    obj_[idx(x, y)] = O_LAMP;
        }
        L.slot(AreaId::Castle, 110, 88);
        L.sign("sign_castle", 96, 62,
               "THE MEMORY CASTLE\nThe throne room is locked.\nA Golden Key hides "
               "somewhere inside...");
        // building shell
        L.fillO(84, 32, 117, 59, O_WALL);
        L.fillO(85, 33, 116, 58, O_NONE);
        L.fillG(85, 33, 116, 58, G_TILE);
        L.fillG(85, 33, 95, 45, G_WOOD);
        // dividers
        L.fillO(85, 46, 116, 46, O_WALL);
        L.fillO(96, 33, 96, 45, O_WALL);
        L.fillO(105, 33, 105, 45, O_WALL);
        L.fillO(90, 33, 90, 40, O_WALL);
        L.fillO(85, 40, 90, 40, O_WALL);
        // front entrance
        L.fillO(100, 59, 101, 59, O_NONE);
        L.fillG(100, 59, 101, 60, G_CARPET);
        L.fillG(100, 47, 101, 58, G_CARPET);
        L.fillG(100, 35, 101, 45, G_CARPET);
        L.fillG(100, 31, 101, 32, G_PLAZA);
        L.fillO(99, 31, 102, 31, O_NONE);
        L.fillG(99, 31, 102, 31, G_PLAZA);
        L.fillG(99, 30, 102, 30, G_PLAZA);
        // decoration of walls
        for (int x = 86; x <= 95; x++)
            if (x != 90)
                obj_[idx(x, 32)] = O_WALL_BOOK;
        for (int y = 41; y <= 45; y += 2) {
            obj_[idx(96, y)] = O_WALL_BOOK;
        }
        for (int x : {98, 103}) {
            obj_[idx(x, 32)] = O_WALL_TORCH;
            obj_[idx(x, 46)] = O_WALL_TORCH;
        }
        for (int x : {108, 111, 114})
            obj_[idx(x, 32)] = O_WALL_PAINT;
        for (int x : {88, 94, 106, 112})
            obj_[idx(x, 59)] = O_WALL_WINDOW;
        for (int x : {88, 94, 107, 113})
            obj_[idx(x, 46)] = O_WALL_TORCH;
        for (int x : {90, 95, 106, 111})
            for (int y : {50, 56})
                obj_[idx(x, y)] = O_PILLAR;
        // doors
        L.gate(PropKind::Door, "door_library", 90, 46, 1, 1, "");
        L.gate(PropKind::Door, "door_gallery", 111, 46, 1, 1, "");
        L.gate(PropKind::Door2, "throne_door", 100, 46, 2, 1, "golden_key");
        L.gate(PropKind::Door2, "grand_doors", 100, 32, 2, 1, "");
        L.gate(PropKind::Gate, "castle_gate", 80, 50, 1, 2, "garden_key");
        L.secretBlock(90, 37, 1, 1, O_SECRET_BOOK, "secret_library");
        auto th = L.prop(PropKind::Throne, "throne", 100, 34, false);
        th->pos = vec2(101 * 16.f, 35 * 16.f);
        L.fillO(100, 34, 101, 34, O_BLOCKER);
        L.slot(AreaId::Castle, 113, 52);
        L.slot(AreaId::Castle, 111, 36);
        L.slot(AreaId::Castle, 86, 34, "secret_library");
        L.prop(PropKind::Pedestal, "pedestal_golden", 88, 37, true);
        L.npc("npc_steward", "Sir Bram", "steward", 97, 53, 0,
              {"Halt! ...Oh, it is you, {name}. Welcome to the Memory Castle!",
               "The throne room is locked with a golden door. The Golden Key is "
               "hidden somewhere in the library, they say.",
               "The ghost painter in the gallery might remember more, if "
               "you bring him enough memory shards."},
              {"Golden Key: somewhere in the library. The painter in the gallery wants "
               "three memory shards."});
        L.npc("npc_ghost", "Mr. Pale", "ghost", 108, 41, 0,
              {"Oooh. A visitor. I paint the memories that hang on these walls.",
               "In the library, one bookcase is not quite like the others: the one "
               "in the wall beside the little closet. Press E on it.",
               "I would tell you more, but I am rather transparent."},
              {"The odd bookcase, in the library, beside the closet."});
        for (int x = 86; x <= 112; x += 4)
            if (x != 98 && x != 102 && x != 90 && x != 110) { /* torches already placed */
            }
        L.decorate(81, 61, 118, 93, 24, [](int, int, float r, Ground g) {
            if (g == G_GRASS) {
                if (r < 0.15f)
                    return D_TUFT0;
                if (r < 0.25f)
                    return D_FLOWER_P;
                if (r < 0.33f)
                    return D_FLOWER_W;
                if (r < 0.40f)
                    return D_FLOWER_Y;
            }
            return D_NONE;
        });
        L.scatter(81, 33, 83, 93, 0.0f, 3, [](uint32_t) { return O_NONE; });
    }

    // ================================================== FINAL CELEBRATION
    {
        L.clearing(101, 13, 11.f, G_PLAZA);
        L.fillG(99, 22, 102, 31, G_PLAZA);
        L.keep(98, 14, 103, 31);
        L.hline(103, 111, 22, G_DIRT);
        L.clearing(113, 22, 3.f, G_GRASS);
        L.sanctum(108, 17, 118, 27, [](uint32_t h) { return h % 3 == 0 ? O_PINE : ((h & 1) ? O_GTREE : O_GTREE2); });
        L.secretBlock(108, 22, 2, 2, O_SECRET_BUSH, "secret_final_nook");
        L.slot(AreaId::Final, 100, 16);
        auto cake = L.deco("cake", 101 * 16.f, 12 * 16.f, 99, 10, 102, 11);
        cake->id = "cake";
        cake->canInteract = true; // press E to blow out the candles
        cake->lightKind = 1;
        cake->lightRadius = 110;
        for (ivec2 lp : {ivec2(95, 21), ivec2(106, 21), ivec2(95, 27), ivec2(106, 27),
                         ivec2(92, 12), ivec2(110, 12), ivec2(96, 7), ivec2(106, 7)})
            obj_[idx(lp.x, lp.y)] = O_LAMP;
        L.npc("npc_bard", "Lyric the Bard", "bard", 96, 17, 3,
              {"You made it, {name}! Everyone has been waiting.",
               "The last chest only opens once every other memory has been found. "
               "Have you gathered them all?",
               "There is a hidden nook to the east, behind a bush that rustles, "
               "where one last star shard shimmers."},
              {"Every memory first, then the final chest. And check the bush to "
               "the east."});
        L.sign("sign_final", 99, 20,
               "Almost there...\nThe last treasure waits in front of the cake.");
        { // balloons
            Rng rng(99);
            for (int i = 0; i < 16; i++) {
                float a = i / 16.f * 2 * PI + rng.range(-0.1f, 0.1f), rr = rng.range(9.5f, 12.f);
                auto p = std::make_unique<Prop>();
                p->kind = PropKind::Balloon;
                p->index = rng.i(0, 5);
                p->phase = rng.range(0, 6.f);
                p->canInteract = false;
                p->pos = vec2((101 + std::cos(a) * rr) * TILE,
                              (13 + std::sin(a) * rr * 0.8f) * TILE + 20);
                int tx = (int)(p->pos.x / TILE), ty = (int)(p->pos.y / TILE);
                if (tx < 84 || tx > 115 || ty < 5 || ty > 27 || (tx >= 98 && tx <= 103 && ty >= 20))
                    continue;
                add(std::move(p));
            }
        }
        L.scatter(83, 4, 116, 28, 0.09f, 61, [](uint32_t h) {
            uint32_t k = h % 10;
            return k < 3   ? O_GTREE
                   : k < 5 ? O_GTREE2
                   : k < 7 ? O_PINE
                   : k < 8 ? O_FBUSH
                   : k < 9 ? O_BUSH3
                           : O_ROCK;
        });
        L.decorate(83, 4, 116, 28, 25, [](int, int, float r, Ground g) {
            if (g == G_GRASS) {
                if (r < 0.2f)
                    return D_TUFT0;
                if (r < 0.3f)
                    return D_FLOWER_P;
                if (r < 0.38f)
                    return D_FLOWER_Y;
                if (r < 0.44f)
                    return D_FLOWER_R;
                if (r < 0.5f)
                    return D_FLOWER_W;
            }
            if (g == G_PLAZA && r < 0.04f)
                return D_FLOWER_W;
            return D_NONE;
        });
    }
    // slot ordering: visible chests first within each area
    std::stable_sort(slots_.begin(), slots_.end(), [](const ChestSlot &a, const ChestSlot &b) {
        if (a.area != b.area)
            return (int)a.area < (int)b.area;
        return a.secretId.empty() && !b.secretId.empty();
    });
    start_ = vec2(10 * TILE + 8.f, 88 * TILE + 8.f);
    (void)st;
}

// ============================================================ entities
// Place the things the player can actually interact with: collectibles, props,
// gates, chests and the characters that bring each area to life. The map geometry
// is already in place by the time this function runs.
void World::placeEntities(const Config &cfg, GameState &st) {
    // collectibles
    auto shardIf = [&](const std::string &id, int tx, int ty) {
        if (!st.shards.count(id))
            addCollectible(ItemKind::Shard, id, tx, ty);
    };
    shardIf("shard_forest", 6, 63);
    shardIf("shard_lake", 77, 76);
    shardIf("shard_garden", mazeShard_.x, mazeShard_.y);
    shardIf("shard_castle", 114, 43);
    shardIf("shard_final", 113, 22);
    if (!st.keys.count("forest_key") && !st.flag("forest_gate") && st.flag("secret_forest_key"))
        addCollectible(ItemKind::KeyForest, "forest_key", 10, 65);
    if (!st.keys.count("golden_key") && !st.flag("throne_door"))
        addCollectible(ItemKind::KeyGolden, "golden_key", 88, 36);
    if (st.flag("garden_solved") && !st.keys.count("garden_key") && !st.flag("castle_gate"))
        addCollectible(ItemKind::KeyGarden, "garden_key", 45, 36);

    // props' state
    if (!cfg.guided)
        for (const char *f : {"forest_gate", "castle_gate", "throne_door", "grand_doors",
                              "bridge_lever", "door_library", "door_gallery", "library_clue"})
            st.setFlag(f);
    for (auto &e : ents_) {
        if (e->type != EType::Prop)
            continue;
        Prop *p = static_cast<Prop *>(e.get());
        if ((p->kind == PropKind::Gate || p->kind == PropKind::Door || p->kind == PropKind::Door2 ||
             p->kind == PropKind::Secret) &&
            st.flag(p->id))
            openGate(*p, true);
        // The hidden library passage stays inert (no prompt, no twinkle) until the
        // ghost painter has given his clue.
        if (p->id == "secret_library" && !p->open)
            p->canInteract = st.flag("library_clue");
        if (p->kind == PropKind::Lever && st.flag(p->id)) {
            p->open = true;
            p->canInteract = false;
            extendBridge();
        }
        if (p->kind == PropKind::GlowFlower)
            p->lit = st.flag("garden_solved") || (flowerOrder.size() == 4 && [&] {
                         for (int k = 0; k < st.flowerProgress; k++)
                             if (flowerOrder[k] == p->index)
                                 return true;
                         return false;
                     }());
    }

    // chests -> slots (config order, visible slots first, overflow moves to
    // nearest free slot)
    std::vector<bool> used(slots_.size(), false);
    std::vector<const ChestDef *> overflow;
    for (const ChestDef &d : cfg.chests) {
        int pick = -1;
        for (size_t i = 0; i < slots_.size(); i++)
            if (!used[i] && slots_[i].area == d.area) {
                pick = (int)i;
                break;
            }
        if (pick < 0) {
            overflow.push_back(&d);
            continue;
        }
        used[pick] = true;
        slotOf_[d.id] = pick;
    }
    for (const ChestDef *d : overflow) {
        int pick = -1, best = 99;
        for (size_t i = 0; i < slots_.size(); i++) {
            if (used[i] || slots_[i].area == AreaId::Final)
                continue;
            int dist = std::abs((int)slots_[i].area - (int)d->area);
            if (dist < best) {
                best = dist;
                pick = (int)i;
            }
        }
        if (pick < 0) {
            Log::warn("No free chest slot for chest id %d - it is not placed", d->id);
            continue;
        }
        used[pick] = true;
        slotOf_[d->id] = pick;
        Log::warn("Area '%s' has no free slot for chest id %d - moved to %s", areaKey(d->area),
                  d->id, areaKey(slots_[pick].area));
    }
    for (const ChestDef &d : cfg.chests) {
        auto it = slotOf_.find(d.id);
        if (it == slotOf_.end())
            continue;
        const ChestSlot &s = slots_[it->second];
        auto c = std::make_unique<Chest>();
        c->id = d.id;
        c->isFinal = d.final;
        c->pos = vec2((s.tx + 0.5f) * TILE, (s.ty + 1.f) * TILE);
        if (st.chestsOpened.count(d.id))
            c->setOpenInstant();
        add(std::move(c));
    }
    // secret passages that lead to an unused chest slot simply stand open
    static const char *chestSecrets[] = {"secret_forest_chest", "secret_lake_cove",
                                         "secret_garden_glade"};
    for (const char *sid : chestSecrets) {
        bool needed = false;
        for (size_t i = 0; i < slots_.size(); i++)
            if (used[i] && slots_[i].secretId == sid)
                needed = true;
        if (!needed)
            if (Prop *p = propById(sid)) {
                openGate(*p, true);
                st.setFlag(sid);
            }
    }
}

// Generate both parts of the world in one call. Keeping the order here makes sure
// the map exists before entities are positioned on top of it.
void World::generate(const Config &cfg, GameState &st) {
    Rng rng(st.seed * 7919u + 13u);
    flowerOrder = {0, 1, 2, 3};
    for (int i = 3; i > 0; i--)
        std::swap(flowerOrder[i], flowerOrder[rng.i(0, i)]);
    slotOf_.clear();
    build(cfg, st);
    placeEntities(cfg, st);
    auto pl = std::make_unique<Player>();
    vec2 sp = start_;
    if (st.playerPos != vec2(0, 0))
        sp = st.playerPos;
    pl->pos = sp;
    pl->facing = st.facing;
    if (blocked(Player::box(pl->pos)))
        pl->pos = start_;
    player_ = static_cast<Player *>(add(std::move(pl)));
    auto pp = std::make_unique<Companion>();
    pp->pos = player_->pos + vec2(10, -30);
    pip_ = static_cast<Companion *>(add(std::move(pp)));
    time_ = 0;
}

// ============================================================ update / draw
// Updating handles moving world objects; drawing is kept separate so rendering can
// happen as often as needed without changing the state of the world.
void World::update(float dt) {
    time_ += dt;
    for (auto &e : ents_)
        e->update(dt, *this);
    ents_.erase(std::remove_if(ents_.begin(), ents_.end(),
                               [](const std::unique_ptr<Entity> &e) { return e->removed; }),
                ents_.end());
}

// Draw the map in layers. Ground and flat decoration go down first, then objects
// and characters are sorted together so things in front naturally cover things behind.
void World::draw(Renderer &r, const Atlas &a, vec2 cam, float zoom, float time, vec2 pp) const {
    (void)zoom;
    vec2 view = r.viewSize();
    int x0 = std::max(0, (int)std::floor((cam.x - view.x * 0.5f) / TILE) - 1),
        x1 = std::min(W - 1, (int)std::floor((cam.x + view.x * 0.5f) / TILE) + 1);
    int y0 = std::max(0, (int)std::floor((cam.y - view.y * 0.5f) / TILE) - 1),
        y1 = std::min(H - 1, (int)std::floor((cam.y + view.y * 0.5f) / TILE) + 1);
    const Texture &T = a.tex();
    auto isWaterLike = [&](int x, int y) {
        Ground g = ground(x, y);
        return g == G_WATER || g == G_BRIDGE_H || g == G_BRIDGE_V || g == G_GLOWSTONE ||
               !inBounds(x, y);
    };
    auto sameGround = [&](int x, int y, Ground g) { return !inBounds(x, y) || ground(x, y) == g; };
    auto maskOf = [&](int x, int y, auto pred) {
        return (pred(x, y - 1) ? 1 : 0) | (pred(x + 1, y) ? 2 : 0) | (pred(x, y + 1) ? 4 : 0) |
               (pred(x - 1, y) ? 8 : 0);
    };

    // ---- ground
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            Ground g = ground(x, y);
            int v = var_[idx(x, y)] & 3;
            float px = (float)x * TILE, py = (float)y * TILE;
            switch (g) {
            case G_DIRT:
                r.drawSprite(T, a.spr(groundSpr_[G_GRASS][v]), px, py);
                r.drawSprite(
                    T,
                    a.spr(pathSpr_[maskOf(
                        x, y, [&](int xx, int yy) { return sameGround(xx, yy, G_DIRT); })]),
                    px, py);
                break;
            case G_CARPET:
                r.drawSprite(T, a.spr(groundSpr_[G_TILE][((x + y) & 1)]), px, py);
                r.drawSprite(
                    T,
                    a.spr(carpetSpr_[maskOf(
                        x, y, [&](int xx, int yy) { return sameGround(xx, yy, G_CARPET); })]),
                    px, py);
                break;
            case G_TILE:
                r.drawSprite(T, a.get(((x + y) & 1) ? "tile_b" : "tile_a"), px, py);
                break;
            case G_WATER:
            case G_BRIDGE_H:
            case G_BRIDGE_V:
            case G_GLOWSTONE: {
                int m = maskOf(x, y, isWaterLike);
                int f = ((int)(time * 2.4f) + (x * 3 + y * 5)) & 3;
                r.drawSprite(T, a.spr(waterSpr_[m][f]), px, py);
                if (g == G_BRIDGE_V)
                    r.drawSprite(T, a.get("bridge_v"), px, py);
                else if (g == G_BRIDGE_H)
                    r.drawSprite(T, a.get("bridge_h"), px, py);
                else if (g == G_GLOWSTONE) {
                    float d = glm::length(vec2(px + 8, py + 8) - pp);
                    float al = 0.35f + 0.65f * (1.f - smoothstepf(20.f, 90.f, d));
                    r.drawSprite(T, a.get("glowstone"), px,
                                 py + std::floor(std::sin(time * 1.6f + x) * 0.8f),
                                 vec4(1, 1, 1, al));
                }
                break;
            }
            default:
                r.drawSprite(T, a.spr(groundSpr_[g][v]), px, py);
                break;
            }
        }
    // ---- decor (flat things lying on the ground)
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint8_t d = decor_[idx(x, y)];
            if (!d)
                continue;
            const Spr &s = a.spr(decorSpr_[d]);
            uint8_t vv = var_[idx(x, y)];
            if (d == D_LILY) {
                r.drawSprite(T, s,
                             x * TILE + std::floor(std::sin(time * 0.8f + x * 1.3f + y) * 1.5f),
                             y * TILE + std::floor(std::cos(time * 0.6f + y) * 1.f));
                continue;
            }
            float ox = (float)(vv & 7), oy = (float)((vv >> 3) & 7);
            if (s.w >= 16) {
                ox = 0;
                oy = 0;
            }
            vec4 tint = vec4(1);
            if (d == D_TUFT0 || d == D_TUFT1) {
                ox = (float)(vv & 7);
            }
            r.drawSprite(T, s, x * TILE + ox, y * TILE + oy, tint);
        }

    // ---- y-sorted objects + entities
    std::vector<const Entity *> es;
    es.reserve(ents_.size());
    float ex0 = cam.x - view.x * 0.5f - 80, ex1 = cam.x + view.x * 0.5f + 80,
          ey0 = cam.y - view.y * 0.5f - 100, ey1 = cam.y + view.y * 0.5f + 100;
    for (auto &e : ents_) {
        if (e->type == EType::Prop) {
            const Prop *p = static_cast<const Prop *>(e.get());
            if (p->kind == PropKind::Gate || p->kind == PropKind::Door ||
                p->kind == PropKind::Door2) {
                es.push_back(e.get());
                continue;
            }
        }
        if (e->pos.x < ex0 || e->pos.x > ex1 || e->pos.y < ey0 || e->pos.y > ey1 + 100)
            continue;
        es.push_back(e.get());
    }
    std::stable_sort(es.begin(), es.end(),
                     [](const Entity *a, const Entity *b) { return a->sortY() < b->sortY(); });
    size_t ei = 0;
    int yEnd = std::min(H - 1, y1 + 6);
    for (int y = y0; y <= yEnd; y++) {
        while (ei < es.size() && (int)std::floor(es[ei]->sortY() / TILE) <= y) {
            es[ei]->draw(r, a, time);
            ei++;
        }
        for (int x = std::max(0, x0 - 2); x <= std::min(W - 1, x1 + 2); x++) {
            Obj o = (Obj)obj_[idx(x, y)];
            if (o == O_NONE || o == O_BLOCKER)
                continue;
            float px = (float)x * TILE, py = (float)y * TILE;
            if (isWallish(o)) {
                bool below = isWallish(obj(x, y + 1)) ||
                             (inBounds(x, y + 1) && obj(x, y + 1) == O_BLOCKER && false);
                if (o == O_WALL) {
                    r.drawSprite(T, a.get(below ? "wall_top" : "wall_face"), px, py);
                } else {
                    int sid = objSpr_[o];
                    if (o == O_WALL_PAINT)
                        sid = a.id((x + y) & 1 ? "wall_paint1" : "wall_paint0");
                    r.drawSprite(T, a.spr(sid), px, py);
                    if (o == O_WALL_TORCH) {
                        int fr = ((int)(time * 7.f + x * 1.3f)) & 3;
                        r.drawSprite(T, a.get("flame_" + std::to_string(fr)), px + 4, py - 1);
                    }
                }
                continue;
            }
            int sid = objSpr_[o];
            if (o == O_REEDS)
                sid = a.id(((int)(time * 1.8f + x) & 1) ? "reeds1" : "reeds0");
            if (o == O_SECRET_BUSH)
                sid = objSpr_[areaAt(x, y) == AreaId::Garden ? O_FBUSH : O_BUSH3];
            const Spr &s = a.spr(sid);
            float sx = px + TILE * 0.5f - s.w * 0.5f, sy = py + TILE - s.h;
            if (o == O_TREE || o == O_TREE2 || o == O_PINE || o == O_GTREE || o == O_GTREE2)
                sx += (float)((var_[idx(x, y)] % 3) - 1);
            if (o == O_SECRET_BUSH)
                sx += std::floor(std::sin(time * 2.3f + x * 2.f + y) * 0.8f);
            r.drawSprite(T, s, std::floor(sx), std::floor(sy));
        }
    }
    while (ei < es.size()) {
        es[ei]->draw(r, a, time);
        ei++;
    }
}

void World::collectLights(const Atlas &, vec2 cam, vec2 view, float time, vec2 pp,
                          std::vector<Light> &out) const {
    int x0 = std::max(0, (int)std::floor((cam.x - view.x * 0.5f) / TILE) - 5),
        x1 = std::min(W - 1, (int)std::floor((cam.x + view.x * 0.5f) / TILE) + 5);
    int y0 = std::max(0, (int)std::floor((cam.y - view.y * 0.5f) / TILE) - 5),
        y1 = std::min(H - 1, (int)std::floor((cam.y + view.y * 0.5f) / TILE) + 8);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            Obj o = (Obj)obj_[idx(x, y)];
            vec2 c((x + 0.5f) * TILE, (y + 0.5f) * TILE);
            float fl = 1.f + 0.07f * std::sin(time * 8.f + x * 3.1f + y);
            switch (o) {
            case O_LAMP:
                out.push_back({c + vec2(0, -14), 84.f, vec4(1.f, 0.82f, 0.48f, 0.95f)});
                break;
            case O_WALL_TORCH:
                out.push_back({c + vec2(0, 6), 66.f * fl, vec4(1.f, 0.7f, 0.35f, 0.95f)});
                break;
            case O_CRYSTAL0:
                out.push_back({c + vec2(0, -6), 48.f, vec4(0.5f, 0.8f, 1.f, 0.8f)});
                break;
            case O_CRYSTAL1:
                out.push_back({c + vec2(0, -6), 48.f, vec4(1.f, 0.55f, 0.9f, 0.8f)});
                break;
            case O_GTREE:
            case O_GTREE2:
                out.push_back({c + vec2(0, -14), 34.f, vec4(0.7f, 0.55f, 1.f, 0.35f)});
                break;
            case O_BIGMUSH:
                out.push_back({c + vec2(0, -4), 22.f, vec4(1.f, 0.45f, 0.4f, 0.4f)});
                break;
            case O_SECRET_BUSH:
                break;
            default:
                break;
            }
            if (ground_[idx(x, y)] == G_GLOWSTONE) {
                float d = glm::length(c - pp), k = 1.f - smoothstepf(20.f, 90.f, d);
                out.push_back({c, 26.f, vec4(0.5f, 0.75f, 1.f, 0.25f + 0.55f * k)});
            }
            uint8_t dc = decor_[idx(x, y)];
            if (dc == D_MUSH_B)
                out.push_back({c, 14.f, vec4(0.5f, 0.7f, 1.f, 0.5f)});
        }
    for (auto &e : ents_) {
        if (e->pos.x < cam.x - view.x * 0.5f - 120 || e->pos.x > cam.x + view.x * 0.5f + 120 ||
            e->pos.y < cam.y - view.y * 0.5f - 120 || e->pos.y > cam.y + view.y * 0.5f + 120)
            continue;
        e->addLights(out, time);
    }
}

} // namespace bd
