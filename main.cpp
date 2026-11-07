// main.cpp - window, game loop, input, interaction rules, UI and save handling.
// It ties together the Renderer, Atlas, World, AudioManager and Config.
//
// Controls: WASD / arrows move, SHIFT runs, E / SPACE / ENTER interact,
//           ESC pause menu, F1 warnings report, F2 screenshot, F11 fullscreen, M mute.
#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <GLFW/glfw3.h>

#include "Atlas.h"
#include "Audio.h"
#include "Common.h"
#include "Config.h"
#include "Renderer.h"
#include "World.h"

using namespace bd;

namespace {

enum class Mode { Title, Play, Dialogue, Reveal, Paused, Ending, Sit, Cake };

// Colours of the four glow flowers, in the same order as the sprites.
const char *kFlowerNames[4] = {"red", "golden", "blue", "pink"};

// One particle of the ending show or of a world-space effect.
// kind: 0 spark, 1 trail (short-lived tail), 2 confetti, 3 comet (a spark that leaves a trail).
struct Particle {
    vec2 p{0, 0}, v{0, 0};
    float life = 0, maxLife = 1;
    float size = 3; // UI units in the ending, world pixels for world-space effects
    float rot = 0, spin = 0, phase = 0;
    vec4 col{1, 1, 1, 1};
    uint8_t kind = 0;
};

// A rocket that climbs to a point and then bursts. shape: 0 peony, 1 double ring, 2 heart, 3 willow.
struct Rocket {
    vec2 p{0, 0}, v{0, 0};
    float life = 0, maxLife = 1;
    vec4 col{1, 1, 1, 1};
    int shape = 0;
};

struct Game {
    // ---- core systems
    GLFWwindow *win = nullptr;
    Renderer r;
    Atlas atlas;
    AudioManager audio;
    World world;
    Config cfg;
    GameState st;

    // ---- loop state
    Mode mode = Mode::Title;
    float time = 0;
    int fbW = 1280, fbH = 720;
    vec2 cam{0, 0};
    bool wantQuit = false;
    bool showReport = false;
    bool fullscreen = false;
    int winX = 100, winY = 100, winW = 1280, winH = 720; // remembered windowed placement
    float saveTimer = 0;
    std::string savePath;

    // ---- input (edge-triggered keys are filled by the GLFW callback)
    bool pressed[GLFW_KEY_LAST + 1] = {};

    // ---- title / pause menu
    int menuSel = 0;
    bool hasSave = false;

    // ---- toast messages
    std::string toast;
    float toastT = 0;

    // ---- dialogue and signs
    std::string speaker;
    std::vector<std::string> pages;
    size_t page = 0;
    float shown = 0; // characters revealed on the current page
    float talkTick = 0;

    // ---- chest reveal
    int revealChest = -1;      // chest currently being opened/shown
    bool revealVisible = false;
    float revealT = 0;
    Texture photo;
    bool photoLoaded = false;

    // ---- area banner and music
    AreaId lastArea = AreaId::Forest;
    float bannerT = 0;
    bool allShardsToast = false;

    // ---- ending
    std::vector<Particle> fireworks; // sparks, trails and confetti of the finale
    std::vector<Rocket> rockets;
    float fwTimer = 0;
    float confAcc = 0; // fractional confetti waiting to be spawned
    float flash = 0;   // white screen flash that follows each burst
    float endT = 0;
    Rng rng{777};

    // ---- sitting on the throne
    vec2 sitFrom{0, 0}, sitSeat{0, 0};
    float sitClock = 0;
    bool sitLeaving = false, sitSaid = false;

    // ---- birthday cake
    float cakeT = 0;
    bool cakeBlown = false, cakeWished = false;
    std::vector<Particle> worldFx; // confetti and sparkles in world coordinates

    // =========================================================== helpers
    float ui() const {
        return std::max(1.f, std::floor(fbH / 540.f + 0.5f));
    }
    bool key(int k) const {
        return glfwGetKey(win, k) == GLFW_PRESS;
    }
    bool hit(int k) {
        bool p = pressed[k];
        pressed[k] = false;
        return p;
    }
    bool hitConfirm() {
        bool a = hit(GLFW_KEY_E), b = hit(GLFW_KEY_SPACE), c = hit(GLFW_KEY_ENTER);
        return a || b || c;
    }
    bool hitUp() {
        bool a = hit(GLFW_KEY_W), b = hit(GLFW_KEY_UP);
        return a || b;
    }
    bool hitDown() {
        bool a = hit(GLFW_KEY_S), b = hit(GLFW_KEY_DOWN);
        return a || b;
    }
    void say(const std::string &s, float secs = 3.f) {
        toast = s;
        toastT = secs;
    }
    static std::string ordinal(int n) {
        int m100 = n % 100, m10 = n % 10;
        const char *suffix = (m100 >= 11 && m100 <= 13) ? "th"
                             : m10 == 1                 ? "st"
                             : m10 == 2                 ? "nd"
                             : m10 == 3                 ? "rd"
                                                        : "th";
        return std::to_string(n) + suffix;
    }
    std::string fillName(const std::string &s) const {
        return util::replaceAll(s, "{name}", cfg.name);
    }

    // Split on explicit newlines, then wrap each piece to the given pixel width.
    std::vector<std::string> wrapText(Face f, int px, const std::string &text, float maxW) {
        std::vector<std::string> out;
        for (const std::string &part : util::splitLines(text)) {
            if (part.empty()) {
                out.push_back("");
                continue;
            }
            for (auto &l : r.wrap(f, px, part, maxW))
                out.push_back(l);
        }
        return out;
    }

    // Switch between a normal window and borderless full screen on the main monitor.
    void toggleFullscreen() {
        if (!fullscreen) {
            GLFWmonitor *m = glfwGetPrimaryMonitor();
            const GLFWvidmode *vm = m ? glfwGetVideoMode(m) : nullptr;
            if (!vm)
                return;
            glfwGetWindowPos(win, &winX, &winY);
            glfwGetWindowSize(win, &winW, &winH);
            glfwSetWindowMonitor(win, m, 0, 0, vm->width, vm->height, vm->refreshRate);
            fullscreen = true;
        } else {
            glfwSetWindowMonitor(win, nullptr, winX, winY, winW, winH, 0);
            fullscreen = false;
        }
        glfwSwapInterval(1);
    }

    // =========================================================== setup
    bool init(const char *argv0) {
        Paths::init(argv0);
        savePath = Paths::saves() + "/save.txt";
        cfg.load(Paths::file("birthday_config.json"));

        if (!glfwInit()) {
            Log::error("Could not initialise GLFW");
            return false;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
        win = glfwCreateWindow(1280, 720, cfg.title.c_str(), nullptr, nullptr);
        if (!win) {
            Log::error("Could not create a 3.3 OpenGL window");
            return false;
        }
        glfwMakeContextCurrent(win);
        glfwSwapInterval(1);
        if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
            Log::error("Could not load OpenGL functions");
            return false;
        }
        glfwSetWindowUserPointer(win, this);
        glfwSetKeyCallback(win, [](GLFWwindow *w, int k, int, int action, int) {
            Game *g = (Game *)glfwGetWindowUserPointer(w);
            if (g && k >= 0 && k <= GLFW_KEY_LAST && action == GLFW_PRESS)
                g->pressed[k] = true;
        });

        if (!r.init()) {
            Log::error("Renderer failed to start");
            return false;
        }
        if (!atlas.build()) {
            Log::error("Sprite atlas failed to build");
            return false;
        }
        r.setWhiteSource(&atlas.tex());
        world.init(atlas);
        audio.init(); // a missing audio device only means a silent game

        hasSave = util::fileExists(savePath);
        audio.music("", 0.1f);
        return true;
    }

    void shutdown() {
        if (mode == Mode::Play || mode == Mode::Dialogue || mode == Mode::Reveal)
            saveGame();
        if (photoLoaded)
            photo.destroy();
        audio.shutdown();
        r.shutdown();
        if (win)
            glfwDestroyWindow(win);
        glfwTerminate();
    }

    // =========================================================== game flow
    void startGame(bool loadSave) {
        if (loadSave && st.load(savePath)) {
            Log::info("Loaded save");
        } else {
            st.reset((uint32_t)(glfwGetTime() * 1000.0) ^ 0xB1D7u);
        }
        world.generate(cfg, st);
        Player *p = world.player();
        cam = p->pos;
        lastArea = world.areaAtPos(p->pos);
        bannerT = 4.f;
        startAreaAudio(lastArea);
        revealChest = -1;
        revealVisible = false;
        allShardsToast = (int)st.shards.size() >= 5;
        mode = Mode::Play;
        if (!loadSave || st.playTime < 1.0)
            say("Welcome, " + cfg.name + "! " + cfg.companion + " is floating beside you.", 5.f);
        else
            say("Welcome back, " + cfg.name + "!", 3.f);
    }

    void saveGame() {
        if (Player *p = world.player()) {
            // While seated on the throne, remember where she stood before sitting down.
            st.playerPos = (mode == Mode::Sit) ? sitFrom : p->pos;
            st.facing = p->facing;
        }
        if (!st.save(savePath))
            Log::warn("Could not write save file %s", savePath.c_str());
        else
            hasSave = true;
    }

    void startAreaAudio(AreaId a) {
        audio.music(std::string("music_") + areaKey(a), 2.f);
        audio.ambience(std::string("amb_") + areaKey(a), 2.f);
    }

    void openDialogue(const std::string &who, std::vector<std::string> lines) {
        speaker = who;
        pages.clear();
        for (auto &l : lines)
            pages.push_back(fillName(l));
        page = 0;
        shown = 0;
        mode = Mode::Dialogue;
        audio.sfx("paper", 0.6f);
    }

