#include "Entities.h"

#include "World.h"

namespace bd {

// Shared little drawing helpers keep the character and object code readable.
// Additive blending is used for light; normal alpha blending is restored right after.
static void glowAt(Renderer &r, const Atlas &a, vec2 c, float rad, vec4 col) {
    r.setBlend(Blend::Additive);
    r.draw(a.tex(), a.spr(a.glow()), c.x - rad, c.y - rad, rad * 2, rad * 2, col);
    r.setBlend(Blend::Alpha);
}
static void shadowAt(Renderer &r, const Atlas &a, vec2 feet, float w = 14, float alpha = 1.f) {
    r.draw(a.tex(), a.get("shadow"), std::floor(feet.x - w / 2), std::floor(feet.y - 3), w,
           w * 0.5f, vec4(1, 1, 1, alpha));
}

// ================================================================ Player
// Player movement is smoothed so changing direction does not feel abrupt. Collision is
// checked on each axis separately, which lets the player slide along walls instead of sticking.
int Player::frame() const {
    return moving ? ((int)walkT) & 3 : 0;
}

void Player::move(float dt, vec2 dir, bool sprint, const World &w) {
    float len = glm::length(dir);
    sprinting = sprint && len > 0.1f;
    vec2 target(0);
    if (len > 0.01f) {
        dir /= len;
        target = dir * (sprinting ? 104.f : 62.f);
        if (std::abs(dir.x) > std::abs(dir.y) + 0.1f)
            facing = dir.x > 0 ? 3 : 2;
        else if (std::abs(dir.y) > 0.1f)
            facing = dir.y > 0 ? 0 : 1;
    }
    vel += (target - vel) * expSmooth(len > 0.01f ? 16.f : 24.f, dt);
    if (len < 0.01f && glm::length(vel) < 3.f)
        vel = vec2(0);

    vec2 np = pos;
    np.x += vel.x * dt;
    if (w.blocked(box(np))) {
        np.x = pos.x;
        vel.x = 0;
    }
    pos.x = np.x;
    np = pos;
    np.y += vel.y * dt;
    if (w.blocked(box(np))) {
        np.y = pos.y;
        vel.y = 0;
    }
    pos.y = np.y;

    float speed = glm::length(vel);
    moving = speed > 8.f;
    if (moving) {
        walkT += dt * speed / 6.5f;
        int f = ((int)walkT) & 3;
        if (f != lastFrame && (f == 1 || f == 3))
            stepEvent = 1;
        lastFrame = f;
        idleT = 0;
    } else {
        idleT += dt;
        walkT = 0;
        lastFrame = 0;
    }
}
// The sprite is positioned from the character feet position, which makes depth sorting and
// collision easier even though the character image extends above the ground position.
void Player::draw(Renderer &r, const Atlas &a, float time) const {
    if (sitK > 0.001f) {
        drawSeated(r, a, time);
        return;
    }
    shadowAt(r, a, pos, 14);
    float bob = (!moving && std::fmod(idleT, 1.8f) > 1.55f) ? -1.f : 0.f;
    const Spr &s = a.get(Atlas::charName("hero", facing, frame()));
    r.drawSprite(a.tex(), s, std::floor(pos.x) - 8, std::floor(pos.y) - 21 + bob);
}
// Seated pose on the throne. The standing sprite is reused: the feet are cropped off, the
// body drops a little, folded legs are added, and a crown drops onto her head. While
// the player hops on or off (sitK < 0.5) the ordinary standing sprite is drawn with a lift.
void Player::drawSeated(Renderer &r, const Atlas &a, float time) const {
    shadowAt(r, a, pos, 14, 1.f - 0.35f * sitK);
    const Spr &base = a.get(Atlas::charName("hero", 0, 0));
    float x = std::floor(pos.x) - 8;
    float y = std::floor(pos.y) - 21 - std::floor(hop);
    if (sitK < 0.5f) {
        r.drawSprite(a.tex(), base, x, y);
        return;
    }
    // gentle breathing: the whole body lifts one pixel now and then
    float breathe = std::sin(sitT * 1.8f) > 0.65f ? -1.f : 0.f;
    const int cut = 5; // rows of leg/feet replaced by the folded legs below
    Spr s = base;
    s.h = std::max(1, base.h - cut);
    s.v1 = lerpf(base.v0, base.v1, (float)s.h / (float)base.h);
    float by = y + 2 + breathe;
    r.drawSprite(a.tex(), s, x, by);
    float ly = by + s.h;
    r.rect(std::floor(pos.x) - 6, ly - 1, 12, 2, rgba(0xb48cf0));  // skirt spreading over the seat
    r.rect(std::floor(pos.x) - 6, ly + 1, 12, 1, rgba(0x8a68c8));
    r.rect(std::floor(pos.x) - 5, ly + 2, 4, 2, rgba(0x6a3a22));   // shoes
    r.rect(std::floor(pos.x) + 1, ly + 2, 4, 2, rgba(0x6a3a22));

    // crown drops in once she is settled
    if (sitT > 0.25f) {
        float cp = easeOutBack((sitT - 0.25f) * 3.f);
        float cy = by + 1 - (1.f - cp) * 8.f;
        float ca = clamp01(cp * 1.5f);
        vec4 gold = rgba(0xf6c93a, ca), hi = rgba(0xfff0a0, ca);
        float cx = std::floor(pos.x) - 4;
        r.rect(cx, cy - 1, 8, 2, gold);
        r.rect(cx, cy - 3, 2, 2, gold);
        r.rect(cx + 3, cy - 4, 2, 3, gold);
        r.rect(cx + 6, cy - 3, 2, 2, gold);
        r.rect(cx + 3, cy - 1, 2, 1, rgba(0xff5a7a, ca));
        r.rect(cx, cy - 1, 8, 1, hi);
    }

    // golden aura and rising sparkles
    float k = sitK;
    r.setBlend(Blend::Additive);
    r.draw(a.tex(), a.spr(a.glow()), pos.x - 30, pos.y - 44, 60, 60,
           vec4(1.f, 0.86f, 0.45f, (0.28f + 0.06f * std::sin(time * 3.f)) * k));
    for (int i = 0; i < 9; i++) {
        float ph = std::fmod(sitT * 0.7f + i / 9.f, 1.f);
        float sx = pos.x + std::sin(i * 2.4f + sitT * 1.5f) * (8.f + ph * 6.f);
        float sy = pos.y - 8 - ph * 34;
        float sz = 3.f + 3.f * std::sin(ph * PI);
        r.draw(a.tex(), a.get("p_star"), sx - sz / 2, sy - sz / 2, sz, sz,
               vec4(1.f, 0.95f, 0.7f, (1.f - ph) * k));
    }
    r.setBlend(Blend::Alpha);
}
void Player::addLights(std::vector<Light> &out, float time) const {
    out.push_back({pos + vec2(0, -10), 58.f, vec4(1.0f, 0.86f, 0.62f, 0.55f)});
    if (sitK > 0.001f)
        out.push_back({pos + vec2(0, -16), 78.f * sitK, vec4(1.f, 0.85f, 0.45f, 0.55f * sitK)});
}

// ================================================================ Companion
// The companion follows a point slightly behind the player, with a little floating motion
// so it feels alive without getting in the way of movement.
void Companion::follow(float dt, const Player &p) {
    t += dt;
    vec2 target = p.pos + vec2(((p.facing == 3) ? -14.f : (p.facing == 2 ? 14.f : 12.f)), -30.f);
    target += vec2(std::sin(t * 1.3f) * 7.f, std::sin(t * 2.3f) * 4.f);
    float dist = glm::length(target - pos);
    float k = expSmooth(dist > 80 ? 12.f : 3.5f, dt);
    pos += (target - pos) * k;
    if (speechT > 0)
        speechT -= dt;
}
void Companion::draw(Renderer &r, const Atlas &a, float time) const {
    float pulse = 0.75f + 0.25f * std::sin(time * 5.f + 1.f);
    glowAt(r, a, pos, 14.f * pulse, vec4(0.9f, 1.0f, 0.45f, 0.55f));
    r.rect(std::floor(pos.x) - 1, std::floor(pos.y) - 1, 2, 2, rgba(0xfffff0));
    bool wing = std::fmod(time * 18.f, 2.f) < 1.f;
    r.rect(std::floor(pos.x) - 3, std::floor(pos.y) - (wing ? 3 : 1), 2, 1, vec4(1, 1, 1, 0.8f));
    r.rect(std::floor(pos.x) + 1, std::floor(pos.y) - (wing ? 3 : 1), 2, 1, vec4(1, 1, 1, 0.8f));
}
void Companion::addLights(std::vector<Light> &out, float time) const {
    out.push_back({pos, 48.f, vec4(0.85f, 1.f, 0.45f, 0.7f)});
}

// ================================================================ NPC
// NPCs share a renderer but can use small sprite-specific behaviours such as blinking,
// bobbing, wing flaps or a ghost-like translucent appearance.
void NPC::draw(Renderer &r, const Atlas &a, float time) const {
    if (sprite == "oak") {
        bool blink = std::fmod(time + phase, 4.f) < 0.18f;
        r.drawSprite(a.tex(), a.get(blink ? "oak_1" : "oak_0"), std::floor(pos.x) - 16,
                     std::floor(pos.y) - 39);
        return;
    }
    if (sprite == "frog") {
        shadowAt(r, a, pos, 14);
        bool puff = std::fmod(time * 1.4f + phase, 2.f) < 0.5f;
        r.drawSprite(a.tex(), a.get(puff ? "frog_1" : "frog_0"), std::floor(pos.x) - 8,
                     std::floor(pos.y) - 14);
        return;
    }
    bool ghost = sprite == "ghost";
    float bob = ghost ? std::sin(time * 2.f + phase) * 2.f - 3.f
                      : ((std::fmod(time + phase, 2.4f) > 2.1f) ? -1.f : 0.f);
    if (!ghost)
        shadowAt(r, a, pos, 14);
    else
        shadowAt(r, a, pos, 10, 0.5f);
    int fr = (std::fmod(time * 1.5f + phase, 2.f) < 1.f) ? 0 : 2;
    if (sprite == "moth")
        fr = (int)(time * 5.f + phase) & 3; // wings keep flapping
    const Spr &s = a.get(Atlas::charName(sprite, facing, ghost ? ((int)(time * 3) & 3) : fr));
    r.drawSprite(a.tex(), s, std::floor(pos.x) - 8, std::floor(pos.y) - 21 + std::floor(bob),
                 ghost ? vec4(1, 1, 1, 0.8f) : vec4(1));
}
void NPC::addLights(std::vector<Light> &out, float time) const {
    if (sprite == "ghost")
        out.push_back({pos + vec2(0, -10), 40.f, vec4(0.6f, 0.75f, 1.f, 0.6f)});
    if (sprite == "moth")
        out.push_back({pos + vec2(0, -10), 36.f, vec4(0.8f, 0.6f, 1.f, 0.5f)});
}

// ================================================================ Chest
// Chests have a short opening animation and keep their opened state once the animation
// finishes. The glow and sparkle effects help the player notice unopened rewards.
void Chest::update(float dt, World &) {
    if (opening) {
        animT += dt / 1.1f;
        if (animT >= 1.f) {
            animT = 1.f;
            opening = false;
            opened = true;
        }
    }
}
void Chest::draw(Renderer &r, const Atlas &a, float time) const {
    shadowAt(r, a, pos, 16);
    int f = 0;
    float shakeX = 0;
    if (opened)
        f = 2;
    else if (opening) {
        f = animT < 0.3f ? 0 : (animT < 0.6f ? 1 : 2);
        if (animT < 0.3f)
            shakeX = std::sin(animT * 120.f) * 0.8f;
    }
    std::string nm = std::string(isFinal ? "chest_gold_" : "chest_wood_") + std::to_string(f);
    vec2 c(pos.x, pos.y - 8);
    if (!opened) {
        float pulse = 0.5f + 0.5f * std::sin(time * 2.4f + pos.x * 0.1f);
        glowAt(r, a, c + vec2(0, -2), (isFinal ? 24.f : 16.f) + pulse * 4.f,
               sealed ? vec4(0.7f, 0.6f, 1.f, 0.35f) : vec4(1.f, 0.9f, 0.5f, 0.28f));
    }
    r.drawSprite(a.tex(), a.get(nm), std::floor(pos.x) - 8 + std::floor(shakeX),
                 std::floor(pos.y) - 16);
    if (!opened && !opening) { // twinkles
        float ph = std::fmod(time * 0.9f + pos.x * 0.37f, 1.f);
        if (ph < 0.35f) {
            float s = std::sin(ph / 0.35f * PI);
            r.setBlend(Blend::Additive);
            r.draw(a.tex(), a.get("p_star"), std::floor(pos.x) + 4, std::floor(pos.y) - 17 - ph * 4,
                   4 + 4 * s, 4 + 4 * s, vec4(1, 1, 0.8f, s));
            r.setBlend(Blend::Alpha);
        }
    }
    if (opened || opening) { // rising glow from the open chest
        float k = opened ? 1.f : easeInOut(animT);
        glowAt(r, a, c + vec2(0, -6), 20.f + 6.f * std::sin(time * 3.f),
               vec4(1.f, 0.92f, 0.6f, 0.35f * k));
    }
}
void Chest::addLights(std::vector<Light> &out, float time) const {
    float k = opened ? 1.f : (opening ? easeInOut(animT) : 0.f);
    float base = isFinal ? 1.4f : 1.f;
    out.push_back(
        {pos + vec2(0, -8), (34.f + 26.f * k) * base, vec4(1.f, 0.88f, 0.5f, 0.45f + 0.5f * k)});
}

// ================================================================ Prop
// Props cover the interactive objects in the world: signs, gates, puzzle pieces and
// decorative objects that can react when the player gets close.
std::string Prop::prompt() const {
    switch (kind) {
    case PropKind::Sign:
        return "Read";
    case PropKind::Lever:
        return open ? "" : "Pull";
    case PropKind::Gate:
    case PropKind::Door:
    case PropKind::Door2:
        return open ? "" : (needKey.empty() ? "Open" : "Unlock");
    case PropKind::GlowFlower:
        return "Touch";
    case PropKind::Secret:
        return open ? "" : "Search";
    case PropKind::Throne:
        return "Sit";
    case PropKind::Pedestal:
        return "";
    case PropKind::Deco:
        return (sprite == "cake" && blowT <= 0) ? "Blow out the candles" : "";
    default:
        return "Inspect";
    }
}
void Prop::update(float dt, World &) {
    // The birthday candles are magic: they relight on their own a few seconds after
    // being blown out.
    if (blowT > 0) {
        blowT += dt;
        if (blowT >= 8.f) {
            blowT = 0;
            relit = true;
        }
    }
}
float Prop::interactRadius() const {
    switch (kind) {
    case PropKind::Door2:
        return 34.f;
    case PropKind::Gate:
        return 32.f;
    case PropKind::Secret:
        return 26.f;
    case PropKind::Throne:
        return 30.f;
    case PropKind::Deco:
        return 44.f; // only the cake is interactive; it is a wide object
    default:
        return 24.f;
    }
}
void Prop::draw(Renderer &r, const Atlas &a, float time) const {
    const Texture &t = a.tex();
    switch (kind) {
    case PropKind::Sign:
        r.drawSprite(t, a.get("sign"), std::floor(pos.x) - 8, std::floor(pos.y) - 16);
        break;
    case PropKind::Lever:
        r.drawSprite(t, a.get(open ? "lever_1" : "lever_0"), std::floor(pos.x) - 8,
                     std::floor(pos.y) - 16);
        break;
    case PropKind::Gate:
        r.drawSprite(t, a.get(open ? "gate_v_open" : "gate_v_closed"), (float)tx * TILE,
                     (float)ty * TILE);
        break;
    case PropKind::Door:
        r.drawSprite(t, a.get(open ? "door_open" : "door_closed"), (float)tx * TILE,
                     (float)ty * TILE);
        break;
    case PropKind::Door2:
        r.drawSprite(t, a.get(open ? "door2_open" : "door2_closed"), (float)tx * TILE,
                     (float)ty * TILE);
        break;
    case PropKind::Pedestal:
        r.drawSprite(t, a.get("pedestal"), std::floor(pos.x) - 8, std::floor(pos.y) - 16);
        break;
    case PropKind::GlowFlower: {
        r.drawSprite(t,
                     a.get("gflower_" + std::to_string(index) + "_" + std::to_string(lit ? 1 : 0)),
                     std::floor(pos.x) - 8, std::floor(pos.y) - 23);
        if (lit) {
            static const vec3 cols[4] = {
                {1, 0.35f, 0.4f}, {1, 0.85f, 0.3f}, {0.35f, 0.7f, 1}, {1, 0.55f, 0.9f}};
            glowAt(r, a, pos + vec2(0, -15), 16.f + 2.f * std::sin(time * 4.f),
                   vec4(cols[index & 3], 0.5f));
        } else {
            float p = 0.5f + 0.5f * std::sin(time * 2.f + phase);
            glowAt(r, a, pos + vec2(0, -15), 9.f, vec4(0.7f, 0.8f, 1.f, 0.12f * p));
        }
        break;
    }
    case PropKind::Secret:
        if (!open && canInteract) { // faint twinkle hints that something is hidden here
            float ph = std::fmod(time * 0.7f + phase, 1.f);
            if (ph < 0.4f) {
                float s = std::sin(ph / 0.4f * PI);
                r.setBlend(Blend::Additive);
                r.draw(t, a.get("p_star"), std::floor(pos.x) - 3, std::floor(pos.y) - 14 - ph * 3,
                       6, 6, vec4(1, 1, 0.8f, s * 0.9f));
                r.setBlend(Blend::Alpha);
            }
        }
        break;
    case PropKind::Throne:
        r.drawSprite(t, a.get("throne"), std::floor(pos.x) - 12, std::floor(pos.y) - 31);
        break;
    case PropKind::Balloon: {
        float by = std::sin(time * 1.1f + phase) * 3.f;
        r.drawSprite(t, a.get("balloon_" + std::to_string(index)),
                     std::floor(pos.x) - 8 + std::floor(std::sin(time * 0.7f + phase) * 1.5f),
                     std::floor(pos.y) - 31 + std::floor(by));
        break;
    }
    case PropKind::Deco: {
        std::string nm = sprite;
        if (sprite == "fountain")
            nm = "fountain_" + std::to_string(((int)(time * 3.f)) & 1);
        const Spr &s = a.get(nm);
        r.drawSprite(t, s, std::floor(pos.x) - s.w / 2, std::floor(pos.y) - s.h);
        if (sprite == "cake") {
            // Candles go out one after another from left to right, leave a wisp of smoke
            // and then relight themselves (see Prop::update).
            for (int i = 0; i < 5; i++) {
                float fx = std::floor(pos.x) - 32 + 21 + i * 5 - 3;
                float fy = std::floor(pos.y) - 56 + 4 - 6;
                float outAt = 0.15f + i * 0.16f;
                float relightAt = 7.2f + i * 0.1f;
                bool isLit = blowT <= 0 || blowT < outAt || blowT >= relightAt;
                if (isLit) {
                    int fr = ((int)(time * 7.f + i * 1.7f)) & 3;
                    float lean = (blowT > 0 && blowT < outAt) ? (blowT / outAt) * 3.f : 0.f;
                    r.drawSprite(t, a.get("flame_" + std::to_string(fr)), fx + std::floor(lean), fy);
                    if (blowT >= relightAt && blowT < relightAt + 0.35f) // little flash on relight
                        glowAt(r, a, vec2(fx + 4, fy + 4), 9.f,
                               vec4(1.f, 0.85f, 0.4f, 0.7f * (1.f - (blowT - relightAt) / 0.35f)));
                } else {
                    float st = blowT - outAt; // seconds since this candle went out
                    if (st < 0.7f)
                        glowAt(r, a, vec2(fx + 4, fy + 5), 6.f,
                               vec4(1.f, 0.4f, 0.15f, 0.5f * (1.f - st / 0.7f)));
                    for (int j = 0; j < 3 && st < 2.6f; j++) {
                        float sj = st - j * 0.22f;
                        if (sj < 0)
                            continue;
                        float u = sj / 2.2f;
                        if (u > 1.f)
                            continue;
                        float sx = fx + 4 + std::sin(sj * 3.f + i + j) * (1.f + u * 2.f);
                        float sy = fy + 4 - sj * 11.f;
                        r.rect(std::floor(sx), std::floor(sy), 2, 2,
                               vec4(0.85f, 0.82f, 0.92f, 0.55f * (1.f - u)));
                    }
                }
            }
        }
        break;
    }
    }
}
void Prop::addLights(std::vector<Light> &out, float time) const {
    if (kind == PropKind::GlowFlower && lit) {
        static const vec3 cols[4] = {
            {1, 0.35f, 0.4f}, {1, 0.85f, 0.3f}, {0.35f, 0.7f, 1}, {1, 0.55f, 0.9f}};
        out.push_back({pos + vec2(0, -14), 40.f, vec4(cols[index & 3], 0.8f)});
    }
    if (lightKind == 0 || lightRadius <= 0)
        return;
    vec4 c = lightKind == 1
                 ? vec4(1.f, 0.82f, 0.5f, 0.9f)
                 : (lightKind == 2 ? vec4(0.8f, 0.6f, 1.f, 0.7f) : vec4(0.6f, 0.8f, 1.f, 0.7f));
    float fl = 1.f + 0.06f * std::sin(time * 9.f + phase * 3.f);
    if (sprite == "cake" && blowT > 0) { // the room dims while the candles are out
        float k = blowT < 0.9f ? lerpf(1.f, 0.35f, blowT / 0.9f)
                  : blowT < 7.2f ? 0.35f
                                 : lerpf(0.35f, 1.f, clamp01((blowT - 7.2f) / 0.8f));
        fl *= k;
    }
    out.push_back({pos + vec2(0, -14), lightRadius * fl, c});
}

// ================================================================ Collectible
// Collectibles use a small bobbing animation and glow to stand out from the ground.
// Their actual pickup logic is handled by the world/gameplay systems, not by drawing.

void Collectible::draw(Renderer &r, const Atlas &a, float time) const {
    float bob = std::sin(time * 2.6f + phase) * 2.5f;
    vec2 c(pos.x, pos.y - 10 + bob);
    shadowAt(r, a, pos, 10, 0.6f);
    if (kind == ItemKind::Shard) {
        glowAt(r, a, c, 20.f, vec4(1.f, 0.9f, 0.4f, 0.55f));
        r.drawSprite(a.tex(), a.get("shard"), std::floor(c.x) - 8, std::floor(c.y) - 8);
        float ph = std::fmod(time * 1.2f + phase, 1.f);
        r.setBlend(Blend::Additive);
        r.drawRotated(a.tex(), a.get("p_star"), c.x + 8 * std::cos(time * 2 + phase),
                      c.y - 6 + 4 * std::sin(time * 2 + phase), 5, 5, time,
                      vec4(1, 1, 0.7f, 1 - ph));
        r.setBlend(Blend::Alpha);
    } else {
        const char *nm = kind == ItemKind::KeyForest
                             ? "key_forest"
                             : (kind == ItemKind::KeyGarden ? "key_garden" : "key_golden");
        vec4 gc = kind == ItemKind::KeyForest
                      ? vec4(0.5f, 1.f, 0.5f, 0.5f)
                      : (kind == ItemKind::KeyGarden ? vec4(1.f, 0.6f, 0.9f, 0.5f)
                                                     : vec4(1.f, 0.85f, 0.3f, 0.55f));
        glowAt(r, a, c, 18.f, gc);
        r.drawSprite(a.tex(), a.get(nm), std::floor(c.x) - 6, std::floor(c.y) - 6);
    }
}
void Collectible::addLights(std::vector<Light> &out, float time) const {
    vec4 c = kind == ItemKind::Shard ? vec4(1.f, 0.9f, 0.5f, 0.8f) : vec4(0.9f, 0.9f, 0.7f, 0.7f);
    out.push_back({pos + vec2(0, -10), 36.f, c});
}

} // namespace bd