    int othersLeft() const {
        int left = 0;
        for (auto &e : world.entities())
            if (e->type == EType::Chest) {
                auto *c = static_cast<const Chest *>(e.get());
                if (!c->isFinal && !c->opened && !c->opening)
                    left++;
            }
        return left;
    }

    const ChestDef *chestDef(int id) const {
        for (auto &d : cfg.chests)
            if (d.id == id)
                return &d;
        return nullptr;
    }

    // =========================================================== interaction
    struct Target {
        Entity *e = nullptr;
        std::string prompt;
        float d = 1e9f;
    };

    Target findTarget() {
        Target best;
        Player *pl = world.player();
        if (!pl)
            return best;
        for (auto &up : world.entities()) {
            Entity *e = up.get();
            float radius;
            std::string prompt;
            if (e->type == EType::NPC) {
                radius = 28.f;
                prompt = "Talk";
            } else if (e->type == EType::Chest) {
                auto *c = static_cast<Chest *>(e);
                if (c->opened || c->opening)
                    continue;
                radius = 28.f;
                prompt = (c->isFinal && c->sealed) ? "Sealed" : "Open";
            } else if (e->type == EType::Prop) {
                auto *p = static_cast<Prop *>(e);
                if (!p->canInteract)
                    continue;
                prompt = p->prompt();
                if (prompt.empty())
                    continue;
                radius = p->interactRadius();
            } else
                continue;
            float d = glm::length(e->pos - pl->pos);
            if (d <= radius && d < best.d) {
                best.e = e;
                best.prompt = prompt;
                best.d = d;
            }
        }
        return best;
    }

    // The sign no longer names the colours. It describes them, so the player has to
    // work out which flower each line means.
    std::vector<std::string> flowerHint() const {
        static const char *clue[4] = {"the colour of a warm heart", "the colour of the afternoon sun",
                                      "the colour of the quiet lake", "the colour of a shy blush"};
        std::string a = "The garden remembers a melody.\nIt will not say the colours aloud:\n\n";
        std::string b;
        for (size_t i = 0; i < world.flowerOrder.size(); i++) {
            static const char *ord[4] = {"First", "Then", "Next", "Last"};
            std::string line = std::string(ord[std::min<size_t>(i, 3)]) + ": " +
                               clue[world.flowerOrder[i] & 3];
            if (i < 2)
                a += line + (i == 0 ? "\n" : "");
            else
                b += (b.empty() ? "" : "\n") + line;
        }
        b += "\n\nA wrong touch and every flower forgets.";
        return {a, b};
    }

    void keyName(const std::string &k, std::string &out) const {
        if (k == "forest_key")
            out = "Forest Key";
        else if (k == "garden_key")
            out = "Garden Key";
        else if (k == "golden_key")
            out = "Golden Key";
        else
            out = k;
    }

    // Mr. Pale only remembers where the hidden library passage is once the player
    // has brought him three memory shards. Until then the bookcase gives no hint.
    void ghostTalk(NPC &n) {
        n.talked = true;
        if (st.flag("library_clue")) {
            openDialogue(n.name, {"The odd bookcase, in the library, beside the closet. It sighs when "
                                  "you are near. Press E on it."});
            return;
        }
        int have = (int)st.shards.size();
        if (have < 3) {
            openDialogue(n.name,
                         {"Oooh. A visitor. I paint the memories that hang on these walls.",
                          "I know a secret about the library, but my memory is faint, faint, faint.",
                          "Bring me three memory shards and perhaps I will remember. You carry " +
                              std::to_string(have) + " so far."});
            return;
        }
        st.setFlag("library_clue");
        if (Prop *p = world.propById("secret_library"))
            p->canInteract = true;
        audio.sfx("secret");
        openDialogue(n.name, {"Three shards! They hum like old lullabies... Yes, yes, I remember now.",
                              "In the library, one bookcase in the wall is not quite like the "
                              "others: the one beside the little closet. Press E on it.",
                              "I would tell you more, but I am rather transparent."});
        saveGame();
    }

    void interact(const Target &t) {
        if (!t.e)
            return;
        if (t.e->type == EType::NPC) {
            auto *n = static_cast<NPC *>(t.e);
            if (n->id == "npc_ghost") {
                ghostTalk(*n);
                return;
            }
            openDialogue(n->name, n->talked && !n->repeatLines.empty() ? n->repeatLines : n->lines);
            n->talked = true;
            return;
        }
        if (t.e->type == EType::Chest) {
            auto *c = static_cast<Chest *>(t.e);
            if (c->isFinal && c->sealed) {
                audio.sfx("chest_locked");
                say("Sealed! " + std::to_string(othersLeft()) + " chest(s) and " +
                    std::to_string(std::max(0, 5 - (int)st.shards.size())) +
                    " memory shard(s) still to find.", 4.5f);
                return;
            }
            c->open();
            st.chestsOpened.insert(c->id);
            revealChest = c->id;
            audio.sfx("chest_open");
            if (c->isFinal)
                audio.music("", 1.f);
            saveGame();
            return;
        }
        auto *p = static_cast<Prop *>(t.e);
        switch (p->kind) {
        case PropKind::Sign: {
            if (p->text.empty() && p->id == "sign_flowers") {
                openDialogue("Sign", flowerHint());
                break;
            }
            openDialogue("Sign", {p->text});
            break;
        }
        case PropKind::Lever:
            if (!st.shards.count("shard_lake")) {
                audio.sfx("chest_locked");
                say("The lever will not budge. A small socket in it glows faintly, as if it "
                    "wants a memory shard from the island.", 5.f);
                break;
            }
            p->open = true;
            p->canInteract = false;
            st.setFlag(p->id);
            world.extendBridge();
            audio.sfx("lever");
            audio.sfx("bridge", 0.8f);
            say("A bridge slowly grows across the river...");
            saveGame();
            break;
        case PropKind::Gate:
        case PropKind::Door:
        case PropKind::Door2:
            if (!p->needKey.empty()) {
                if (!st.keys.count(p->needKey)) {
                    std::string nm;
                    keyName(p->needKey, nm);
                    audio.sfx("chest_locked");
                    say("Locked. You need the " + nm + ".");
                    break;
                }
                st.keys.erase(p->needKey);
                audio.sfx("door_unlock");
            }
            st.setFlag(p->id);
            world.openGate(*p, false);
            audio.sfx("door_open");
            saveGame();
            break;
        case PropKind::Secret:
            st.setFlag(p->id);
            world.openGate(*p, false);
            audio.sfx("secret");
            say("You found a hidden passage!");
            if (p->id == "secret_forest_key" && !st.keys.count("forest_key"))
                world.addCollectible(ItemKind::KeyForest, "forest_key", 10, 65);
            saveGame();
            break;
        case PropKind::Throne:
            beginSit(*p);
            break;
        case PropKind::Deco:
            if (p->sprite == "cake" && p->blowT <= 0)
                beginCake(*p);
            break;
        case PropKind::GlowFlower:
            touchFlower(*p);
            break;
        default:
            break;
        }
    }

    void touchFlower(Prop &p) {
        if (st.flag("garden_solved"))
            return;
        if (st.flowerProgress >= 0 && st.flowerProgress < (int)world.flowerOrder.size() &&
            p.index == world.flowerOrder[st.flowerProgress]) {
            p.lit = true;
            st.flowerProgress++;
            audio.sfx("flower_" + std::to_string(p.index & 3));
            if (st.flowerProgress >= 4) {
                st.setFlag("garden_solved");
                audio.sfx("flower_good");
                world.addCollectible(ItemKind::KeyGarden, "garden_key", 45, 36);
                say("The fountain shimmers. A key rises from the water!", 5.f);
                saveGame();
            }
        } else {
            audio.sfx("flower_bad");
            st.flowerProgress = 0;
            for (auto &e : world.entities())
                if (e->type == EType::Prop) {
                    auto *q = static_cast<Prop *>(e.get());
                    if (q->kind == PropKind::GlowFlower)
                        q->lit = false;
                }
            say("The flowers fade. Try the order again.");
        }
    }

    void collectNearby() {
        Player *pl = world.player();
        for (auto &up : world.entities()) {
            if (up->type != EType::Collectible || up->removed)
                continue;
            auto *c = static_cast<Collectible *>(up.get());
            if (glm::length(c->pos - pl->pos) > 16.f)
                continue;
            c->removed = true;
            if (c->kind == ItemKind::Shard) {
                st.shards.insert(c->id);
                audio.sfx("shard");
                say("Memory shard found! (" + std::to_string(st.shards.size()) + "/5)");
                if (st.shards.size() >= 5 && !allShardsToast) {
                    allShardsToast = true;
                    audio.sfx("achievement");
                    say("All five memory shards gathered!", 5.f);
                }
            } else {
                st.keys.insert(c->id);
                audio.sfx("key");
                std::string nm;
                keyName(c->id, nm);
                say("You found the " + nm + "!");
            }
            saveGame();
        }
    }

    // =========================================================== update
    const char *stepSound(vec2 p) {
        switch (world.ground((int)std::floor(p.x / TILE), (int)std::floor((p.y - 2) / TILE))) {
        case G_STONE:
        case G_TILE:
        case G_PLAZA:
        case G_CARPET:
            return "step_stone";
        case G_WOOD:
        case G_BRIDGE_H:
        case G_BRIDGE_V:
            return "step_wood";
        case G_SAND:
            return "step_sand";
        case G_WATER:
        case G_GLOWSTONE:
            return "step_water";
        default:
            return "step_grass";
        }
    }

    void updatePlay(float dt) {
        Player *pl = world.player();
        vec2 dir(0, 0);
        if (key(GLFW_KEY_A) || key(GLFW_KEY_LEFT))
            dir.x -= 1;
        if (key(GLFW_KEY_D) || key(GLFW_KEY_RIGHT))
            dir.x += 1;
        if (key(GLFW_KEY_W) || key(GLFW_KEY_UP))
            dir.y -= 1;
        if (key(GLFW_KEY_S) || key(GLFW_KEY_DOWN))
            dir.y += 1;
        bool sprint = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
        pl->move(dt, dir, sprint, world);
        if (pl->stepEvent) {
            pl->stepEvent = 0;
            audio.sfx(stepSound(pl->pos), 0.45f, 0.95f + rng.f() * 0.1f);
        }
        world.pip()->follow(dt, *pl);
        world.update(dt);
        collectNearby();

        // The golden chest stays sealed until every other chest has been opened.
        int left = othersLeft();
        for (auto &e : world.entities())
            if (e->type == EType::Chest) {
                auto *c = static_cast<Chest *>(e.get());
                if (c->isFinal)
                    c->sealed = left > 0 || st.shards.size() < 5;
            }

        // A chest that finished its opening animation shows its message.
        if (revealChest >= 0) {
            Chest *c = world.chestById(revealChest);
            if (c && c->opened) {
                revealVisible = true;
                revealT = 0;
                mode = Mode::Reveal;
                loadPhoto(revealChest);
                audio.sfx("magic", 0.7f);
            }
        }

        // Area change: banner and music.
        AreaId a = world.areaAtPos(pl->pos);
        if (a != lastArea) {
            lastArea = a;
            bannerT = 3.5f;
            startAreaAudio(a);
        }

        Target t = findTarget();
        if (t.e && hitConfirm())
            interact(t);

        st.playTime += dt;
        saveTimer += dt;
        if (saveTimer > 30.f) {
            saveTimer = 0;
            saveGame();
        }
        if (hit(GLFW_KEY_ESCAPE)) {
            mode = Mode::Paused;
            menuSel = 0;
            audio.sfx("ui_click");
        }
    }

    void updateDialogue(float dt) {
        world.update(dt); // keep the world alive behind the box
        const std::string &text = pages[page];
        float total = (float)util::utf8Length(text);
        float before = shown;
        shown = std::min(total, shown + dt * 45.f);
        talkTick += shown - before;
        if (talkTick >= 2.f && shown < total) {
            talkTick = 0;
            audio.sfx("talk", 0.35f);
        }
        bool esc = hit(GLFW_KEY_ESCAPE);
        bool ok = hitConfirm();
        if (esc) { // Esc skips the rest of the conversation
            mode = Mode::Play;
            audio.sfx("ui_back", 0.5f);
        } else if (ok) {
            if (shown < total) {
                shown = total; // first press finishes typing
            } else if (page + 1 < pages.size()) {
                page++;
                shown = 0;
                audio.sfx("ui_move", 0.5f);
            } else {
                mode = Mode::Play;
                audio.sfx("ui_back", 0.5f);
            }
        }
    }

    void loadPhoto(int chestId) {
        if (photoLoaded) {
            photo.destroy();
            photoLoaded = false;
        }
        const ChestDef *d = chestDef(chestId);
        if (!d || d->photo.empty())
            return;
        std::string path = Paths::file(d->photo);
        if (util::fileExists(path) && loadTextureFile(path, photo)) {
            photoLoaded = true;
        } else {
            Log::warn("Photo '%s' for chest %d could not be loaded - showing a placeholder",
                      d->photo.c_str(), chestId);
            makePlaceholderTexture(photo, 256, 192);
            photoLoaded = photo.valid();
        }
    }

    void updateReveal(float dt) {
        world.update(dt);
        revealT += dt;
        if (revealT > 0.6f && hitConfirm()) {
            const ChestDef *d = chestDef(revealChest);
            bool fin = d && d->final;
            revealVisible = false;
            revealChest = -1;
            if (photoLoaded) {
                photo.destroy();
                photoLoaded = false;
            }
            audio.sfx("ui_back", 0.6f);
            if (fin) {
                startEnding();
            } else {
                mode = Mode::Play;
                if (othersLeft() == 0 && st.shards.size() >= 5)
                    say("Every memory is found. The golden chest awakens!", 5.f);
                else if (othersLeft() == 0)
                    say("The golden chest still sleeps... some memory shards are missing.", 5.f);
            }
        }
    }

    // =========================================================== throne: sitting
    // The player hops onto the seat, settles in (a crown drops onto her head and sparkles
    // drift up), and hops back to where she stood when the player asks to get up.
    void beginSit(Prop &p) {
        Player *pl = world.player();
        sitFrom = pl->pos;
        sitSeat = p.pos + vec2(0, 1.f); // just in front of the throne so she draws over it
        sitClock = 0;
        sitLeaving = false;
        sitSaid = false;
        pl->vel = vec2(0);
        pl->moving = false;
        pl->facing = 0;
        pl->walkT = 0;
        pl->sitK = 0.001f;
        pl->sitT = 0;
        mode = Mode::Sit;
        audio.sfx("sit");
    }

    void updateSit(float dt) {
        Player *pl = world.player();
        world.update(dt);
        world.pip()->follow(dt, *pl);
        sitClock += dt;
        const float hopTime = 0.5f;
        if (!sitLeaving) {
            float u = clamp01(sitClock / hopTime);
            float e = easeInOut(u);
            pl->pos = sitFrom + (sitSeat - sitFrom) * e;
            pl->hop = std::sin(u * PI) * 9.f;
            pl->sitK = std::max(0.001f, e);
            if (u >= 1.f) {
                if (!sitSaid) {
                    sitSaid = true;
                    audio.sfx("magic", 0.45f, 1.25f);
                    say("You sit on the throne. Very comfortable, Your Majesty.", 3.5f);
                }
                pl->hop = 0;
                pl->sitT += dt;
                if (pl->sitT > 3.6f && pl->sitT < 3.6f + dt + 0.001f)
                    say("Long live Queen " + cfg.name + "!", 3.f);
            }
            // any confirm / movement / escape press gets her up (after she has landed)
            bool up = hitConfirm();
            up = hit(GLFW_KEY_ESCAPE) || up;
            up = hitUp() || up;
            up = hitDown() || up;
            up = hit(GLFW_KEY_A) || up;
            up = hit(GLFW_KEY_D) || up;
            up = hit(GLFW_KEY_LEFT) || up;
            up = hit(GLFW_KEY_RIGHT) || up;
            if (up && sitClock > hopTime + 0.3f) {
                sitLeaving = true;
                sitClock = 0;
                audio.sfx("ui_back", 0.5f);
            }
        } else {
            float u = clamp01(sitClock / 0.4f);
            float e = easeInOut(u);
            pl->pos = sitSeat + (sitFrom - sitSeat) * e;
            pl->hop = std::sin(u * PI) * 9.f;
            pl->sitK = std::max(0.001f, 1.f - e);
            if (u >= 1.f) {
                pl->pos = sitFrom;
                pl->hop = 0;
                pl->sitK = 0;
                pl->sitT = 0;
                toastT = 0;
                mode = Mode::Play;
            }
        }
    }

    // =========================================================== cake: blow out the candles
    Prop *cake() {
        return world.propById("cake");
    }

    void beginCake(Prop &) {
        Player *pl = world.player();
        pl->vel = vec2(0);
        pl->moving = false;
        pl->facing = 1; // face the cake
        pl->walkT = 0;
        cakeT = 0;
        cakeBlown = false;
        cakeWished = false;
        mode = Mode::Cake;
        audio.sfx("ui_move", 0.6f, 0.6f); // a deep breath in
    }

    void cakeConfetti(vec2 origin, int n) {
        for (int i = 0; i < n; i++) {
            Particle q;
            q.p = origin + vec2(rng.range(-14.f, 14.f), rng.range(-4.f, 4.f));
            float a = -rng.range(0.3f, PI - 0.3f); // upward fan
            q.v = vec2(std::cos(a), std::sin(a)) * rng.range(50.f, 170.f);
            q.maxLife = rng.range(1.8f, 3.2f);
            q.size = rng.range(1.0f, 2.0f);
            q.rot = rng.range(0, 6.28f);
            q.spin = rng.range(-9.f, 9.f);
            q.phase = rng.range(0, 6.28f);
            q.col = festiveColor();
            q.kind = 2;
            worldFx.push_back(q);
        }
        for (int i = 0; i < n / 2; i++) { // golden glitter
            Particle q;
            q.p = origin;
            float a = rng.range(0, 2 * PI);
            q.v = vec2(std::cos(a), std::sin(a)) * rng.range(20.f, 100.f);
            q.maxLife = rng.range(0.8f, 1.6f);
            q.size = rng.range(1.5f, 2.6f);
            q.phase = rng.range(0, 6.28f);
            q.col = rgba(rng.chance(0.5f) ? 0xffd166 : 0xfff3c4);
            q.kind = 0;
            worldFx.push_back(q);
        }
    }

    void updateCake(float dt) {
        Player *pl = world.player();
        Prop *c = cake();
        world.update(dt);
        world.pip()->follow(dt, *pl);
        cakeT += dt;
        if (!c) {
            mode = Mode::Play;
            return;
        }
        if (!cakeBlown && cakeT >= 0.7f) { // the breath: candles go out left to right
            cakeBlown = true;
            c->blowT = 0.001f;
            audio.sfx("secret", 0.7f, 1.9f);
        }
        if (cakeBlown && !cakeWished && cakeT >= 1.9f) { // the wish: confetti and chimes
            cakeWished = true;
            cakeConfetti(c->pos + vec2(0, -52), 150);
            audio.sfx("magic", 0.8f);
            audio.sfx("achievement", 0.5f, 1.1f);
        }
        bool skip = cakeT > 2.6f && hitConfirm();
        if (cakeT >= 4.6f || skip) {
            mode = Mode::Play;
            say("Make it a good one. The magic candles will relight themselves...", 4.f);
        }
    }

    // World-space confetti and sparkles (cake). Advanced every frame in every mode.
    void updateWorldFx(float dt) {
        for (auto &f : worldFx) {
            f.life += dt;
            if (f.kind == 2) {
                f.v = f.v * (1.f - 1.5f * dt) + vec2(0, 110.f * dt);
                f.p += f.v * dt;
                f.p.x += std::sin(f.life * 4.f + f.phase) * 10.f * dt;
                f.rot += f.spin * dt;
            } else {
                f.v = f.v * (1.f - 1.4f * dt) + vec2(0, 30.f * dt);
                f.p += f.v * dt;
            }
        }
        worldFx.erase(std::remove_if(worldFx.begin(), worldFx.end(),
                                     [](const Particle &f) { return f.life >= f.maxLife; }),
                      worldFx.end());
    }

    void drawWorldFx() {
        if (worldFx.empty())
            return;
        float zoom = r.zoom();
        vec2 c = r.camCenter();
        vec2 half(fbW * 0.5f, fbH * 0.5f);
        r.setBlend(Blend::Alpha);
        for (auto &f : worldFx) {
            if (f.kind != 2)
                continue;
            vec2 s = (f.p - c) * zoom + half;
            float fade = clamp01((f.maxLife - f.life) * 2.5f);
            float sw = f.size * zoom * 1.6f;
            float sh = f.size * zoom * (0.3f + 0.7f * std::abs(std::cos(f.rot)));
            r.rotRect(s.x, s.y, sw, sh, f.rot * 0.4f, withAlpha(f.col, fade));
        }
        r.setBlend(Blend::Additive);
        for (auto &f : worldFx) {
            if (f.kind == 2)
                continue;
            vec2 s = (f.p - c) * zoom + half;
            float t = f.life / f.maxLife;
            float tw = 0.6f + 0.4f * std::sin(f.phase + f.life * 30.f);
            float sz = f.size * zoom * (1.5f + 1.5f * tw);
            r.draw(atlas.tex(), atlas.get("p_star"), s.x - sz / 2, s.y - sz / 2, sz, sz,
                   withAlpha(f.col, (1.f - t) * tw));
        }
        r.setBlend(Blend::Alpha);
    }

    void drawCakeOverlay() {
        if (!cakeBlown)
            return;
        float u = ui();
        float t = cakeT - 0.9f;
        float a = clamp01(t * 1.5f) * clamp01((4.6f - cakeT) * 1.5f);
        if (a <= 0)
            return;
        float rise = (1.f - easeOutCubic(t * 0.8f)) * 14 * u;
        r.textShadow(Face::SerifItalic, (int)(32 * u), "Make a wish, " + cfg.name + "...", fbW * 0.5f,
                     fbH * 0.2f + rise, rgba(0xfff3c4, a), Align::Center);
    }

    // =========================================================== the finale
    vec4 festiveColor() {
        static const uint32_t pal[] = {0xffd166, 0xff6b9d, 0x6bd5ff, 0xb78bff,
                                       0x7aeaa0, 0xff8a5c, 0xfff3c4};
        return rgba(pal[rng.i(0, 6)]);
    }

    static Particle makeTrail(vec2 p, vec2 v, vec4 c, float size, float life) {
        Particle t;
        t.p = p;
        t.v = v;
        t.maxLife = life;
        t.size = size;
        t.col = c;
        t.kind = 1;
        return t;
    }

    void startEnding() {
        st.setFlag("finished");
        saveGame();
        mode = Mode::Ending;
        endT = 0;
        fireworks.clear();
        rockets.clear();
        fwTimer = 0.6f;
        confAcc = 0;
        flash = 0;
        audio.music("music_birthday", 1.5f);
        confettiCannons();
        for (int i = 0; i < 3; i++) // opening salvo
            spawnRocket();
    }

    // Two confetti cannons in the bottom corners.
    void confettiCannons() {
        float u = ui();
        for (int side = 0; side < 2; side++)
            for (int i = 0; i < 120; i++) {
                float a = -rng.range(0.55f, 1.35f);
                float sp = rng.range(300.f, 720.f) * u;
                vec2 v(std::cos(a) * sp, std::sin(a) * sp);
                if (side)
                    v.x = -v.x;
                spawnConfetti(vec2(side ? fbW - 10 * u : 10 * u, fbH - 10 * u), v);
            }
    }

    void spawnConfetti(vec2 p, vec2 v) {
        Particle q;
        q.p = p;
        q.v = v;
        q.maxLife = 30.f;
        q.size = rng.range(2.5f, 4.5f);
        q.rot = rng.range(0, 6.28f);
        q.spin = rng.range(-8.f, 8.f);
        q.phase = rng.range(0, 6.28f);
        q.col = festiveColor();
        q.kind = 2;
        fireworks.push_back(q);
    }

    void spawnRocket() {
        float u = ui();
        Rocket k;
        vec2 target((0.12f + rng.f() * 0.76f) * fbW, (0.10f + rng.f() * 0.38f) * fbH);
        k.maxLife = rng.range(0.7f, 1.05f);
        k.p = vec2(target.x + rng.range(-50.f, 50.f) * u, (float)fbH + 8 * u);
        k.v = (target - k.p) / k.maxLife;
        k.col = festiveColor();
        float q = rng.f();
        k.shape = q < 0.42f ? 0 : (q < 0.67f ? 1 : (q < 0.87f ? 2 : 3));
        rockets.push_back(k);
        audio.sfx("fw_launch", 0.35f, 0.9f + rng.f() * 0.2f);
    }

    void spawnBurst(vec2 c, vec4 col, int shape) {
        float u = ui();
        vec4 col2 = festiveColor();
        if (shape == 2)
            col = rgba(0xff6b9d);
        if (shape == 3)
            col = rgba(0xffc860);
        int n = 120 + rng.i(0, 50);
        for (int i = 0; i < n; i++) {
            Particle q;
            q.p = c;
            float a = rng.range(0, 2 * PI), sp = rng.range(40.f, 190.f) * u;
            vec2 vel(std::cos(a) * sp, std::sin(a) * sp);
            q.col = col;
            q.maxLife = rng.range(1.0f, 1.8f);
            if (shape == 1) { // double ring
                a = i / (float)n * 2 * PI;
                sp = ((i & 1) ? 165.f : 100.f) * u;
                vel = vec2(std::cos(a) * sp, std::sin(a) * sp);
                if (i & 1)
                    q.col = col2;
            } else if (shape == 2) { // heart
                float t = i / (float)n * 2 * PI, s3 = std::sin(t);
                vec2 h(16 * s3 * s3 * s3, -(13 * std::cos(t) - 5 * std::cos(2 * t) -
                                            2 * std::cos(3 * t) - std::cos(4 * t)));
                vel = h * (9.5f * u * (0.93f + 0.07f * rng.f()));
            } else if (shape == 3) { // willow: slow, long-lived golden rain
                sp = rng.range(50.f, 140.f) * u;
                vel = vec2(std::cos(a) * sp, std::sin(a) * sp);
                q.maxLife = rng.range(2.0f, 3.1f);
            }
            if (rng.chance(0.12f))
                q.col = rgba(0xffffff);
            q.v = vel;
            q.size = rng.range(2.2f, 4.2f);
            q.phase = rng.range(0, 6.28f);
            q.kind = rng.chance(shape == 3 ? 0.6f : 0.35f) ? 3 : 0;
            fireworks.push_back(q);
        }
        flash = 0.4f;
        audio.sfx("fw_boom", 0.5f, 0.9f + rng.f() * 0.3f);
    }

    void updateParticles(float dt) {
        float u = ui();
        std::vector<Particle> born;
        for (auto &f : fireworks) {
            f.life += dt;
            if (f.kind == 0 || f.kind == 3) {
                f.p += f.v * dt;
                f.v = f.v * (1.f - 1.1f * dt) + vec2(0, 70.f * u * dt);
                if (f.kind == 3 && rng.chance(0.5f))
                    born.push_back(makeTrail(f.p, vec2(rng.range(-6.f, 6.f), rng.range(0.f, 10.f)) * u,
                                             withAlpha(f.col, 0.8f), f.size * 0.6f,
                                             rng.range(0.3f, 0.5f)));
            } else if (f.kind == 1) {
                f.p += f.v * dt;
            } else { // confetti: drag, gentle fall, sway and tumble
                f.v = f.v * (1.f - 1.7f * dt) + vec2(0, 95.f * u * dt);
                f.p.x += (f.v.x + std::sin(f.life * 3.f + f.phase) * 28.f * u) * dt;
                f.p.y += f.v.y * dt;
                f.rot += f.spin * dt;
            }
        }
        if (fireworks.size() < 9000)
            fireworks.insert(fireworks.end(), born.begin(), born.end());
        float floorY = fbH + 30 * u;
        fireworks.erase(std::remove_if(fireworks.begin(), fireworks.end(),
                                       [&](const Particle &f) {
                                           return f.life >= f.maxLife || f.p.y > floorY;
                                       }),
                        fireworks.end());
    }

    void updateEnding(float dt) {
        float u = ui();
        world.update(dt);
        endT += dt;

        // rockets: launch regularly, sometimes two at once
        fwTimer -= dt;
        if (fwTimer <= 0) {
            fwTimer = rng.range(0.25f, 0.6f);
            spawnRocket();
            if (rng.chance(0.3f))
                spawnRocket();
        }
        for (auto &k : rockets) {
            k.life += dt;
            k.p += k.v * dt;
            for (int i = 0; i < 2; i++)
                fireworks.push_back(makeTrail(k.p + vec2(rng.range(-1.5f, 1.5f), 0) * u,
                                              vec2(rng.range(-6.f, 6.f), rng.range(8.f, 20.f)) * u,
                                              rgba(0xffd9a0, 0.9f), 2.f, rng.range(0.3f, 0.6f)));
        }
        for (size_t i = 0; i < rockets.size();) {
            if (rockets[i].life >= rockets[i].maxLife) {
                Rocket k = rockets[i];
                rockets.erase(rockets.begin() + (long)i);
                spawnBurst(k.p, k.col, k.shape);
            } else
                i++;
        }

        // a steady rain of confetti from the top
        confAcc += dt * 60.f;
        while (confAcc >= 1.f) {
            confAcc -= 1.f;
            spawnConfetti(vec2(rng.f() * fbW, -8 * u),
                          vec2(rng.range(-20.f, 20.f), rng.range(30.f, 70.f)) * u);
        }
        flash = std::max(0.f, flash - dt * 1.6f);
        updateParticles(dt);

        if (endT > 2.f && hitConfirm()) {
            mode = Mode::Play;
            startAreaAudio(lastArea);
            say("Keep exploring as long as you like!");
        }
    }

    void menuNav(int count) {
        if (hitUp()) {
            menuSel = (menuSel + count - 1) % count;
            audio.sfx("ui_move", 0.5f);
        }
        if (hitDown()) {
            menuSel = (menuSel + 1) % count;
            audio.sfx("ui_move", 0.5f);
        }
    }

    void updateTitle(float) {
        int count = hasSave ? 3 : 2;
        menuNav(count);
        if (hit(GLFW_KEY_F1))
            showReport = !showReport;
        if (hitConfirm()) {
            audio.sfx("ui_click");
            int sel = menuSel;
            if (!hasSave)
                sel += 1; // skip "Continue"
            if (sel == 0)
                startGame(true);
            else if (sel == 1)
                startGame(false);
            else
                wantQuit = true;
        }
    }

    void updatePaused(float) {
        menuNav(4);
        if (hit(GLFW_KEY_ESCAPE)) {
            mode = Mode::Play;
            audio.sfx("ui_back");
            return;
        }
        if (hitConfirm()) {
            audio.sfx("ui_click");
            if (menuSel == 0)
                mode = Mode::Play;
            else if (menuSel == 1) {
                saveGame();
                say("Game saved.");
                mode = Mode::Play;
            } else if (menuSel == 2)
                audio.setEnabled(!audio.enabled());
            else {
                saveGame();
                hasSave = true;
                audio.music("", 1.f);
                audio.ambience("", 1.f);
                mode = Mode::Title;
                menuSel = 0;
            }
        }
    }

    void update(float dt) {
        time += dt;
        if (toastT > 0)
            toastT -= dt;
        if (bannerT > 0)
            bannerT -= dt;
        audio.update();
        if (hit(GLFW_KEY_M))
            audio.setEnabled(!audio.enabled());
        if (hit(GLFW_KEY_F11))
            toggleFullscreen();
        if (hit(GLFW_KEY_F1) && mode != Mode::Title)
            showReport = !showReport;
        if (mode == Mode::Title)
            worldFx.clear(); // a new world may be generated; drop stale effects
        else {
            updateWorldFx(dt);
            if (Prop *c = cake(); c && c->relit) { // the magic candles light themselves again
                c->relit = false;
                audio.sfx("shard", 0.6f, 1.4f);
                for (int i = 0; i < 5; i++)
                    cakeConfetti(vec2(c->pos.x - 32 + 21 + i * 5 - 3 + 4, c->pos.y - 56 + 2), 4);
                say("The magic candles relit themselves!", 3.f);
            }
        }
        switch (mode) {
        case Mode::Title:
            updateTitle(dt);
            break;
        case Mode::Play:
            updatePlay(dt);
            break;
        case Mode::Dialogue:
            updateDialogue(dt);
            break;
        case Mode::Reveal:
            updateReveal(dt);
            break;
        case Mode::Paused:
            updatePaused(dt);
            break;
        case Mode::Ending:
            updateEnding(dt);
            break;
        case Mode::Sit:
            updateSit(dt);
            break;
        case Mode::Cake:
            updateCake(dt);
            break;
        }
        if (world.player() && mode != Mode::Title) {
            // Smooth camera, snapped to the pixel grid so sprites do not shimmer.
            vec2 target = world.player()->pos + vec2(0, -10);
            cam += (target - cam) * expSmooth(8.f, dt);
        }
    }

    // =========================================================== drawing
    vec3 ambientFor(AreaId a) const {
        switch (a) {
        case AreaId::Forest:
            return vec3(0.30f, 0.34f, 0.52f);
        case AreaId::Lake:
            return vec3(0.30f, 0.36f, 0.58f);
        case AreaId::Garden:
            return vec3(0.38f, 0.32f, 0.58f);
        case AreaId::Castle:
            return vec3(0.48f, 0.42f, 0.52f);
        case AreaId::Final:
            return vec3(0.42f, 0.42f, 0.62f);
        }
        return vec3(0.4f);
    }

    void panel(float x, float y, float w, float h, float alpha = 0.92f) {
        r.rect(x, y, w, h, rgba(0x14102a, alpha));
        r.rectOutline(x, y, w, h, std::max(1.f, ui()), rgba(0xe9d8a6, 0.9f));
    }

    void drawWorldView() {
        float zoom = std::max(1.f, std::floor(fbH / 220.f));
        vec2 c(std::floor(cam.x * zoom) / zoom, std::floor(cam.y * zoom) / zoom);
        r.setWorld(c, zoom);
        vec2 pp = world.player() ? world.player()->pos : c;
        world.draw(r, atlas, c, zoom, time, pp);
        r.flush();

        std::vector<Light> lights;
        world.collectLights(atlas, c, r.viewSize(), time, pp, lights);
        r.lightBegin(ambientFor(world.areaAtPos(pp)));
        const Spr &g = atlas.spr(atlas.glow());
        for (const Light &l : lights)
            r.draw(atlas.tex(), g, l.pos.x - l.radius, l.pos.y - l.radius, l.radius * 2,
                   l.radius * 2, l.color);
        r.lightEnd(); // switches back to the screen projection
    }

    void drawHud() {
        float u = ui();
        int px = (int)(15 * u), small = (int)(12 * u);
        // Collection counters.
        float x = 12 * u, y = 10 * u;
        int chestsTotal = 0, chestsOpen = 0;
        for (auto &d : cfg.chests) {
            chestsTotal++;
            if (st.chestsOpened.count(d.id))
                chestsOpen++;
        }
        std::string line = "Shards " + std::to_string(st.shards.size()) + "/5   Chests " +
                           std::to_string(chestsOpen) + "/" + std::to_string(chestsTotal);
        float w = r.textWidth(Face::Sans, px, line) + 20 * u;
        panel(x, y, w, 28 * u, 0.7f);
        r.textShadow(Face::Sans, px, line, x + 10 * u, y + 5 * u, rgba(0xfff3c4));
        float ky = y + 34 * u;
        for (auto &k : st.keys) {
            std::string nm;
            keyName(k, nm);
            r.textShadow(Face::Sans, small, "Key: " + nm, x + 2 * u, ky, rgba(0xffe28a));
            ky += 16 * u;
        }
        if (!audio.enabled())
            r.textShadow(Face::Sans, small, "Sound off (M)", fbW - 12 * u, 12 * u,
                         rgba(0xffffff, 0.7f), Align::Right);

        // Area banner.
        if (bannerT > 0) {
            float a = clamp01(bannerT); // fades out over the last second
            r.textShadow(Face::SerifBold, (int)(30 * u), areaTitle(lastArea), fbW * 0.5f, fbH * 0.14f,
                         rgba(0xfff3c4, a), Align::Center);
        }

        // Interaction prompt above the nearest target.
        if (mode == Mode::Play) {
            Target t = findTarget();
            if (t.e) {
                float zoom = r.zoom();
                vec2 sc = ((t.e->pos + vec2(0, -34)) - (r.camCenter())) * zoom +
                          vec2(fbW * 0.5f, fbH * 0.5f);
                std::string s = "[E] " + t.prompt;
                float tw = r.textWidth(Face::SansBold, px, s) + 16 * u;
                panel(sc.x - tw / 2, sc.y - 12 * u, tw, 26 * u, 0.85f);
                r.textShadow(Face::SansBold, px, s, sc.x, sc.y - 7 * u, rgba(0xffffff),
                             Align::Center);
            }
        }

        // Throne: how to get up again.
        if (mode == Mode::Sit && !sitLeaving && sitClock > 0.8f) {
            std::string s = "[E] Stand up";
            float tw = r.textWidth(Face::SansBold, px, s) + 20 * u;
            float pulse = 0.8f + 0.2f * std::sin(time * 4.f);
            panel(fbW * 0.5f - tw / 2, fbH * 0.80f, tw, 28 * u, 0.85f * pulse);
            r.textShadow(Face::SansBold, px, s, fbW * 0.5f, fbH * 0.80f + 6 * u, rgba(0xffffff),
                         Align::Center);
        }

        // Toast.
        if (toastT > 0) {
            float a = clamp01(toastT * 2.f);
            auto lines = wrapText(Face::Sans, px, toast, std::min(fbW - 40 * u, 640 * u));
            float h = lines.size() * r.lineHeight(Face::Sans, px) + 16 * u;
            float wd = 0;
            for (auto &l : lines)
                wd = std::max(wd, r.textWidth(Face::Sans, px, l));
            wd += 28 * u;
            float bx = (fbW - wd) / 2, by = fbH - h - 24 * u;
            r.rect(bx, by, wd, h, rgba(0x14102a, 0.85f * a));
            float ty = by + 8 * u;
            for (auto &l : lines) {
                r.text(Face::Sans, px, l, fbW * 0.5f, ty, rgba(0xfff3c4, a), Align::Center);
                ty += r.lineHeight(Face::Sans, px);
            }
        }
    }

    void drawDialogue() {
        float u = ui();
        int px = (int)(17 * u);
        float w = std::min(fbW - 40 * u, 780 * u), h = 150 * u;
        float x = (fbW - w) / 2, y = fbH - h - 24 * u;
        panel(x, y, w, h);
        bool isSign = speaker == "Sign";
        if (!isSign) {
            float nw = r.textWidth(Face::SerifBold, (int)(18 * u), speaker) + 28 * u;
            panel(x + 16 * u, y - 16 * u, nw, 30 * u, 1.f);
            r.text(Face::SerifBold, (int)(18 * u), speaker, x + 30 * u, y - 13 * u, rgba(0xffe28a));
        }
        auto lines = wrapText(Face::Sans, px, pages[page], w - 40 * u);
        int remaining = (int)shown;
        float ty = y + 20 * u;
        for (auto &l : lines) {
            int n = (int)util::utf8Length(l);
            r.text(Face::Sans, px, l, x + 20 * u, ty, rgba(0xffffff), Align::Left,
                   std::max(0, std::min(n, remaining)));
            remaining -= n;
            ty += r.lineHeight(Face::Sans, px);
        }
        if (shown >= (float)util::utf8Length(pages[page])) {
            float bob = std::sin(time * 6.f) * 2.f * u;
            r.text(Face::Sans, (int)(14 * u), page + 1 < pages.size() ? "E  >" : "E  close",
                   x + w - 18 * u, y + h - 26 * u + bob, rgba(0xe9d8a6), Align::Right);
        }
    }

    void drawReveal() {
        float u = ui();
        r.rect(0, 0, (float)fbW, (float)fbH, rgba(0x05030f, 0.6f * easeOutCubic(revealT * 2.f)));
        const ChestDef *d = chestDef(revealChest);
        float k = easeOutBack(revealT * 1.5f);
        float w = std::min(fbW - 40 * u, 720 * u), h = std::min(fbH - 40 * u, 440 * u);
        float x = (fbW - w) / 2, y = (fbH - h) / 2 + (1.f - k) * 30 * u;
        panel(x, y, w, h, 0.97f);
        std::string title = d ? d->title : "A Memory";
        r.textShadow(Face::SerifBold, (int)(26 * u), title, fbW * 0.5f, y + 16 * u, rgba(0xffe28a),
                     Align::Center);
        float cy = y + 60 * u;
        float textW = w - 40 * u;
        if (photoLoaded && photo.valid()) {
            float maxW = w * 0.45f, maxH = h - 100 * u;
            float s = std::min(maxW / photo.w, maxH / photo.h);
            float pw = photo.w * s, ph = photo.h * s;
            r.rect(x + 18 * u, cy - 2 * u, pw + 4 * u, ph + 4 * u, rgba(0xe9d8a6));
            r.image(photo, x + 20 * u, cy, pw, ph);
            textW = w - pw - 60 * u;
            int px = (int)(17 * u);
            float tx = x + pw + 40 * u;
            for (auto &l : wrapText(Face::Serif, px, d ? fillName(d->message) : "", textW)) {
                r.text(Face::Serif, px, l, tx, cy, rgba(0xffffff));
                cy += r.lineHeight(Face::Serif, px);
            }
        } else {
            int px = (int)(18 * u);
            for (auto &l : wrapText(Face::Serif, px, d ? fillName(d->message) : "", textW)) {
                r.text(Face::Serif, px, l, fbW * 0.5f, cy, rgba(0xffffff), Align::Center);
                cy += r.lineHeight(Face::Serif, px);
            }
        }
        if (revealT > 0.6f)
            r.text(Face::Sans, (int)(14 * u), "E  continue", x + w - 18 * u,
                   y + h - 28 * u + std::sin(time * 6.f) * 2.f * u, rgba(0xe9d8a6), Align::Right);
    }

    void drawEnding() {
        float u = ui();
        r.rect(0, 0, (float)fbW, (float)fbH, rgba(0x05030f, 0.35f));
        const Spr &gl = atlas.spr(atlas.glow());
        r.setBlend(Blend::Additive);
        if (flash > 0)
            r.rect(0, 0, (float)fbW, (float)fbH, vec4(1.f, 0.95f, 0.85f, flash * 0.25f));
        for (auto &k : rockets) {
            r.draw(atlas.tex(), gl, k.p.x - 9 * u, k.p.y - 9 * u, 18 * u, 18 * u,
                   vec4(1.f, 0.85f, 0.5f, 0.6f));
            r.rect(std::floor(k.p.x), std::floor(k.p.y), 3 * u, 3 * u, rgba(0xfff3c4));
        }
        for (auto &f : fireworks) {
            if (f.kind == 2)
                continue;
            float t = f.life / f.maxLife, a = 1.f - t, sz = f.size * u;
            if (f.kind == 1) { // trail: shrinks as it fades
                float s = sz * (1.f - 0.6f * t);
                r.rect(std::floor(f.p.x), std::floor(f.p.y), s, s, withAlpha(f.col, a * 0.8f));
                continue;
            }
            float tw = 0.65f + 0.35f * std::sin(f.phase + f.life * 40.f); // twinkle
            r.draw(atlas.tex(), gl, f.p.x - sz * 2.5f, f.p.y - sz * 2.5f, sz * 5, sz * 5,
                   withAlpha(f.col, a * 0.3f * tw));
            r.rect(std::floor(f.p.x), std::floor(f.p.y), sz, sz, withAlpha(f.col, a * tw));
        }
        r.setBlend(Blend::Alpha);
        for (auto &f : fireworks) { // confetti tumbles, so its height squashes with the spin
            if (f.kind != 2)
                continue;
            float fade = clamp01((f.maxLife - f.life) * 2.f);
            float sw = f.size * u * 1.8f, sh = f.size * u * (0.25f + 0.75f * std::abs(std::cos(f.rot)));
            r.rotRect(f.p.x, f.p.y, sw, sh, f.rot * 0.35f, withAlpha(f.col, fade));
        }
        float k = easeOutBack(endT * 0.8f);
        std::string head = cfg.age > 0 ? "Happy " + ordinal(cfg.age) + " Birthday, " + cfg.name + "!"
                                       : "Happy Birthday, " + cfg.name + "!";
        r.textShadow(Face::SerifBold, (int)(38 * u * k), head, fbW * 0.5f, fbH * 0.30f,
                     rgba(0xffe28a), Align::Center);
        int px = (int)(20 * u);
        float ty = fbH * 0.30f + 70 * u;
        for (auto &l : wrapText(Face::Serif, px, fillName(cfg.finalMessage), std::min(fbW - 60 * u, 700 * u))) {
            r.textShadow(Face::Serif, px, l, fbW * 0.5f, ty, rgba(0xffffff), Align::Center);
            ty += r.lineHeight(Face::Serif, px);
        }
        if (endT > 2.f)
            r.textShadow(Face::Sans, (int)(15 * u), "E  keep exploring", fbW * 0.5f, fbH - 50 * u,
                         rgba(0xe9d8a6), Align::Center);
    }

    void drawMenu(const std::vector<std::string> &items, float y, bool fancy = false) {
        float u = ui();
        int px = (int)(22 * u);
        for (size_t i = 0; i < items.size(); i++) {
            bool sel = (int)i == menuSel;
            if (!fancy) {
                std::string s = (sel ? "> " : "  ") + items[i];
                r.textShadow(Face::SerifBold, px, s, fbW * 0.5f, y + i * 38 * u,
                             sel ? rgba(0xffe28a) : rgba(0xcfc7e8), Align::Center);
                continue;
            }
            float iy = y + i * 46 * u;
            if (sel) {
                float tw = r.textWidth(Face::SerifBold, px, items[i]);
                float bw = tw + 90 * u, bx = (fbW - bw) / 2, bh = 36 * u;
                float pulse = 0.85f + 0.15f * std::sin(time * 3.f);
                float by = iy - 4 * u;
                // glass-style button: warm body, bright upper gloss, soft lower glow
                r.rectV(bx, by, bw, bh, rgba(0xffe28a, 0.16f * pulse), rgba(0xffb35a, 0.07f * pulse));
                r.rectV(bx + 2 * u, by + 2 * u, bw - 4 * u, bh * 0.46f, rgba(0xffffff, 0.32f),
                        rgba(0xffffff, 0.06f));
                // a narrow glint that slides across the button
                float sw = 22 * u, sp = std::fmod(time * 0.45f, 1.6f) / 1.6f;
                float sx = bx + (bw + sw) * sp - sw;
                float x0 = std::max(sx, bx + 2 * u), x1 = std::min(sx + sw, bx + bw - 2 * u);
                if (x1 > x0)
                    r.rectV(x0, by + 2 * u, x1 - x0, bh - 4 * u, rgba(0xffffff, 0.0f),
                            rgba(0xffffff, 0.22f));
                r.rect(bx, iy - 4 * u, bw, std::max(1.f, u), rgba(0xe9d8a6, 0.55f));
                r.rect(bx, iy - 4 * u + bh - std::max(1.f, u), bw, std::max(1.f, u),
                       rgba(0xe9d8a6, 0.55f));
                r.rotRect(bx + 16 * u, iy - 4 * u + bh / 2, 6 * u, 6 * u, PI / 4, rgba(0xffe28a));
                r.rotRect(bx + bw - 16 * u, iy - 4 * u + bh / 2, 6 * u, 6 * u, PI / 4,
                          rgba(0xffe28a));
            }
            r.textShadow(Face::SerifBold, px, items[i], fbW * 0.5f, iy,
                         sel ? rgba(0xffe28a) : rgba(0xb9b0dc), Align::Center);
        }
    }

    // Layered night scene for the title screen: gradient sky, aurora curtains, horizon glow
    // with god rays, a moon, bokeh, stars, shooting stars, a castle on drifting hills,
    // fireflies, falling party confetti and a slow glossy sheen.
    void drawTitleBackdrop() {
        float u = ui();
        float W = (float)fbW, H = (float)fbH, cx = W * 0.5f;
        const Texture &T = atlas.tex();
        const Spr &g = atlas.spr(atlas.glow());

        // sky: deep indigo to a warm magenta glow at the horizon
        r.rectV(0, 0, W, H * 0.5f, rgba(0x05031a), rgba(0x241657));
        r.rectV(0, H * 0.5f, W, H * 0.5f, rgba(0x241657), rgba(0x8a3f92));

        r.setBlend(Blend::Additive);

        // aurora curtains: three layers of thin vertical strips with a travelling wave
        static const uint32_t auroraCol[3] = {0x3ad8c8, 0x9a6bff, 0xff6bd0};
        const int cols = 96;
        float cwid = W / cols + 1.f;
        for (int layer = 0; layer < 3; layer++)
            for (int i = 0; i < cols; i++) {
                float t = i / (float)cols;
                float wave = std::sin(t * 6.f + time * 0.35f + layer * 2.1f) * 0.5f +
                             std::sin(t * 13.f - time * 0.5f + layer) * 0.25f;
                float top = H * (0.08f + 0.07f * layer) + wave * H * 0.07f;
                float bot = top + H * (0.24f + 0.05f * std::sin(t * 4.f + time * 0.3f + layer));
                float inten = 0.5f + 0.5f * std::sin(t * 9.f + time * 0.7f + layer * 1.7f);
                float edge = smoothstepf(0.f, 0.08f, t) * (1.f - smoothstepf(0.92f, 1.f, t));
                float al = (0.07f + 0.10f * inten) * edge;
                float mid = top + (bot - top) * 0.3f;
                uint32_t c = auroraCol[layer];
                r.rectV(t * W, top, cwid, mid - top, rgba(c, 0.f), rgba(c, al));
                r.rectV(t * W, mid, cwid, bot - mid, rgba(c, al), rgba(c, 0.f));
            }

        // horizon glow with slowly swaying god rays
        float hy = H * 0.80f;
        r.draw(T, g, cx - W * 0.55f, hy - H * 0.28f, W * 1.1f, H * 0.56f,
               rgba(0xff7ac8, 0.22f + 0.03f * std::sin(time * 0.7f)));
        for (int i = 0; i < 13; i++) {
            float ang = -PI * 0.5f + (i - 6) * 0.17f + std::sin(time * 0.15f + i) * 0.04f;
            float len = H * 0.95f;
            float al = std::max(0.f, 0.035f + 0.025f * std::sin(time * 0.9f + i * 1.3f));
            r.rotRect(cx + std::cos(ang) * len * 0.5f, hy + std::sin(ang) * len * 0.5f,
                      (22 + (i % 3) * 10) * u, len, ang + PI * 0.5f, rgba(0xffd9ff, al));
        }

        // moon with halo
        float mx = W * 0.9f, my = H * 0.1f;
        r.draw(T, g, mx - 110 * u, my - 110 * u, 220 * u, 220 * u, rgba(0xbfa8ff, 0.22f));
        r.draw(T, g, mx - 34 * u, my - 34 * u, 68 * u, 68 * u, rgba(0xfff6e0, 0.85f));
        r.draw(T, g, mx - 16 * u, my - 16 * u, 32 * u, 32 * u, rgba(0xffffff, 0.9f));

        // soft glow behind the title
        float gr = std::min(W * 0.45f, 420 * u);
        r.draw(T, g, cx - gr, H * 0.24f - gr * 0.7f, gr * 2, gr * 1.4f,
               rgba(0x7a5fd0, 0.26f + 0.04f * std::sin(time * 0.8f)));

        // bokeh orbs drifting in the background
        static const uint32_t bok[4] = {0xff7ac8, 0x7ad8ff, 0xffd166, 0xb78bff};
        for (int i = 0; i < 16; i++) {
            uint32_t h = hash2(i, 91, 5);
            float rad = (28 + (float)((h >> 20) & 63)) * u;
            float x = (h & 1023) / 1023.f * W + std::sin(time * 0.18f + i) * 34 * u;
            float y = ((h >> 10) & 1023) / 1023.f * H * 0.85f + std::cos(time * 0.14f + i * 1.3f) * 24 * u;
            float al = 0.045f + 0.035f * std::sin(time * 0.6f + i * 0.9f);
            r.draw(T, g, x - rad, y - rad, rad * 2, rad * 2, rgba(bok[i & 3], al));
        }

        // twinkling stars, a few with a cross-shaped flare
        for (int i = 0; i < 170; i++) {
            uint32_t h = hash2(i, 17, 3);
            float sx = (h & 1023) / 1023.f * W, sy = ((h >> 10) & 1023) / 1023.f * H * 0.78f;
            float tw = 0.4f + 0.6f * std::sin(time * (0.8f + (h & 7) * 0.2f) + i);
            float sz = (i % 9 == 0) ? 3.f : 2.f;
            r.rect(std::floor(sx), std::floor(sy), sz * u, sz * u, rgba(0xfff3c4, 0.3f + 0.5f * tw));
            if (i % 14 == 0) {
                float fl = (5.f + 4.f * tw) * u, al = 0.25f + 0.35f * tw;
                r.rect(std::floor(sx) + u * 0.5f - fl, std::floor(sy) + u * 0.5f, fl * 2, u * 0.7f, rgba(0xffffff, al));
                r.rect(std::floor(sx) + u * 0.5f, std::floor(sy) + u * 0.5f - fl, u * 0.7f, fl * 2, rgba(0xffffff, al));
            }
        }

        // a shooting star every few seconds
        {
            float period = 6.f, cyc = std::fmod(time, period);
            if (cyc < 0.8f) {
                float t = cyc / 0.8f;
                uint32_t h = hash2((int)(time / period), 3, 11);
                vec2 start(W * (0.1f + 0.5f * ((h & 255) / 255.f)), H * (0.05f + 0.18f * (((h >> 8) & 255) / 255.f)));
                vec2 d = glm::normalize(vec2(1.f, 0.45f));
                vec2 head = start + d * (t * W * 0.45f);
                float fade = std::sin(t * PI);
                for (int k = 0; k < 18; k++) {
                    vec2 p = head - d * (k * 8.f * u);
                    float sz = std::max(1.f, 3.f - k * 0.14f) * u;
                    r.rect(std::floor(p.x), std::floor(p.y), sz, sz, rgba(0xfff3c4, (1.f - k / 18.f) * fade * 0.9f));
                }
                r.draw(T, g, head.x - 12 * u, head.y - 12 * u, 24 * u, 24 * u, rgba(0xffffff, 0.6f * fade));
            }
        }

        r.setBlend(Blend::Alpha);

        // hills with a castle silhouette; each ridge drifts at its own speed for parallax
        auto ridge = [&](float base, float amp, float speed, float seed, uint32_t top, uint32_t bot) {
            float step = std::max(3.f, 3.f * u), freq = 0.006f / u;
            for (float x = 0; x < W; x += step) {
                float xo = x + time * speed * u;
                float y = base + amp * u * (std::sin(xo * freq + seed) * 0.5f +
                                            std::sin(xo * freq * 2.3f + seed * 1.7f) * 0.3f +
                                            std::sin(xo * freq * 0.45f + seed * 0.6f) * 0.4f);
                r.rectV(x, y, step + 1, H - y + 1, rgba(top), rgba(bot));
            }
        };
        ridge(H * 0.80f, 18, 3.f, 1.f, 0x3b2a7e, 0x2a1d63);

        {
            float y0 = H * 0.80f;
            vec4 cc = rgba(0x2f2270);
            auto box = [&](float x, float top, float w, float bottom) {
                r.rect(cx + x * u, top, w * u, bottom - top, cc);
            };
            float bottom = H * 0.9f;
            box(-38, y0 - 26 * u, 76, bottom);   // keep
            box(-52, y0 - 46 * u, 15, bottom);   // left tower
            box(37, y0 - 46 * u, 15, bottom);    // right tower
            box(-26, y0 - 38 * u, 11, bottom);   // inner towers
            box(15, y0 - 38 * u, 11, bottom);
            box(-9, y0 - 60 * u, 18, bottom);    // tall centre tower
            struct Roof { float x, top, w; };    // rotated squares: only the upper half shows
            for (Roof rf : {Roof{-44.5f, 46, 15}, Roof{44.5f, 46, 15}, Roof{-20.5f, 38, 11},
                            Roof{20.5f, 38, 11}, Roof{0.f, 60, 18}})
                r.rotRect(cx + rf.x * u, y0 - rf.top * u, rf.w * 0.8f * u, rf.w * 0.8f * u, PI / 4, cc);
            r.setBlend(Blend::Additive); // lit windows
            static const float win[][2] = {{-46, 36}, {43, 36}, {-2, 50}, {-2, 30}, {-24, 24},
                                           {20, 24},  {-30, 12}, {28, 12}, {-2, 12}};
            for (int i = 0; i < 9; i++) {
                float tw = 0.55f + 0.35f * std::sin(time * 1.3f + i * 2.1f);
                float wx = cx + win[i][0] * u, wy = y0 - win[i][1] * u;
                r.draw(T, g, wx - 5 * u, wy - 5 * u, 12 * u, 12 * u, rgba(0xffc66b, 0.18f * tw));
                r.rect(wx, wy, 3 * u, 5 * u, rgba(0xffd98a, tw));
            }
            r.setBlend(Blend::Alpha);
        }
        ridge(H * 0.865f, 14, 6.f, 4.f, 0x2a1e62, 0x170f3a);
        ridge(H * 0.93f, 10, 10.f, 7.f, 0x1a1040, 0x08051a);

        // drifting fireflies rising from the bottom
        r.setBlend(Blend::Additive);
        for (int i = 0; i < 44; i++) {
            uint32_t h = hash2(i, 41, 7);
            float bx = (h & 1023) / 1023.f * W;
            float speed = (7.f + (float)((h >> 10) & 15)) * u;
            float off = ((h >> 14) & 1023) / 1023.f * H;
            float t = std::fmod(time * speed + off, H) / H; // 0 bottom .. 1 top
            float x = bx + std::sin(time * 0.6f + i) * 14 * u;
            float y = H * (1.f - t);
            float fade = smoothstepf(0.f, 0.15f, t) * (1.f - smoothstepf(0.75f, 1.f, t));
            float tw = 0.6f + 0.4f * std::sin(time * 2.f + i * 1.7f);
            r.draw(T, g, x - 7 * u, y - 7 * u, 14 * u, 14 * u, rgba(0xffe9a8, 0.35f * fade * tw));
            r.rect(std::floor(x), std::floor(y), 2 * u, 2 * u, rgba(0xfff6d0, 0.8f * fade * tw));
        }
        r.setBlend(Blend::Alpha);

        // party confetti floating down
        static const uint32_t confCol[6] = {0xff6b9d, 0xffd166, 0x6bd5ff, 0xb78bff, 0x7aeaa0, 0xff8a5c};
        for (int i = 0; i < 26; i++) {
            uint32_t h = hash2(i, 77, 13);
            float speed = (14.f + (float)((h >> 10) & 15)) * u;
            float off = ((h >> 14) & 1023) / 1023.f * H;
            float y = std::fmod(time * speed + off, H + 20 * u) - 10 * u;
            float x = (h & 1023) / 1023.f * W + std::sin(time * 0.9f + i * 1.7f) * 22 * u;
            float rot = time * (0.8f + (i % 5) * 0.3f) + i;
            float flip = 0.35f + 0.65f * std::abs(std::cos(rot * 0.7f));
            r.rotRect(x, y, 5 * u, 3 * u * flip, rot, rgba(confCol[i % 6], 0.55f));
        }

        // slow glossy sheen sweeping across the whole screen
        r.setBlend(Blend::Additive);
        float sx = (std::fmod(time * 0.09f, 1.7f) - 0.35f) * W;
        r.rotRect(sx, H * 0.5f, W * 0.09f, H * 1.9f, 0.38f, rgba(0xffffff, 0.028f));
        r.rotRect(sx + W * 0.065f, H * 0.5f, W * 0.012f, H * 1.9f, 0.38f, rgba(0xffffff, 0.05f));
        r.setBlend(Blend::Alpha);

        // vignette: stacked translucent bars darken the left and right edges
        float vw = W * 0.16f;
        for (int i = 0; i < 8; i++) {
            float w = vw * (i + 1) / 8.f;
            r.rect(0, 0, w, H, rgba(0x05030f, 0.05f));
            r.rect(W - w, 0, w, H, rgba(0x05030f, 0.05f));
        }
    }

    void drawTitle() {
        float u = ui();
        float cx = fbW * 0.5f;
        drawTitleBackdrop();

        // gentle darkening at the top and bottom edges
        r.rectV(0, 0, (float)fbW, fbH * 0.22f, rgba(0x05030f, 0.35f), rgba(0x05030f, 0.f));
        r.rectV(0, fbH * 0.84f, (float)fbW, fbH * 0.16f, rgba(0x05030f, 0.f), rgba(0x05030f, 0.45f));

        // title, ornament and subtitle
        float ty = fbH * 0.17f;
        r.textShadow(Face::SerifBold, (int)(46 * u), cfg.title, cx, ty, rgba(0xffe28a), Align::Center);
        // gloss: a pale additive pass nudged up so the top edge of each letter catches light
        r.setBlend(Blend::Additive);
        r.text(Face::SerifBold, (int)(46 * u), cfg.title, cx, ty - std::max(1.f, u),
               rgba(0xfff4d0, 0.22f), Align::Center);
        r.setBlend(Blend::Alpha);
        // faint glassy reflection under the title
        r.rectV(cx - fbW * 0.3f, ty + 66 * u, fbW * 0.6f, 6 * u, rgba(0xffffff, 0.0f),
                rgba(0xffffff, 0.05f));
        float ly = ty + 74 * u;
        float lw = std::min(fbW * 0.22f, 190 * u);
        float lh = std::max(1.f, u);
        r.rectV(cx - 16 * u - lw, ly, lw, lh, rgba(0xe9d8a6, 0.f), rgba(0xe9d8a6, 0.8f));
        r.rectV(cx + 16 * u, ly, lw, lh, rgba(0xe9d8a6, 0.8f), rgba(0xe9d8a6, 0.f));
        r.rotRect(cx, ly + lh * 0.5f, 7 * u, 7 * u, PI / 4, rgba(0xffe28a));
        r.textShadow(Face::SerifItalic, (int)(20 * u), "A little adventure made for " + cfg.name,
                     cx, ly + 18 * u, rgba(0xe3d6f7), Align::Center);

        std::vector<std::string> items;
        if (hasSave)
            items.push_back("Continue");
        items.push_back("New Adventure");
        items.push_back("Quit");
        {   // frosted-glass card behind the menu
            float cw = std::min(fbW * 0.5f, 400 * u), ch = items.size() * 46 * u + 24 * u;
            float cxl = cx - cw / 2, cyt = fbH * 0.52f - 16 * u;
            r.rectV(cxl, cyt, cw, ch, rgba(0xffffff, 0.09f), rgba(0xb89cff, 0.04f));
            r.rectV(cxl, cyt, cw, ch * 0.42f, rgba(0xffffff, 0.12f), rgba(0xffffff, 0.0f)); // top gloss
            r.rectOutline(cxl, cyt, cw, ch, std::max(1.f, u), rgba(0xe9d8a6, 0.30f));
            r.rect(cxl + 2 * u, cyt + std::max(1.f, u), cw - 4 * u, std::max(1.f, u),
                   rgba(0xffffff, 0.35f)); // bright top edge highlight
        }
        drawMenu(items, fbH * 0.52f, true);

        r.text(Face::Sans, (int)(13 * u), "W/S choose  -  E select  -  F11 fullscreen", cx,
               fbH - 34 * u, rgba(0xffffff, 0.55f), Align::Center);
        size_t nw = Log::warnings().size();
        if (nw)
            r.text(Face::Sans, (int)(13 * u),
                   std::to_string(nw) + " warning(s) - press F1 to read them", 12 * u, fbH - 28 * u,
                   rgba(0xffb36b));
    }

    void drawPaused() {
        float u = ui();
        r.rect(0, 0, (float)fbW, (float)fbH, rgba(0x05030f, 0.6f));
        r.textShadow(Face::SerifBold, (int)(34 * u), "Paused", fbW * 0.5f, fbH * 0.25f,
                     rgba(0xffe28a), Align::Center);
        drawMenu({"Resume", "Save", audio.enabled() ? "Sound: on" : "Sound: off", "Save and quit to title"},
                 fbH * 0.40f);
    }

    void drawReport() {
        float u = ui();
        r.rect(0, 0, (float)fbW, (float)fbH, rgba(0x05030f, 0.88f));
        int px = (int)(14 * u);
        r.text(Face::SansBold, (int)(18 * u), "Warnings and errors (F1 to close)", 24 * u, 20 * u,
               rgba(0xffe28a));
        float y = 56 * u;
        auto &w = Log::warnings();
        if (w.empty())
            r.text(Face::Sans, px, "Nothing to report.", 24 * u, y, rgba(0xffffff));
        for (auto &s : w) {
            for (auto &l : wrapText(Face::Sans, px, s, fbW - 48 * u)) {
                if (y > fbH - 24 * u)
                    return;
                r.text(Face::Sans, px, l, 24 * u, y, rgba(0xffffff));
                y += r.lineHeight(Face::Sans, px);
            }
        }
    }

    void render() {
        glfwGetFramebufferSize(win, &fbW, &fbH);
        if (fbW < 1 || fbH < 1)
            return;
        r.beginFrame(fbW, fbH, rgba(0x0b0818));
        if (mode == Mode::Title) {
            r.setScreen();
            drawTitle();
        } else {
            drawWorldView();
            r.setScreen();
            drawWorldFx();
            if (mode != Mode::Ending)
                drawHud();
            if (mode == Mode::Cake)
                drawCakeOverlay();
            if (mode == Mode::Dialogue)
                drawDialogue();
            else if (mode == Mode::Reveal)
                drawReveal();
            else if (mode == Mode::Paused)
                drawPaused();
            else if (mode == Mode::Ending)
                drawEnding();
        }
        if (showReport) {
            r.setScreen();
            drawReport();
        }
        r.flush();
        if (hit(GLFW_KEY_F2)) {
            std::string path = Paths::file("screenshot.png");
            if (r.screenshot(path))
                Log::info("Saved screenshot to %s", path.c_str());
        }
    }

    int run() {
        double last = glfwGetTime();
        while (!glfwWindowShouldClose(win) && !wantQuit) {
            glfwPollEvents();
            double now = glfwGetTime();
            float dt = (float)std::min(0.05, now - last);
            last = now;
            update(dt);
            render();
            glfwSwapBuffers(win);
        }
        return 0;
    }
};

} // namespace

int main(int, char **argv) {
    Game g;
    if (!g.init(argv[0])) {
        Log::error("Startup failed - see messages above");
        g.shutdown();
        return 1;
    }
    int rc = g.run();
    g.shutdown();
    return rc;
}
