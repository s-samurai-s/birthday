#include "Atlas.h"

namespace bd {

// ============================================================ Canvas
// Canvas is the temporary drawing surface used by the atlas generator.
// Pixels are stored in row-major RGBA order, so every pixel occupies four
// consecutive bytes: red, green, blue, and alpha.

// Write one pixel. Coordinates outside the canvas are ignored so that drawing
// routines can safely work near sprite edges without repeating bounds checks.
void Canvas::set(int x, int y, uint32_t rgb, int a) {
    if (!in(x, y)) return;
    uint8_t* p = &px[((size_t)y * w + x) * 4];
    p[0] = (rgb >> 16) & 255;
    p[1] = (rgb >> 8) & 255;
    p[2] = rgb & 255;
    p[3] = (uint8_t)a;
}
// Composite a colour over the existing pixel using straight-alpha blending.
// The output alpha is calculated first; the RGB channels are then divided by
// that resulting alpha so partially transparent pixels keep the right colour.
void Canvas::blend(int x, int y, uint32_t rgb, float a) {
    if (!in(x, y)) return;
    uint8_t* p = &px[((size_t)y * w + x) * 4];
    float pa = p[3] / 255.f, oa = a + pa * (1 - a);
    if (oa <= 0) return;
    float r = (((rgb >> 16) & 255) * a + p[0] * pa * (1 - a)) / oa;
    float g = (((rgb >> 8) & 255) * a + p[1] * pa * (1 - a)) / oa;
    float b = ((rgb & 255) * a + p[2] * pa * (1 - a)) / oa;
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
    p[3] = (uint8_t)(oa * 255);
}
// Read a pixel as 0xAARRGGBB. Returning zero for an invalid coordinate makes
// an out-of-bounds read behave like a fully transparent pixel.
uint32_t Canvas::get(int x, int y) const {
    if (!in(x, y)) return 0;
    const uint8_t* p = &px[((size_t)y * w + x) * 4];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
// Fill a rectangle by writing each pixel individually. Canvas::set handles
// clipping, which also allows rectangles to extend beyond the canvas safely.
void Canvas::rect(int x, int y, int rw, int rh, uint32_t rgb, int a) {
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++) set(x + i, y + j, rgb, a);
}
// A disc is simply an ellipse whose horizontal and vertical radii are equal.
void Canvas::disc(float cx, float cy, float r, uint32_t rgb) { ellipse(cx, cy, r, r, rgb); }
// Test the centre of every candidate pixel against the ellipse equation.
// Using pixel centres produces a more balanced edge for small pixel-art shapes.
void Canvas::ellipse(float cx, float cy, float rx, float ry, uint32_t rgb, int a) {
    for (int y = (int)std::floor(cy - ry - 1); y <= (int)std::ceil(cy + ry + 1); y++)
        for (int x = (int)std::floor(cx - rx - 1); x <= (int)std::ceil(cx + rx + 1); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) set(x, y, rgb, a);
        }
}
// Draw a line with Bresenham's integer algorithm. It advances along the
// dominant axis and uses the accumulated error to decide when to step sideways.
void Canvas::line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        set(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
// Add a one-pixel outline around opaque content. Candidate pixels are collected
// first and written afterwards; this prevents newly added outline pixels from
// being treated as part of the original shape while the scan is still running.
void Canvas::outline(uint32_t rgb) {
    std::vector<std::pair<int, int>> todo;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (alpha(x, y) == 0 && (alpha(x - 1, y) > 128 || alpha(x + 1, y) > 128 || alpha(x, y - 1) > 128 || alpha(x, y + 1) > 128))
                todo.push_back({x, y});
    for (auto& p : todo) set(p.first, p.second, rgb);
}

// ============================================================ helpers
namespace {
// These helpers are private to this file. They keep the sprite-generation code
// compact while ensuring that repeated details remain deterministic.

// Convert the low 16 bits of the coordinate hash into a normalized value [0, 1].
float hf(int x, int y, uint32_t seed = 0) { return (hash2(x, y, seed) & 0xFFFF) / 65535.f; }
// Scale each RGB channel by f and clamp it to the valid byte range. This is used
// to create highlights and shadows without changing the colour's hue.
uint32_t shade(uint32_t c, float f) {
    auto ch = [&](int s) { return (uint32_t)std::max(0.f, std::min(255.f, (float)((c >> s) & 255) * f)); };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
// Linearly interpolate between two RGB colours. t = 0 returns a, while t = 1
// returns b; values between them create the intermediate colour ramp.
uint32_t mixc(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int s) { return (uint32_t)(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t); };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Shade every opaque pixel of `mask` into `dst` using a 4-step colour ramp lit from (lx,ly).
// Turn a binary shape mask into a shaded sprite. The distance from each opaque
// pixel to the light position selects one of four colours in the ramp. A small
// deterministic hash offset breaks up perfectly smooth bands, giving the result
// a textured pixel-art appearance without introducing random changes per build.
void shadeMask(Canvas& dst, const Canvas& mask, const uint32_t ramp[4], float lx, float ly, float radius, uint32_t seed) {
    for (int y = 0; y < mask.h; y++)
        for (int x = 0; x < mask.w; x++) {
            if (mask.alpha(x, y) < 128) continue;
            float d = std::sqrt((x - lx) * (x - lx) + (y - ly) * (y - ly)) / radius;
            d += (hf(x, y, seed) - 0.5f) * 0.28f;
            int i = d < 0.22f ? 0 : d < 0.5f ? 1 : d < 0.8f ? 2 : 3;
            dst.set(x, y, ramp[i]);
        }
}
// Fill an upright triangle one horizontal scanline at a time. Its half-width
// grows linearly from zero at the apex to halfW at baseY.
void triangle(Canvas& c, int apexX, int apexY, int baseY, int halfW, uint32_t col) {
    for (int y = apexY; y <= baseY; y++) {
        float t = (float)(y - apexY) / std::max(1, baseY - apexY);
        int hw = (int)std::round(halfW * t);
        for (int x = apexX - hw; x <= apexX + hw; x++) c.set(x, y, col);
    }
}

// Carve ragged edges (transparent) on sides whose neighbour is NOT of the same kind. mask bits: N=1 E=2 S=4 W=8 (= neighbour present)
// Remove a shallow, deterministic strip from each exposed tile edge. The four
// low bits of mask represent N, E, S, and W neighbours. An edge is left intact
// when its corresponding neighbour bit is set, allowing adjacent tiles to join
// without a visible seam. edgeCol optionally colours the last pixel of each cut.
void carveEdges(Canvas& c, int mask, uint32_t seed, int maxDepth, uint32_t edgeCol) {
    for (int side = 0; side < 4; side++) {
        if (mask & (1 << side)) continue;
        for (int i = 0; i < 16; i++) {
            int d = 1 + (int)(hash2(i, side, seed) % (uint32_t)maxDepth);
            for (int k = 0; k <= d; k++) {
                int x = 0, y = 0;
                switch (side) {
                    case 0: x = i;
                    y = k;
                    break;
                    case 1: x = 15 - k;
                    y = i;
                    break;
                    case 2: x = i;
                    y = 15 - k;
                    break;
                    default: x = k;
                    y = i;
                    break;
                }
                if (k < d) c.set(x, y, 0, 0);
                else if (edgeCol) c.set(x, y, edgeCol);
            }
        }
    }
}
}  // namespace

// ============================================================ generation
// ArtGen owns the procedural drawing routines. Each routine creates one or
// more temporary canvases and registers them in Atlas using stable sprite names.
// The generator does not own the Atlas; A is the destination for every result.
struct ArtGen {
    Atlas& A;
    explicit ArtGen(Atlas& a) : A(a) {}

    // ------------------------------------------------ terrain
    // Generate ground tiles used by the different regions. Variants share the
    // same base palette but use different deterministic details so repeated
    // tiles do not look like one identical stamp.
    void grass() {
        const uint32_t base[4] = {0x3f8f43, 0x3c8a41, 0x42944a, 0x3a8540};
        for (int v = 0; v < 4; v++) {
            Canvas c(16, 16);
            c.rect(0, 0, 16, 16, base[v]);
            for (int i = 0; i < 40; i++) {
                int x = hash2(i, v, 1) % 16, y = hash2(i, v, 2) % 16;
                c.set(x, y, (hash2(i, v, 3) & 1) ? 0x4ea652 : 0x2f7a38);
            }
            for (int i = 0; i < 3 + v; i++) {   // little blades
                int x = 1 + hash2(i, v, 4) % 14, y = 2 + hash2(i, v, 5) % 12;
                c.set(x, y, 0x6cc464);
                c.set(x, y - 1, 0x6cc464);
                c.set(x + 1, y - 2, 0x8bdc7a);
            }
            A.add("grass" + std::to_string(v), c);
        }
        // sand
        for (int v = 0; v < 2; v++) {
            Canvas c(16, 16);
            c.rect(0, 0, 16, 16, 0xe2cd92);
            for (int i = 0; i < 30; i++) c.set(hash2(i, v, 7) % 16, hash2(i, v, 8) % 16, (i & 1) ? 0xcdb67a : 0xf0dda8);
            A.add("sand" + std::to_string(v), c);
        }
        // garden moss (dark, magical)
        for (int v = 0; v < 3; v++) {
            Canvas c(16, 16);
            c.rect(0, 0, 16, 16, 0x2b6b5c);
            for (int i = 0; i < 44; i++) c.set(hash2(i, v, 9) % 16, hash2(i, v, 10) % 16, (i % 3) ? 0x347a68 : 0x215a4f);
            for (int i = 0; i < 4; i++) {
                int x = hash2(i, v, 11) % 15, y = hash2(i, v, 12) % 15;
                c.set(x, y, 0x9ff5d8); if (i == 0) { c.set(x + 1, y, 0x5ed6b0); c.set(x - 1, y, 0x5ed6b0); }
            }
            A.add("moss" + std::to_string(v), c);
        }
        // stone cobbles
        for (int v = 0; v < 2; v++) {
            Canvas c(16, 16);
            c.rect(0, 0, 16, 16, 0x8e8c9c);
            for (int row = 0; row < 4; row++) {
                int off = (row & 1) ? 4 : 0;
                for (int col = -1; col < 3; col++) {
                    int x = col * 8 + off, y = row * 4;
                    uint32_t cc = mixc(0xa9a7b8, 0xc2c0d0, hf(row, col, 20 + v));
                    c.rect(x + 1, y + 1, 7, 3, cc);
                    c.rect(x + 1, y + 1, 7, 1, shade(cc, 1.1f));
                }
            }
            A.add("stone" + std::to_string(v), c);
        }
        // plaza (final area): pale rose marble
        for (int v = 0; v < 2; v++) {
            Canvas c(16, 16);
            c.rect(0, 0, 16, 16, 0xe9d8e8);
            c.rect(0, 0, 16, 1, 0xd0b8d4);
            c.rect(0, 0, 1, 16, 0xd0b8d4);
            for (int i = 0; i < 10; i++) c.set(hash2(i, v, 30) % 16, hash2(i, v, 31) % 16, 0xf8eef8);
            if (v) { c.line(3, 14, 10, 6, 0xdcc6e0); }
            A.add("plaza" + std::to_string(v), c);
        }
        // castle checker floor
        {
            Canvas a(16, 16), b(16, 16);
            a.rect(0, 0, 16, 16, 0xd6cdbf);
            b.rect(0, 0, 16, 16, 0x9d94ad);
            for (Canvas* c : {&a, &b}) {
                c->rect(0, 0, 16, 1, shade(c->get(1, 1), 1.12f) & 0xFFFFFF);
                c->rect(0, 0, 1, 16, shade(c->get(1, 1), 1.12f) & 0xFFFFFF);
                c->rect(0, 15, 16, 1, shade(c->get(1, 1), 0.85f) & 0xFFFFFF);
                c->rect(15, 0, 1, 16, shade(c->get(1, 1), 0.85f) & 0xFFFFFF);
                for (int i = 0; i < 6; i++) c->set(2 + hash2(i, 1, 40) % 12, 2 + hash2(i, 2, 40) % 12, shade(c->get(5, 5), 1.06f) & 0xFFFFFF);
            }
            A.add("tile_a", a);
            A.add("tile_b", b);
        }
        // wood floor
        for (int v = 0; v < 2; v++) {
            Canvas c(16, 16);
            for (int row = 0; row < 4; row++) {
                uint32_t cc = mixc(0x9b6a3c, 0xb07c48, hf(row, v, 50));
                c.rect(0, row * 4, 16, 3, cc);
                c.rect(0, row * 4 + 3, 16, 1, 0x5e3b20);
                int jx = 3 + hash2(row, v, 51) % 10;
                c.rect(jx, row * 4, 1, 3, 0x6e4626);
                for (int i = 0; i < 4; i++) c.set(hash2(i, row, 52 + v) % 16, row * 4 + 1, shade(cc, 0.88f) & 0xFFFFFF);
            }
            A.add("wood" + std::to_string(v), c);
        }
    }

    // Generate all 16 neighbour combinations for paths and carpets. Each bit in
    // m indicates whether a matching tile exists on that side, so exposed edges
    // can receive a border while connected edges remain seamless.
    void pathAndCarpet() {
        for (int m = 0; m < 16; m++) {
            Canvas c(16, 16);   // dirt path
            c.rect(0, 0, 16, 16, 0xb48f5a);
            for (int i = 0; i < 36; i++) c.set(hash2(i, m, 60) % 16, hash2(i, m, 61) % 16, (i % 3 == 0) ? 0xc9a870 : 0x9c7a49);
            for (int i = 0; i < 3; i++) { int x = hash2(i, m, 62) % 14, y = hash2(i, m, 63) % 14; c.rect(x, y, 2, 1, 0xd6bc86); c.set(x, y + 1, 0x8a6a3e); }
            carveEdges(c, m, 64, 2, 0x8a6a3e);
            A.add("path_" + std::to_string(m), c);

            Canvas r(16, 16);   // carpet
            r.rect(0, 0, 16, 16, 0xa8283c);
            for (int y = 0; y < 16; y += 4)
                for (int x = ((y / 4) & 1) * 4; x < 16; x += 8) { r.set(x + 2, y + 2, 0xc03a50); r.set(x + 1, y + 2, 0x8e1f30); r.set(x + 3, y + 2, 0x8e1f30); r.set(x + 2, y + 1, 0x8e1f30); r.set(x + 2, y + 3, 0x8e1f30); }
            for (int side = 0; side < 4; side++) {
                if (m & (1 << side)) continue;
                for (int i = 0; i < 16; i++) {
                    int x = 0, y = 0, x2 = 0, y2 = 0;
                    switch (side) {
                        case 0: x = i;
                        y = 0;
                        x2 = i;
                        y2 = 1;
                        break;
                        case 1: x = 15;
                        y = i;
                        x2 = 14;
                        y2 = i;
                        break;
                        case 2: x = i;
                        y = 15;
                        x2 = i;
                        y2 = 14;
                        break;
                        default: x = 0;
                        y = i;
                        x2 = 1;
                        y2 = i;
                        break;
                    }
                    r.set(x, y, 0xe6b840);
                    r.set(x2, y2, 0xc8922a);
                }
            }
            A.add("carpet_" + std::to_string(m), r);
        }
    }

    // Water has two independent dimensions of variation: m describes which
    // sides connect to other water tiles, and f selects one of four animation
    // frames. The same frame index is used across the map to keep the surface
    // animation coherent. Bridges and the glowing stepping stone are generated
    // after the water tiles because they are drawn as separate overlays.
    void water() {
        for (int m = 0; m < 16; m++)
            for (int f = 0; f < 4; f++) {
                // Each frame shifts the wave phases, changing highlights and
                // darker ripples while keeping the tile dimensions unchanged.
                Canvas c(16, 16);
                c.rect(0, 0, 16, 16, 0x2c74c4);
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) {
                        float w = std::sin((x * 0.55f) + (y * 0.9f) + f * (PI / 2));
                        float w2 = std::sin((x * 0.33f) - (y * 1.7f) - f * (PI / 2) * 1.0f);
                        if (w > 0.92f) c.set(x, y, 0x5aa6ea);
                        else if (w2 > 0.96f) c.set(x, y, 0x1f5fb0);
                        if (hash2(x, y, 70 + f) % 61 == 0) c.set(x, y, 0xd8f0ff);
                    }
                // shoreline foam on sides without water (m bit set = water neighbour)
                for (int side = 0; side < 4; side++) {
                    if (m & (1 << side)) continue;
                    for (int i = 0; i < 16; i++) {
                        int wob = (int)std::round(std::sin(i * 0.9f + f * 1.57f) * 0.7f);
                        int d1 = 1 + wob;
                        if (d1 < 0) d1 = 0;
                        for (int k = 0; k <= d1 + 1; k++) {
                            int x = 0, y = 0;
                            switch (side) { case 0: x = i; y = k; break; case 1: x = 15 - k; y = i; break; case 2: x = i; y = 15 - k; break; default: x = k; y = i; break; }
                            c.set(x, y, k <= d1 ? 0xeaf6ff : 0x8fc8f4);
                        }
                    }
                }
                A.add("water_" + std::to_string(m) + "_" + std::to_string(f), c);
            }
        // bridge planks
        for (int v = 0; v < 2; v++) {
            Canvas c(16, 16);
            for (int i = 0; i < 4; i++) {
                uint32_t cc = mixc(0xa8743c, 0xc08a4c, hf(i, v, 80));
                if (!v) { c.rect(i * 4, 0, 3, 16, cc); c.rect(i * 4 + 3, 0, 1, 16, 0x5a381c); c.rect(i * 4, 0, 1, 16, shade(cc, 1.12f) & 0xFFFFFF); }
                else { c.rect(0, i * 4, 16, 3, cc); c.rect(0, i * 4 + 3, 16, 1, 0x5a381c); c.rect(0, i * 4, 16, 1, shade(cc, 1.12f) & 0xFFFFFF); }
            }
            if (!v) { c.rect(0, 0, 16, 2, 0x6e4626); c.rect(0, 14, 16, 2, 0x6e4626); c.rect(0, 2, 16, 1, 0x3e2a18); c.rect(0, 13, 16, 1, 0x3e2a18); }
            else { c.rect(0, 0, 2, 16, 0x6e4626); c.rect(14, 0, 2, 16, 0x6e4626); c.rect(2, 0, 1, 16, 0x3e2a18); c.rect(13, 0, 1, 16, 0x3e2a18); }
            A.add(v ? "bridge_v" : "bridge_h", c);
        }
        // glow stepping stone (drawn over water, walkable)
        {
            Canvas c(16, 16);
            c.disc(8, 8.5f, 6.2f, 0x3a4a78);
            c.disc(8, 8, 5.6f, 0x6f84c0);
            c.disc(7.4f, 7.4f, 3.6f, 0x9ab4f0);
            c.rect(7, 6, 2, 3, 0xe4f4ff);
            c.rect(6, 7, 4, 1, 0xe4f4ff);
            A.add("glowstone", c);
        }
    }

    // ------------------------------------------------ walls / interiors
    // Build wall faces, wall tops, and decorative interior pieces. The local
    // brick lambda is shared by the wall variants so their mortar spacing and
    // brick highlights stay consistent.
    void walls() {
        auto brick = [&](Canvas& c, uint32_t base, uint32_t mortar, uint32_t seed) {
            c.rect(0, 0, 16, 16, mortar);
            for (int row = 0; row < 4; row++) {
                int off = (row & 1) ? 4 : 0;
                for (int col = -1; col < 3; col++) {
                    int x = col * 8 + off;
                    uint32_t cc = mixc(shade(base, 0.9f), shade(base, 1.12f), hf(row, col, seed));
                    c.rect(x + 1, row * 4 + 1, 7, 3, cc);
                    c.rect(x + 1, row * 4 + 1, 7, 1, shade(cc, 1.15f));
                }
            }
        };
        {
            Canvas c(16, 16);
            brick(c, 0x7a6f94, 0x463d5c, 90);
            c.rect(0, 15, 16, 1, 0x2e2740);
            A.add("wall_face", c);
            Canvas t(16, 16);
            t.rect(0, 0, 16, 16, 0xa59cc0);
            t.rect(0, 0, 16, 2, 0xc4bce0);
            t.rect(0, 14, 16, 2, 0x7e7599);
            for (int i = 0; i < 14; i++) t.set(hash2(i, 1, 91) % 16, hash2(i, 2, 91) % 16, 0xb4acd0);
            A.add("wall_top", t);
            Canvas tz(16, 16);   // torch wall
            brick(tz, 0x7a6f94, 0x463d5c, 90);
            tz.rect(7, 5, 2, 7, 0x3a2e22);
            tz.rect(6, 4, 4, 2, 0x2a2018);
            tz.rect(5, 11, 6, 2, 0x2a2018);
            A.add("wall_torch", tz);
            Canvas bk(16, 16);   // bookshelf
            bk.rect(0, 0, 16, 16, 0x5a381c);
            for (int row = 0; row < 3; row++) {
                bk.rect(1, row * 5 + 1, 14, 4, 0x2e1c0e);
                int x = 1;
                while (x < 15) {
                    int bw = 1 + (int)(hash2(x, row, 92) % 3);
                    uint32_t cols[6] = {0xc0392b, 0x2e86c1, 0x27ae60, 0xd4ac0d, 0x8e44ad, 0xe67e22};
                    uint32_t cc = cols[hash2(x, row, 93) % 6];
                    int bh = 3 + (int)(hash2(x, row, 94) % 2);
                    bk.rect(x, row * 5 + 5 - bh, bw, bh, cc);
                    bk.rect(x, row * 5 + 5 - bh, 1, bh, shade(cc, 1.2f));
                    x += bw;
                }
                bk.rect(0, row * 5 + 5, 16, 1, 0x7a5028);
            }
            bk.rect(0, 15, 16, 1, 0x3a2410);
            A.add("wall_book", bk);
            for (int v = 0; v < 2; v++) {   // paintings
                Canvas pc(16, 16);
                brick(pc, 0x7a6f94, 0x463d5c, 90);
                pc.rect(2, 2, 12, 11, 0xd4a82a);
                pc.rect(3, 3, 10, 9, 0x8a6414);
                if (!v) { pc.rect(4, 4, 8, 7, 0x6ab8f0); pc.rect(4, 8, 8, 3, 0x4a9a4a); pc.disc(10, 6, 1.4f, 0xfff0a0); }
                else { pc.rect(4, 4, 8, 7, 0x2a2a60); pc.rect(4, 9, 8, 2, 0x1a1a3c); pc.set(6, 5, 0xffffff); pc.set(9, 6, 0xffffff); pc.set(11, 5, 0xffe9a0); pc.disc(7, 8, 1.3f, 0xf0e6c0); }
                A.add("wall_paint" + std::to_string(v), pc);
            }
            Canvas wn(16, 16);   // window
            brick(wn, 0x7a6f94, 0x463d5c, 90);
            wn.rect(4, 3, 8, 11, 0x2a2438);
            wn.rect(5, 4, 6, 9, 0x23306a);
            wn.rect(5, 4, 6, 3, 0x2c3c84);
            wn.set(6, 5, 0xffffff);
            wn.set(9, 8, 0xffffff);
            wn.set(8, 6, 0xffe9a0);
            wn.set(7, 11, 0xffffff);
            wn.rect(7, 4, 2, 9, 0x2a2438);
            wn.rect(5, 8, 6, 1, 0x2a2438);
            A.add("wall_window", wn);
        }
        {   // hedge block
            Canvas c(16, 16);
            c.rect(0, 0, 16, 16, 0x2e7a3a);
            for (int i = 0; i < 70; i++) {
                int x = hash2(i, 1, 100) % 16, y = hash2(i, 2, 100) % 16;
                float r = hf(i, 3, 100);
                c.set(x, y, r < 0.35f ? 0x1f5e2c : r < 0.7f ? 0x3c9448 : 0x58b45a);
            }
            c.rect(0, 0, 16, 2, 0x58b45a);
            c.rect(0, 14, 16, 2, 0x1a4d26);
            A.add("hedge", c);
        }
    }

    // ------------------------------------------------ nature objects
    // Nature sprites use masks to define silhouettes, then shadeMask applies
    // lighting before outlines and small decorative details are added.
    void trees() {
        const uint32_t gr[4] = {0x86d86c, 0x55b552, 0x3b9042, 0x2a6c3a};
        const uint32_t gr2[4] = {0xb4dc6a, 0x86bf4e, 0x5f9a3e, 0x437230};
        for (int v = 0; v < 2; v++) {
            Canvas mask(32, 40), c(32, 40);
            mask.disc(16, 13, 12, 1);
            mask.disc(8, 19, 8, 1);
            mask.disc(24, 19, 8, 1);
            mask.disc(16, 22, 8.5f, 1);
            if (v) { mask.disc(6, 12, 6, 1); mask.disc(26, 11, 6, 1); }
            // trunk
            c.rect(13, 26, 6, 14, 0x6b4423);
            c.rect(16, 26, 3, 14, 0x4d2f18);
            c.rect(13, 26, 1, 14, 0x8a5a30);
            c.rect(11, 37, 3, 3, 0x6b4423);
            c.rect(18, 37, 3, 3, 0x4d2f18);
            shadeMask(c, mask, v ? gr2 : gr, 11, 7, 24, 110 + v);
            c.outline(0x1c3a26);
            A.add("tree" + std::to_string(v), c);
        }
        {   // pine
            Canvas mask(24, 40), c(24, 40);
            const uint32_t pr[4] = {0x6cc79a, 0x3f9f72, 0x2c7a58, 0x1e5642};
            c.rect(10, 32, 4, 8, 0x5a3a20);
            c.rect(12, 32, 2, 8, 0x3e2812);
            triangle(mask, 12, 1, 14, 7, 1);
            triangle(mask, 12, 9, 24, 9, 1);
            triangle(mask, 12, 17, 34, 11, 1);
            shadeMask(c, mask, pr, 8, 8, 26, 120);
            c.outline(0x173a2e);
            A.add("pine0", c);
        }
        {   // magic garden trees
            for (int v = 0; v < 2; v++) {
                Canvas mask(40, 48), c(40, 48);
                const uint32_t mr[4] = {0xb8a0f0, 0x8c70d4, 0x6a50ae, 0x4a3784};
                const uint32_t mr2[4] = {0x9ae8e0, 0x60c4c4, 0x4296a8, 0x2f6a84};
                c.rect(17, 28, 7, 20, 0x5a3a52);
                c.rect(21, 28, 3, 20, 0x3e2640);
                c.rect(17, 28, 1, 20, 0x7a5272);
                c.rect(14, 44, 4, 4, 0x5a3a52);
                c.rect(23, 44, 4, 4, 0x3e2640);
                mask.disc(20, 15, 14, 1);
                mask.disc(10, 22, 9, 1);
                mask.disc(30, 22, 9, 1);
                mask.disc(20, 25, 10, 1);
                shadeMask(c, mask, v ? mr2 : mr, 14, 8, 30, 130 + v);
                for (int i = 0; i < 26; i++) {
                    int x = hash2(i, v, 140) % 40, y = hash2(i, v, 141) % 36;
                    if (mask.alpha(x, y) > 128) {
                        uint32_t bc = (i % 3 == 0) ? 0xffe6f8 : (i % 3 == 1) ? 0xff9ed8 : 0xbaf4ff;
                        c.set(x, y, bc); if (i % 4 == 0) { c.set(x + 1, y, bc); c.set(x, y + 1, bc); }
                    }
                }
                c.outline(v ? 0x1c3a52 : 0x2a1f4a);
                A.add("gtree" + std::to_string(v), c);
            }
        }
        {   // bushes
            for (int v = 0; v < 3; v++) {
                Canvas mask(16, 16), c(16, 16);
                const uint32_t br[4] = {0x7ad26a, 0x4eae50, 0x398c42, 0x2a6a38};
                mask.disc(8, 10, 6, 1);
                mask.disc(4, 11, 4, 1);
                mask.disc(12, 11, 4, 1);
                if (v == 1) mask.disc(8, 6, 4, 1);
                shadeMask(c, mask, br, 6, 6, 14, 150 + v);
                if (v == 2)
                    for (int i = 0; i < 6; i++) {
                        int x = 3 + hash2(i, 0, 155) % 10, y = 6 + hash2(i, 1, 155) % 7;
                        if (mask.alpha(x, y) > 128) { c.set(x, y, 0xff8ec8); c.set(x + 1, y, 0xffc4e4); }
                    }
                c.outline(0x1c3a26);
                A.add("bush" + std::to_string(v), c);
            }
            Canvas mask(16, 16), c(16, 16);   // flowering bush (garden)
            const uint32_t br[4] = {0x9ae8c0, 0x5cc49a, 0x3c9a7c, 0x2a7060};
            mask.disc(8, 10, 6.5f, 1);
            mask.disc(4, 11, 4, 1);
            mask.disc(12, 11, 4, 1);
            shadeMask(c, mask, br, 6, 6, 14, 160);
            for (int i = 0; i < 9; i++) { int x = 2 + hash2(i, 0, 161) % 12, y = 5 + hash2(i, 1, 161) % 9; if (mask.alpha(x, y) > 128) { c.set(x, y, i % 2 ? 0xff9ee0 : 0xfff0a0); } }
            c.outline(0x1c4a40);
            A.add("fbush", c);
        }
        {   // rocks
            for (int v = 0; v < 2; v++) {
                Canvas mask(16, 16), c(16, 16);
                const uint32_t rr[4] = {0xd4d8e2, 0xa4abbc, 0x7c8498, 0x5a6074};
                mask.ellipse(8, 11, 7, 5, 1);
                mask.ellipse(v ? 10 : 6, 8, 4.5f, 4, 1);
                shadeMask(c, mask, rr, 5, 5, 14, 170 + v);
                c.outline(0x323848);
                A.add("rock" + std::to_string(v), c);
            }
        }
        {   // stump, giant mushroom, reeds, lily, tall crystal
            Canvas c(16, 16);
            c.ellipse(8, 11, 6, 4, 0x6b4423);
            c.rect(2, 7, 12, 5, 0x7b5230);
            c.ellipse(8, 7, 6, 3, 0xc89a5e);
            c.ellipse(8, 7, 3.5f, 1.6f, 0xa97a44);
            c.outline(0x2e1c0e);
            A.add("stump", c);

            Canvas m(16, 24);
            m.rect(6, 12, 4, 11, 0xf2e8d0);
            m.rect(6, 12, 1, 11, 0xffffff);
            m.rect(9, 12, 1, 11, 0xd6c8a8);
            Canvas mk(16, 24);
            mk.ellipse(8, 9, 8, 7, 1);
            mk.rect(0, 9, 16, 1, 0);
            const uint32_t mr[4] = {0xff7a6e, 0xe04a4a, 0xb02e3c, 0x80202e};
            shadeMask(m, mk, mr, 5, 4, 14, 175);
            for (int i = 0; i < 5; i++) { int x = 3 + hash2(i, 0, 176) % 10, y = 4 + hash2(i, 1, 176) % 5; if (mk.alpha(x, y) > 128) { m.set(x, y, 0xfff2f0); m.set(x + 1, y, 0xfff2f0); } }
            m.outline(0x40121c);
            A.add("bigmush", m);

            for (int f = 0; f < 2; f++) {
                Canvas r(16, 16);
                for (int i = 0; i < 5; i++) {
                    int x = 2 + i * 3, sway = (f ? 1 : 0) * ((i & 1) ? 1 : -1);
                    int hgt = 9 + (int)(hash2(i, 0, 180) % 5);
                    for (int y = 0; y < hgt; y++) { int xx = x + (y > hgt - 4 ? sway : 0); r.set(xx, 15 - y, y % 3 == 0 ? 0x5e9a46 : 0x3f7a38); }
                    r.rect(x + sway - 1, 15 - hgt - 2, 2, 3, 0x7a4a2a);
                }
                A.add("reeds" + std::to_string(f), r);
            }
            Canvas l(16, 16);
            l.ellipse(8, 8, 6.5f, 5, 0x3a9a4a);
            l.ellipse(8, 7.5f, 5.5f, 4, 0x56b85c);
            for (int y = 8; y < 13; y++) l.set(8 + (y - 8) / 3, y, 0);
            l.ellipse(8, 7, 2, 1.6f, 0xff9ed8);
            l.set(8, 6, 0xfff0a0);
            A.add("lily", l);

            for (int v = 0; v < 2; v++) {
                Canvas mask(16, 24), cc(16, 24);
                const uint32_t cr[2][4] = {{0xe0f8ff, 0x8fd8ff, 0x4aa8e8, 0x2a6ac0}, {0xffe4f8, 0xffa8e4, 0xe068bc, 0xa03a8c}};
                for (int y = 0; y < 22; y++) { int hw = y < 6 ? 1 + y / 2 : (y > 17 ? std::max(1, (22 - y) * 2) : 4); for (int x = 8 - hw; x < 8 + hw; x++) mask.set(x, y + 1, 1); }
                for (int y = 8; y < 22; y++) { int hw = y > 19 ? 1 : 2; for (int x = 2 - hw + 1; x < 2 + hw; x++) mask.set(x, y + 1, 1); }
                shadeMask(cc, mask, cr[v], 6, 5, 16, 185 + v);
                cc.line(7, 4, 7, 15, v ? 0xfff0ff : 0xf4feff);
                cc.outline(v ? 0x4a1840 : 0x14305e);
                A.add("crystal" + std::to_string(v), cc);
            }
        }
        {   // pillar + lamp
            Canvas p(16, 32);
            p.rect(2, 28, 12, 4, 0xb8b0c8);
            p.rect(2, 28, 12, 1, 0xe0d8f0);
            p.rect(4, 6, 8, 22, 0xe4dcf0);
            p.rect(4, 6, 2, 22, 0xfaf4ff);
            p.rect(10, 6, 2, 22, 0xb4abc8);
            for (int y = 8; y < 26; y += 4) p.rect(7, y, 1, 3, 0xcfc6e0);
            p.rect(2, 2, 12, 4, 0xc4bcd6);
            p.rect(2, 2, 12, 1, 0xf0e8fa);
            p.rect(3, 6, 10, 1, 0x9a92b0);
            p.outline(0x3a3250);
            A.add("pillar", p);
            Canvas l(16, 32);
            l.rect(7, 12, 2, 18, 0x3a3648);
            l.rect(5, 28, 6, 3, 0x2a2638);
            l.rect(5, 29, 6, 1, 0x4a4660);
            l.rect(4, 4, 8, 8, 0x2a2638);
            l.rect(5, 5, 6, 6, 0xffe39a);
            l.rect(6, 6, 4, 4, 0xfff7d0);
            l.rect(3, 3, 10, 2, 0x3a3648);
            l.rect(6, 1, 4, 2, 0x3a3648);
            l.outline(0x14121c);
            A.add("lamp", l);
        }
        // great tree (garden centrepiece, 64x80)
        {
            Canvas mask(64, 80), c(64, 80);
            const uint32_t mr[4] = {0xcab4f8, 0x9a7ce0, 0x7456bc, 0x523a8c};
            c.rect(24, 44, 16, 36, 0x5e3c58);
            for (int y = 44; y < 80; y++) { c.rect(24, y, 3, 1, 0x7e5276); c.rect(36, y, 4, 1, 0x3e2540); }
            c.rect(18, 72, 8, 8, 0x5e3c58);
            c.rect(38, 72, 8, 8, 0x3e2540);
            mask.disc(32, 24, 22, 1);
            mask.disc(14, 34, 14, 1);
            mask.disc(50, 34, 14, 1);
            mask.disc(32, 38, 16, 1);
            mask.disc(20, 16, 12, 1);
            mask.disc(44, 16, 12, 1);
            shadeMask(c, mask, mr, 22, 10, 50, 190);
            for (int i = 0; i < 80; i++) {
                int x = hash2(i, 0, 191) % 64, y = hash2(i, 1, 191) % 56;
                if (mask.alpha(x, y) > 128) {
                    uint32_t bc = (i % 3 == 0) ? 0xffe6f8 : (i % 3 == 1) ? 0xff9ed8 : 0xbaf4ff;
                    c.set(x, y, bc); if (i % 3 == 0) { c.set(x + 1, y, bc); c.set(x, y + 1, bc); }
                }
            }
            c.outline(0x241846);
            A.add("greattree", c);
        }
        // oak (talking tree, 32x40 with face)
        for (int f = 0; f < 2; f++) {
            Canvas mask(32, 40), c(32, 40);
            const uint32_t gr3[4] = {0x9adc78, 0x62b85a, 0x45954a, 0x2f6c3e};
            c.rect(11, 20, 10, 20, 0x74492a);
            c.rect(17, 20, 4, 20, 0x54321c);
            c.rect(11, 20, 1, 20, 0x9a6438);
            c.rect(8, 36, 4, 4, 0x74492a);
            c.rect(20, 36, 4, 4, 0x54321c);
            mask.disc(16, 11, 11, 1);
            mask.disc(7, 15, 7, 1);
            mask.disc(25, 15, 7, 1);
            shadeMask(c, mask, gr3, 11, 6, 22, 195);
            // face
            if (!f) { c.rect(13, 26, 2, 3, 0x1a1008); c.rect(18, 26, 2, 3, 0x1a1008); c.set(13, 26, 0xffffff); c.set(18, 26, 0xffffff); }
            else { c.rect(13, 28, 2, 1, 0x1a1008); c.rect(18, 28, 2, 1, 0x1a1008); }
            c.rect(14, 32, 5, 1, 0x1a1008);
            c.set(13, 31, 0x1a1008);
            c.set(19, 31, 0x1a1008);
            c.rect(11, 29, 2, 1, 0xd88060);
            c.rect(20, 29, 2, 1, 0xd88060);
            c.rect(10, 24, 4, 1, 0xd8c8a0);
            c.rect(17, 24, 4, 1, 0xd8c8a0);
              // eyebrows
            c.outline(0x1c3a26);
            A.add("oak_" + std::to_string(f), c);
        }
    }

    // Small ground decorations are deliberately kept at low resolutions. They
    // are intended to be scattered over terrain, so their silhouettes and
    // colours remain readable without competing with larger objects.
    void decor() {
        for (int v = 0; v < 2; v++) {
            Canvas c(8, 8);
            for (int i = 0; i < 4; i++) { int x = 1 + i * 2; int h = 3 + (int)(hash2(i, v, 200) % 3); for (int y = 0; y < h; y++) c.set(x + (y > h - 2 ? (i & 1 ? 1 : -1) : 0), 7 - y, y == h - 1 ? 0x8bdc7a : 0x56b052); }
            A.add("tuft" + std::to_string(v), c);
        }
        const uint32_t fc[5] = {0xff5a6e, 0xffd84a, 0x6ab8ff, 0xff8ee0, 0xffffff};
        const char* fn[5] = {"r", "y", "b", "p", "w"};
        for (int i = 0; i < 5; i++) {
            Canvas c(8, 8);
            c.set(4, 7, 0x3f7a38);
            c.set(4, 6, 0x3f7a38);
            c.set(4, 5, 0x3f7a38);
            c.set(5, 6, 0x56b052);
            c.set(3, 3, fc[i]);
            c.set(5, 3, fc[i]);
            c.set(4, 2, fc[i]);
            c.set(4, 4, fc[i]);
            c.set(4, 3, i == 1 ? 0xff9a3a : 0xfff2a0);
            A.add(std::string("flower_") + fn[i], c);
        }
        {
            Canvas c(8, 8);
            c.rect(1, 5, 3, 2, 0x9a9eb0);
            c.rect(5, 4, 2, 2, 0x8a8ea0);
            c.set(1, 5, 0xc0c4d4);
            c.set(5, 4, 0xb0b4c4);
            A.add("pebbles", c);
            for (int v = 0; v < 2; v++) {
                Canvas m(8, 8);
                uint32_t cap = v ? 0x7aa8ff : 0xff6a5a, cap2 = v ? 0x4a78d8 : 0xc43a3a;
                m.rect(3, 4, 2, 4, 0xf2e8d0);
                m.rect(1, 2, 6, 3, cap);
                m.rect(1, 4, 6, 1, cap2);
                m.set(2, 3, 0xffffff);
                m.set(5, 2, 0xffffff);
                m.outline(0x2a1a30);
                A.add(v ? "mush_b" : "mush_r", m);
            }
            Canvas lf(8, 8);
            lf.ellipse(4, 4, 3, 2, 0xe0a040);
            lf.line(1, 6, 7, 2, 0xa06820);
            A.add("leaf", lf);
        }
    }

    // ------------------------------------------------ props
    // Return one of the four fixed shades used by gold-coloured details.
    static uint32_t gold(int i) { static const uint32_t g[4] = {0xfff0a0, 0xf0c030, 0xc08a18, 0x7a5410}; return g[i]; }

    // Create wooden and golden chests in three visual states: closed, opening,
    // and open. The frame suffix is part of the sprite name and is expected by
    // the animation code that selects the appropriate chest image.
    void chests() {
        for (int kind = 0; kind < 2; kind++) {
            uint32_t w1 = kind ? 0xf6d052 : 0x9a6434, w2 = kind ? 0xc99a22 : 0x6e4422, w3 = kind ? 0x8a6414 : 0x4a2c14;
            uint32_t band = kind ? 0xfffaf0 : 0xe6b84a, bandD = kind ? 0xc8c0b0 : 0xa8801c;
            for (int f = 0; f < 3; f++) {
                Canvas c(16, 16);
                // body
                c.rect(2, 8, 12, 7, w2);
                c.rect(2, 8, 12, 2, w1);
                c.rect(2, 14, 12, 1, w3);
                c.rect(4, 8, 2, 7, band);
                c.rect(10, 8, 2, 7, band);
                c.rect(5, 8, 1, 7, bandD);
                c.rect(11, 8, 1, 7, bandD);
                if (f == 0) {
                    c.rect(2, 4, 12, 4, w1);
                    c.rect(3, 3, 10, 1, w1);
                    c.rect(2, 7, 12, 1, w3);
                    c.rect(4, 3, 2, 5, band);
                    c.rect(10, 3, 2, 5, band);
                    c.rect(7, 7, 2, 3, 0xffe27a);
                    c.set(8, 8, w3);
                    if (kind) { c.set(7, 5, 0xff5a7a); c.set(8, 5, 0xff5a7a); c.set(3, 5, 0x7ad0ff); c.set(12, 5, 0x7ad0ff); }
                } else if (f == 1) {
                    c.rect(3, 7, 10, 2, 0xfff6b0);                // light spilling out
                    c.rect(2, 3, 12, 3, w1);
                    c.rect(3, 2, 10, 1, w1);
                    c.rect(2, 5, 12, 1, w3);
                      // lid raised
                    c.rect(4, 2, 2, 4, band);
                    c.rect(10, 2, 2, 4, band);
                } else {
                    c.rect(3, 6, 10, 3, 0xfff6b0);
                    c.rect(4, 7, 8, 1, 0xffffff);
                      // glowing interior
                    c.rect(3, 0, 10, 4, w2);
                    c.rect(3, 0, 10, 1, w1);
                    c.rect(4, 0, 2, 4, band);
                    c.rect(10, 0, 2, 4, band);
                      // lid back
                    c.rect(2, 8, 12, 1, w1);
                }
                c.outline(0x241408);
                A.add(std::string("chest_") + (kind ? "gold" : "wood") + "_" + std::to_string(f), c);
            }
        }
    }

    // Generate collectible items and interactive world objects. Related states
    // use consistent naming so gameplay code can request them without needing
    // to know how each sprite is drawn internally.
    void items() {
        // keys (12x12) in 3 colours
        const uint32_t kc[3][3] = {{0xb8f0a0, 0x6ac84a, 0x3a8a2a}, {0xffc4e8, 0xff7ac0, 0xc04a8a}, {0xfff0a0, 0xf0c030, 0xc08a18}};
        const char* kn[3] = {"key_forest", "key_garden", "key_golden"};
        for (int k = 0; k < 3; k++) {
            Canvas c(12, 12);
            c.disc(3.5f, 3.5f, 3.2f, kc[k][1]);
            c.disc(3.5f, 3.5f, 1.4f, 0);
            c.rect(1, 1, 3, 1, kc[k][0]);
            for (int i = 0; i < 7; i++) c.set(5 + i / 1, 5 + i, 0);
            c.line(5, 5, 10, 10, kc[k][1]);
            c.line(6, 5, 10, 9, kc[k][0]);
            c.line(5, 6, 9, 10, kc[k][2]);
            c.rect(9, 9, 3, 1, kc[k][1]);
            c.rect(7, 8, 2, 1, kc[k][1]);
            c.outline(0x2a1a0a);
            A.add(kn[k], c);
        }
        {   // star shard
            Canvas c(16, 16);
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                float dx = std::abs(x + 0.5f - 8), dy = std::abs(y + 0.5f - 8);
                float d4 = std::pow(dx, 0.6f) + std::pow(dy, 0.6f);
                if (d4 < 3.0f) c.set(x, y, d4 < 1.6f ? 0xffffff : d4 < 2.4f ? 0xfff0a0 : 0xf0c030);
            }
            c.outline(0x8a5a14);
            A.add("shard", c);
        }
        {   // pedestal
            Canvas c(16, 16);
            c.rect(3, 6, 10, 9, 0xb8b0c8);
            c.rect(2, 12, 12, 3, 0x9a92b0);
            c.rect(2, 4, 12, 3, 0xd8d0e8);
            c.rect(3, 6, 2, 8, 0xdcd4ec);
            c.rect(11, 6, 2, 8, 0x8a82a0);
            c.outline(0x2e2640);
            A.add("pedestal", c);
        }
        {   // sign
            Canvas c(16, 16);
            c.rect(7, 8, 2, 8, 0x5a381c);
            c.rect(7, 8, 1, 8, 0x7a5028);
            c.rect(1, 1, 14, 9, 0x9a6a38);
            c.rect(1, 1, 14, 1, 0xc08a4c);
            c.rect(1, 9, 14, 1, 0x5a381c);
            c.rect(3, 4, 10, 1, 0xf0e0b8);
            c.rect(3, 6, 7, 1, 0xf0e0b8);
            c.outline(0x2a1808);
            A.add("sign", c);
        }
        for (int s = 0; s < 2; s++) {   // lever
            Canvas c(16, 16);
            c.rect(2, 11, 12, 4, 0x8a8aa0);
            c.rect(2, 11, 12, 1, 0xb4b4c8);
            c.rect(2, 14, 12, 1, 0x5a5a70);
            c.rect(6, 9, 4, 3, 0x6a6a80);
            if (!s) { c.line(8, 10, 4, 3, 0xc8c8d8); c.line(9, 10, 5, 3, 0x8a8aa0); c.disc(4, 3, 2.2f, 0xe04a4a); c.set(3, 2, 0xff9a9a); }
            else { c.line(8, 10, 12, 3, 0xc8c8d8); c.line(7, 10, 11, 3, 0x8a8aa0); c.disc(12, 3, 2.2f, 0x4ae06a); c.set(11, 2, 0xb0ffc0); }
            c.outline(0x22222e);
            A.add("lever_" + std::to_string(s), c);
        }
        // gates and doors
        for (int o = 0; o < 2; o++) {
            Canvas g(16, 32);
            if (!o) {
                g.rect(0, 0, 3, 32, 0x7a7694);
                g.rect(13, 0, 3, 32, 0x7a7694);
                g.rect(0, 0, 1, 32, 0xa6a2c0);
                g.rect(15, 0, 1, 32, 0x504c68);
                for (int x : {4, 7, 10}) { g.rect(x, 1, 2, 30, 0x3e3a50); g.rect(x, 1, 1, 30, 0x6a6684); g.rect(x - 1, 0, 4, 2, 0x8a86a4); }
                g.rect(3, 6, 10, 2, 0x504c68);
                g.rect(3, 24, 10, 2, 0x504c68);
                g.rect(6, 13, 4, 6, 0xf0c030);
                g.rect(7, 14, 2, 2, 0x7a5410);
                g.rect(6, 13, 4, 1, 0xfff0a0);
            } else {
                g.rect(0, 0, 3, 32, 0x7a7694);
                g.rect(13, 0, 3, 32, 0x7a7694);
                g.rect(0, 0, 1, 32, 0xa6a2c0);
                g.rect(15, 0, 1, 32, 0x504c68);
                g.rect(0, 0, 3, 3, 0x9a96b4);
                g.rect(13, 0, 3, 3, 0x9a96b4);
            }
            g.outline(0x1c1a28);
            A.add(o ? "gate_v_open" : "gate_v_closed", g);
        }
        for (int o = 0; o < 2; o++) {   // single wooden door (16x16)
            Canvas d(16, 16);
            d.rect(0, 0, 16, 16, 0x463d5c);
            if (!o) {
                d.rect(2, 2, 12, 14, 0x7a4a26);
                d.rect(2, 2, 12, 2, 0x9a6a3a);
                for (int x = 5; x < 14; x += 4) d.rect(x, 3, 1, 13, 0x4e2e16);
                d.rect(2, 7, 12, 1, 0x3a3a48);
                d.rect(2, 12, 12, 1, 0x3a3a48);
                d.rect(10, 9, 2, 2, 0xf0c030);
                d.rect(1, 1, 14, 1, 0x9a92b8);
            } else {
                d.rect(2, 2, 12, 14, 0x1a1626);
                d.rect(2, 2, 12, 1, 0x2e2840);
                d.rect(2, 2, 2, 14, 0x5a3a1e);
                d.rect(1, 1, 14, 1, 0x9a92b8);
            }
            A.add(o ? "door_open" : "door_closed", d);
        }
        for (int o = 0; o < 2; o++) {   // golden double door (32x16)
            Canvas d(32, 16);
            d.rect(0, 0, 32, 16, 0x463d5c);
            if (!o) {
                d.rect(2, 1, 28, 15, 0x8a6414);
                d.rect(2, 1, 28, 2, 0xf0c030);
                d.rect(3, 3, 12, 13, 0xc08a18);
                d.rect(17, 3, 12, 13, 0xc08a18);
                d.rect(4, 4, 10, 11, 0xe0aa28);
                d.rect(18, 4, 10, 11, 0xe0aa28);
                for (int i = 0; i < 2; i++) { d.disc(9 + i * 14, 9, 2.4f, 0xfff0a0); d.disc(9 + i * 14, 9, 1.2f, 0xc08a18); }
                d.rect(15, 3, 2, 13, 0x5a3a08);
                d.rect(14, 8, 4, 3, 0xff5a7a);
            } else {
                d.rect(2, 1, 28, 15, 0x1a1626);
                d.rect(2, 1, 28, 2, 0xf0c030);
                d.rect(2, 3, 3, 13, 0xc08a18);
                d.rect(27, 3, 3, 13, 0xc08a18);
            }
            A.add(o ? "door2_open" : "door2_closed", d);
        }
        // glow flowers (puzzle): 4 colours x dim/lit
        const uint32_t pc[4] = {0xff5a6e, 0xffd84a, 0x5ab8ff, 0xff8ee0};
        for (int k = 0; k < 4; k++)
            for (int lit = 0; lit < 2; lit++) {
                Canvas c(16, 24);
                uint32_t col = lit ? pc[k] : mixc(pc[k], 0x606878, 0.6f);
                c.rect(7, 12, 2, 12, 0x3a8a5a);
                c.rect(8, 12, 1, 12, 0x2a6a44);
                c.ellipse(4, 18, 3.5f, 1.6f, 0x3a8a5a);
                c.ellipse(12, 20, 3.5f, 1.6f, 0x2a6a44);
                for (int i = 0; i < 6; i++) { float a = i * PI / 3; c.ellipse(8 + std::cos(a) * 4.2f, 8 + std::sin(a) * 3.8f, 2.8f, 2.8f, col); }
                c.disc(8, 8, 2.8f, lit ? 0xffffff : 0xb8b8c0);
                c.outline(0x16283a);
                A.add("gflower_" + std::to_string(k) + "_" + std::to_string(lit), c);
            }
    }

    // Larger decorative objects use canvases bigger than a standard 16x16 tile.
    // Their dimensions are preserved in the atlas metadata, allowing the
    // renderer to draw them at their intended proportions.
    void bigProps() {
        for (int f = 0; f < 2; f++) {   // fountain 48x40
            Canvas c(48, 40);
            c.ellipse(24, 30, 22, 9, 0x8a86a4);
            c.ellipse(24, 28, 22, 9, 0xb8b4d0);
            c.ellipse(24, 28, 18, 6.5f, 0x4aa8e8);
            c.ellipse(24, 27, 15, 5, 0x7ac8f8);
            c.rect(2, 28, 44, 4, 0x8a86a4);
            c.ellipse(24, 29, 21, 8, 0xb8b4d0);
            c.ellipse(24, 28, 18, 6.5f, 0x4aa8e8);
            c.ellipse(24, 27, 15, 5, 0x7ac8f8);
            c.rect(21, 12, 6, 16, 0xa8a4c4);
            c.rect(21, 12, 2, 16, 0xd4d0ec);
            c.rect(18, 24, 12, 4, 0x8a86a4);
            c.ellipse(24, 12, 7, 3, 0xb8b4d0);
            c.ellipse(24, 11, 5, 2, 0x7ac8f8);
            int off = f * 2;
            for (int i = 0; i < 5; i++) { int h = 4 + i; c.set(24, 10 - h + off, 0xd8f4ff); c.set(24 + (i % 2 ? 1 : -1) * (i / 2 + 1), 11 - i + off, 0xb0e4ff); }
            for (int i = 0; i < 6; i++) { c.set(14 + i * 4, 24 + ((i + f) % 2), 0xe8faff); }
            c.outline(0x2e2a48);
            A.add("fountain_" + std::to_string(f), c);
        }
        {   // birthday cake 64x56
            Canvas c(64, 56);
            c.ellipse(32, 50, 31, 6, 0xd8d0e8);
            c.ellipse(32, 49, 31, 6, 0xf0e8fa);
            auto tier = [&](int cx, int y, int w, int h, uint32_t body, uint32_t dark, uint32_t frost) {
                c.rect(cx - w / 2, y, w, h, body);
                c.rect(cx - w / 2, y, 3, h, shade(body, 1.1f));
                c.rect(cx + w / 2 - 4, y, 4, h, dark);
                c.ellipse(cx, y, w / 2.f, 4, frost);
                for (int x = cx - w / 2 + 2; x < cx + w / 2 - 2; x += 5) { int dl = 2 + (int)(hash2(x, y, 210) % 4); c.rect(x, y, 3, dl, frost); c.disc(x + 1.5f, y + dl, 1.7f, frost); }
            };
            tier(32, 36, 54, 12, 0xffc4dc, 0xe898b8, 0xfffaf4);
            tier(32, 24, 38, 12, 0xfff0b0, 0xe0c878, 0xfffaf4);
            tier(32, 13, 24, 10, 0xc4e8ff, 0x90c0e8, 0xfffaf4);
            for (int x = 8; x < 58; x += 6) { c.disc((float)x, 43, 2.2f, 0xff5a7a); c.set(x - 1, 42, 0xffb0c0); }
            for (int x = 22; x < 42; x += 6) c.disc((float)x, 29, 1.8f, 0xff9ed8);
            for (int i = 0; i < 5; i++) { int x = 21 + i * 5; c.rect(x, 4, 2, 8, i % 2 ? 0xff9ed8 : 0x9ad0ff); c.rect(x, 5, 2, 1, 0xffffff); c.rect(x, 7, 2, 1, 0xffffff); c.rect(x, 9, 2, 1, 0xffffff); }
            c.outline(0x4a2a58);
            A.add("cake", c);
            for (int f = 0; f < 4; f++) {
                Canvas fl(8, 8);
                int w = (f % 2) ? 2 : 3;
                fl.ellipse(4, 4.5f, w, 3, 0xff9a2a);
                fl.ellipse(4, 5, w - 1, 2, 0xffe04a);
                fl.set(4, 6, 0xfff8c0);
                if (f >= 2) fl.set(4 + (f == 2 ? 1 : -1), 1, 0xff9a2a);
                A.add("flame_" + std::to_string(f), fl);
            }
        }
        {   // balloons
            const uint32_t bc[6] = {0xff5a6e, 0xffd84a, 0x5ab8ff, 0xff8ee0, 0x7aeaa0, 0xb88aff};
            for (int i = 0; i < 6; i++) {
                Canvas c(16, 32);
                Canvas m(16, 32);
                m.ellipse(8, 9, 6.5f, 8.2f, 1);
                const uint32_t rp[4] = {mixc(bc[i], 0xffffff, 0.55f), mixc(bc[i], 0xffffff, 0.15f), bc[i], shade(bc[i], 0.7f)};
                shadeMask(c, m, rp, 5, 5, 14, 220 + i);
                c.set(8, 17, shade(bc[i], 0.6f));
                c.set(7, 18, shade(bc[i], 0.6f));
                c.set(9, 18, shade(bc[i], 0.6f));
                for (int y = 19; y < 31; y++) c.set(8 + (int)std::round(std::sin(y * 0.7f) * 1.2f), y, 0xe8e0f0);
                c.outline(shade(bc[i], 0.4f));
                c.rect(4, 4, 2, 3, 0xffffff);
                A.add("balloon_" + std::to_string(i), c);
            }
        }
        {   // throne
            Canvas c(24, 32);
            c.rect(3, 2, 18, 24, 0x8a1c30);
            c.rect(5, 4, 14, 20, 0xb02a44);
            c.rect(3, 2, 18, 2, 0xf0c030);
            c.rect(3, 2, 2, 24, 0xf0c030);
            c.rect(19, 2, 2, 24, 0xc08a18);
            c.rect(1, 18, 22, 8, 0x8a1c30);
            c.rect(1, 18, 22, 2, 0xf0c030);
            c.rect(4, 20, 16, 6, 0xb02a44);
            c.rect(3, 26, 4, 6, 0xc08a18);
            c.rect(17, 26, 4, 6, 0xc08a18);
            c.rect(9, 5, 6, 4, 0xfff0a0);
            c.set(11, 6, 0xff5a7a);
            c.set(12, 6, 0xff5a7a);
            for (int x : {3, 11, 19}) { c.rect(x, 0, 2, 3, 0xf0c030); c.set(x, 0, 0xfff0a0); }
            c.outline(0x2a0c18);
            A.add("throne", c);
        }
    }

    // ------------------------------------------------ characters
    // A character palette describes appearance independently of animation and
    // direction. Optional flags let the same drawing routine handle special
    // silhouettes such as dresses, armour, wings, long hair, and ghosts.
    struct CharPal {
        uint32_t skin, hair, hairD, top, topD, accent, bottom, shoes, hat;
        int hatStyle = 0;
        bool longHair = false, dress = false, wings = false, ghost = false, armor = false;
    };

    // Draw one 16x24 character frame. dir selects the viewing direction and
    // frame controls the small movement changes used for walking animation.
    // The body is assembled in layers: optional wings, legs, torso, head, and
    // hat. Local lambdas keep repeated body parts consistent across directions.
    void human(Canvas& c, const CharPal& p, int dir, int frame) {
        const int yo = (frame % 2 == 1) ? 0 : 1;
        const int liftL = frame == 1 ? 1 : 0, liftR = frame == 3 ? 1 : 0;
        const uint32_t dark = 0x1a1226;
        if (p.wings) {   // behind body
            const int flap = (frame % 2 == 1) ? 1 : 0;
            for (int s = 0; s < 2; s++) {
                int sx = s ? 11 : 4;
                for (int y = 8; y < 18; y++)
                    for (int k = 0; k < 4 - std::abs(y - 12) / 2; k++) {
                        int x = s ? sx + k + 1 : sx - k - 1;
                        c.set(x, y + yo - flap, (k + y) % 3 ? 0xd8b8ff : 0xf0e0ff);
                    }
            }
        }
        auto legsDown = [&]() {
            if (p.ghost) {
                for (int y = 18; y < 24; y++) {
                    int hw = 5 - (y - 18) / 3;
                    for (int x = 8 - hw; x < 8 + hw; x++) {
                        bool wave = ((x + y + frame * 2) % 4 == 0) && y > 21;
                        if (!wave) c.set(x, y + (yo ? 0 : -1), p.top);
                    }
                }
                return;
            }
            // left leg (x 5..6), right leg (x 9..10)
            for (int i = 0; i < 2; i++) {
                int x = i ? 9 : 5, lift = i ? liftR : liftL;
                uint32_t col = p.dress ? p.skin : p.bottom;
                c.rect(x, 18 + yo, 2, 4 - lift - yo + 1, col);
                c.rect(x, 22 - lift, 2, 2, p.shoes);
            }
        };
        // The head helper separates front-facing facial details from the back
        // view, where hair covers the face. Side views are drawn separately below.
        auto head = [&](bool front, bool back) {
            if (back) {
                c.rect(4, 2 + yo, 8, 8, p.hair);
                c.rect(4, 8 + yo, 8, 1, p.hairD);
                if (p.longHair) { c.rect(3, 4 + yo, 2, 8, p.hair); c.rect(11, 4 + yo, 2, 8, p.hair); c.rect(4, 10 + yo, 8, 3, p.hair); c.rect(5, 12 + yo, 6, 1, p.hairD); }
            } else if (front) {
                // Long hair frames the face and falls beside the neck. It must not be
                // drawn across the chest, which previously looked like a beard.
                if (p.longHair) {
                    c.rect(3, 4 + yo, 10, 6, p.hair);
                    c.rect(3, 10 + yo, 2, 3, p.hair);
                    c.rect(11, 10 + yo, 2, 3, p.hair);
                    c.rect(3, 12 + yo, 2, 1, p.hairD);
                    c.rect(11, 12 + yo, 2, 1, p.hairD);
                }
                c.rect(4, 4 + yo, 8, 6, p.skin);
                c.rect(4, 2 + yo, 8, 3, p.hair);
                c.rect(3, 3 + yo, 1, 3, p.hair);
                c.rect(12, 3 + yo, 1, 3, p.hair);
                c.rect(4, 4 + yo, 2, 1, p.hair);
                c.rect(10, 4 + yo, 2, 1, p.hair);
                c.rect(7, 4 + yo, 2, 1, p.hairD);
                if (!p.armor && !p.ghost) {   // glossy hair highlight
                    uint32_t hl = shade(p.hair, 1.65f);
                    c.set(5, 3 + yo, hl); c.set(6, 3 + yo, hl); c.set(10, 3 + yo, shade(p.hair, 1.35f));
                    if (p.longHair) { c.set(3, 6 + yo, hl); c.set(12, 7 + yo, shade(p.hair, 1.35f)); }
                }
                c.set(6, 7 + yo, 0x2a1a2a);
                c.set(9, 7 + yo, 0x2a1a2a);
                c.set(5, 8 + yo, mixc(p.skin, 0xff6a8a, 0.5f));
                c.set(10, 8 + yo, mixc(p.skin, 0xff6a8a, 0.5f));
                if (p.armor) { c.rect(4, 4 + yo, 8, 2, 0x7a8294); c.rect(5, 6 + yo, 6, 1, 0x20242c); }
            }
        };
        // A short neck joins the head to the shoulders. The upper pixel is shaded
        // as if under the chin so the head reads as separate from the torso.
        auto neck = [&](int dirn) {
            if (p.armor || p.ghost) return;
            uint32_t n = shade(p.skin, 0.86f);
            if (dirn == 0) {
                c.set(5, 10 + yo, p.top); c.set(10, 10 + yo, p.top);
                c.set(6, 10 + yo, shade(p.top, 1.2f)); c.set(9, 10 + yo, shade(p.top, 1.2f));
                c.set(7, 10 + yo, n); c.set(8, 10 + yo, n);
            } else if (dirn == 1) {
                c.set(7, 10 + yo, shade(p.skin, 0.8f)); c.set(8, 10 + yo, shade(p.skin, 0.8f));
            } else {
                c.set(7, 10 + yo, n); c.set(8, 10 + yo, n);
            }
        };
        // Hat styles are encoded as small integers in CharPal. Keeping the hat
        // drawing in one place means every direction uses the same accessory.
        auto hat = [&]() {
            switch (p.hatStyle) {
                case 1: c.rect(3, 2 + yo, 10, 2, p.hat);
                triangle(c, 8, 0, 2 + yo, 4, p.hat);
                c.set(8, 1 + yo, 0xfff0a0);
                break;
                case 2: c.rect(4, 1 + yo, 8, 2, 0xf0c030);
                c.set(4, 0 + yo, 0xf0c030);
                c.set(7, 0 + yo, 0xf0c030);
                c.set(8, 0 + yo, 0xf0c030);
                c.set(11, 0 + yo, 0xf0c030);
                c.set(8, 1 + yo, 0xff5a7a);
                break;
                case 3: c.ellipse(8.5f, 2.5f + yo, 5.5f, 2.2f, p.hat);
                c.set(11, 0 + yo, p.hat);
                break;
                case 4: c.rect(4, 2 + yo, 8, 3, 0x9aa2b6);
                c.rect(3, 3 + yo, 1, 3, 0x7a8294);
                c.rect(12, 3 + yo, 1, 3, 0x7a8294);
                        c.rect(6, 0 + yo, 4, 2, 0xe0384a);
                        c.rect(7, -0 + yo, 2, 1, 0xff7a8a);
                        break;
                case 5: for (int x = 4; x < 12; x += 2) { c.set(x, 2 + yo, (x % 4) ? 0xff9ed8 : 0xffffff); c.set(x + 1, 2 + yo, 0x56b85c); } c.set(8, 1 + yo, 0xffd84a); c.set(7, 1 + yo, 0xff9ed8); c.set(9, 1 + yo, 0xff9ed8); break;
                default: break;
            }
        };
        if (dir == 0) {   // front
            if (p.longHair) c.rect(3, 4 + yo, 10, 8, p.hair);
            legsDown();
            c.rect(5, 10 + yo, 6, 4, p.top);
            c.rect(5, 10 + yo, 6, 1, shade(p.top, 1.15f));
            c.rect(5, 13 + yo, 6, 1, p.accent);
            if (p.dress) { for (int y = 14; y < 19; y++) { int hw = 3 + (y - 13) / 2; c.rect(8 - hw, y + yo, hw * 2, 1, (y == 18) ? p.accent : p.bottom); } }
            else c.rect(5, 14 + yo, 6, 4, p.bottom);
            if (p.armor) { c.rect(5, 10 + yo, 6, 4, 0x9aa2b6); c.rect(7, 10 + yo, 2, 4, 0xe0384a); c.rect(4, 10 + yo, 2, 2, 0xb8c0d4); c.rect(10, 10 + yo, 2, 2, 0xb8c0d4); }
            int swing = (frame == 1) ? 1 : (frame == 3 ? -1 : 0);
            c.rect(3, 11 + yo + (swing > 0 ? 1 : 0), 2, 4, p.top);
            c.rect(11, 11 + yo + (swing < 0 ? 1 : 0), 2, 4, p.top);
            c.set(3, 15 + yo + (swing > 0 ? 1 : 0), p.skin);
            c.set(4, 15 + yo + (swing > 0 ? 1 : 0), p.skin);
            c.set(11, 15 + yo + (swing < 0 ? 1 : 0), p.skin);
            c.set(12, 15 + yo + (swing < 0 ? 1 : 0), p.skin);
            head(true, false);
            neck(0);
            hat();
        } else if (dir == 1) {   // back
            legsDown();
            c.rect(5, 10 + yo, 6, 4, p.topD);
            c.rect(5, 13 + yo, 6, 1, p.accent);
            if (p.dress) { for (int y = 14; y < 19; y++) { int hw = 3 + (y - 13) / 2; c.rect(8 - hw, y + yo, hw * 2, 1, (y == 18) ? p.accent : shade(p.bottom, 0.85f)); } }
            else c.rect(5, 14 + yo, 6, 4, shade(p.bottom, 0.85f));
            if (p.armor) { c.rect(5, 10 + yo, 6, 8, 0x8a92a6); c.rect(5, 10 + yo, 6, 2, 0x9aa2b6); }
            c.rect(3, 11 + yo, 2, 4, p.topD);
            c.rect(11, 11 + yo, 2, 4, p.topD);
            head(false, true);
            hat();
        } else {   // side facing right (dir 3); dir 2 = mirrored
            if (p.longHair) { c.rect(4, 4 + yo, 5, 9, p.hair); }
            // legs
            for (int i = 0; i < 2; i++) {
                int spread = (frame == 1) ? (i ? -2 : 2) : (frame == 3 ? (i ? 2 : -2) : 0);
                int x = 6 + spread + (i ? 0 : 0);
                if (p.ghost) continue;
                c.rect(x, 18 + yo, 3, 5 - yo, i ? shade(p.dress ? p.skin : p.bottom, 0.8f) : (p.dress ? p.skin : p.bottom));
                c.rect(x + (i ? 0 : 0), 22, 4, 2, p.shoes);
            }
            if (p.ghost) {
                for (int y = 18; y < 24; y++) { int hw = 3 - (y - 18) / 3; for (int x = 8 - hw - 1; x < 8 + hw + 1; x++) if (!(((x + y + frame * 2) % 4 == 0) && y > 21)) c.set(x, y + (yo ? 0 : -1), p.top); }
            }
            c.rect(5, 10 + yo, 6, 4, p.top);
            c.rect(5, 13 + yo, 6, 1, p.accent);
            if (p.dress) { for (int y = 14; y < 19; y++) { int hw = 3 + (y - 13) / 3; c.rect(8 - hw, y + yo, hw * 2, 1, (y == 18) ? p.accent : p.bottom); } }
            else c.rect(5, 14 + yo, 6, 4, p.bottom);
            if (p.armor) { c.rect(5, 10 + yo, 6, 4, 0x9aa2b6); c.rect(5, 14 + yo, 6, 4, 0x7a8294); }
            int ax = 6 + ((frame == 1) ? 2 : (frame == 3 ? -2 : 0));
            c.rect(ax, 11 + yo, 3, 4, shade(p.top, 1.1f));
            c.set(ax + 1, 15 + yo, p.skin);
            c.set(ax + 2, 15 + yo, p.skin);
            // head profile
            c.rect(5, 4 + yo, 7, 6, p.skin);
            c.rect(4, 2 + yo, 8, 3, p.hair);
            c.rect(4, 4 + yo, 3, 6, p.hair);
            c.rect(4, 8 + yo, 3, 1, p.hairD);
            c.rect(9, 4 + yo, 3, 1, p.hair);
            c.set(10, 7 + yo, 0x2a1a2a);
            c.set(12, 8 + yo, shade(p.skin, 0.88f));
            c.set(10, 8 + yo, mixc(p.skin, 0xff6a8a, 0.5f));
            if (p.armor) { c.rect(5, 4 + yo, 7, 2, 0x7a8294); c.rect(10, 6 + yo, 2, 1, 0x20242c); }
            neck(3);
            hat();
        }
        c.outline(dark);
    }

    // Define the available character palettes, then generate every direction
    // and animation frame for each character. Direction 2 is produced by
    // mirroring the right-facing frame, avoiding a second copy of the drawing
    // logic and keeping left/right details symmetrical.
    void characters() {
        struct Def { const char* name; CharPal pal; };
        std::vector<Def> defs;
        CharPal hero{0xf4cfa8, 0x5a3322, 0x3e2214, 0xb48cf0, 0x8a68c8, 0xf0c030, 0xd4b0ff, 0x6a3a22, 0};
        hero.hatStyle = 5; hero.longHair = true; hero.dress = true; defs.push_back({"hero", hero});
        CharPal steward{0xf0c8a0, 0x7a7a8a, 0x5a5a6a, 0x9aa2b6, 0x7a8294, 0xe0384a, 0x6a7288, 0x3a3a4a, 0};
        steward.hatStyle = 4; steward.armor = true; defs.push_back({"steward", steward});
        CharPal moth{0xf0d0c0, 0xe8e0ff, 0xb8a8e0, 0x7a5ac8, 0x5a3ea0, 0xffd84a, 0x9a7ae0, 0x4a2e78, 0};
        moth.hatStyle = 1; moth.hat = 0x5a3ea0; moth.longHair = true; moth.dress = true; moth.wings = true; defs.push_back({"moth", moth});
        CharPal ghost{0xe8f0ff, 0xc8d4f0, 0xa8b8e0, 0xdce8ff, 0xb8c8ec, 0xff8a9a, 0xdce8ff, 0xdce8ff, 0};
        ghost.hatStyle = 3; ghost.hat = 0xe0384a; ghost.ghost = true; defs.push_back({"ghost", ghost});
        CharPal bard{0xe8b890, 0xc86a2a, 0x8a4418, 0x3a9a7a, 0x2a7a5e, 0xf0c030, 0x3a4a78, 0x5a3322, 0};
        bard.hatStyle = 3; bard.hat = 0x3a9a7a; defs.push_back({"bard", bard});
        for (auto& d : defs) {
            for (int dir = 0; dir < 4; dir++)
                for (int fr = 0; fr < 4; fr++) {
                    Canvas c(16, 24);
                    if (dir == 2) {
                        Canvas r(16, 24);
                        human(r, d.pal, 3, fr);
                        for (int y = 0; y < 24; y++) for (int x = 0; x < 16; x++) { uint32_t p = r.get(15 - x, y); c.set(x, y, p & 0xFFFFFF, (int)(p >> 24)); }
                    } else human(c, d.pal, dir, fr);
                    A.add(Atlas::charName(d.name, dir, fr), c);
                }
        }
        for (int f = 0; f < 2; f++) {   // frog fisher
            Canvas c(16, 16);
            c.ellipse(8, 11, 6, 4.5f, 0x4ab85a);
            c.ellipse(8, 12, 4, 3, 0xcdf0a0);
            c.disc(4.5f, 6, 2.6f, 0x4ab85a);
            c.disc(11.5f, 6, 2.6f, 0x4ab85a);
            c.disc(4.5f, 6, 1.6f, 0xffffff);
            c.disc(11.5f, 6, 1.6f, 0xffffff);
            c.set(5, 6, 0x101010);
            c.set(11, 6, 0x101010);
            c.rect(5, 10 + f, 6, 1, 0x2a6a38);
            c.ellipse(8, 3, 6.5f, 1.8f, 0xe8c060);
            c.ellipse(8, 2, 3.5f, 2.2f, 0xf0d078);
            c.rect(4, 3, 8, 1, 0xc09840);
            c.rect(3, 14, 3, 2, 0x4ab85a);
            c.rect(10, 14, 3, 2, 0x4ab85a);
            c.outline(0x143a20);
            A.add("frog_" + std::to_string(f), c);
        }
    }

    // ------------------------------------------------ particles / icons
    // Particle textures are mostly white masks because the renderer can tint
    // them at draw time. The glow and shadow sprites use alpha gradients, while
    // the small symbols are simple opaque shapes.
    void particles() {
        {   // soft glow
            Canvas c(64, 64);
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                float dx = (x + 0.5f - 32) / 32.f, dy = (y + 0.5f - 32) / 32.f, r = std::sqrt(dx * dx + dy * dy);
                float a = clamp01(1.f - r);
                a = a * a * (3 - 2 * a);
                c.set(x, y, 0xFFFFFF, (int)(a * 255));
            }
            A.glowId_ = A.add("glow", c);
        }
        {
            Canvas c(16, 8);
            for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) {
                float dx = (x + 0.5f - 8) / 8.f, dy = (y + 0.5f - 4) / 4.f, r = std::sqrt(dx * dx + dy * dy);
                if (r < 1) c.set(x, y, 0x0a0614, (int)(clamp01(1.4f - r * 1.2f) * 110));
            }
            A.add("shadow", c);
        }
        {
            Canvas h(8, 8);
            const char* hp[7] = {"..XX.XX.", ".XXXXXXX", ".XXXXXXX", ".XXXXXXX", "..XXXXX.", "...XXX..", "....X..."};
            for (int y = 0; y < 7; y++) for (int x = 0; x < 8; x++) if (hp[y][x] == 'X') h.set(x, y, 0xFFFFFF);
            A.add("p_heart", h);
            Canvas s(8, 8);
            s.set(3, 0, 0xFFFFFF);
            s.set(4, 0, 0xFFFFFF);
            s.rect(3, 1, 2, 6, 0xFFFFFF);
            s.rect(0, 3, 8, 2, 0xFFFFFF);
            s.rect(2, 2, 4, 4, 0xFFFFFF);
            A.add("p_star", s);
            Canvas lf(6, 6);
            lf.ellipse(3, 3, 2.8f, 1.8f, 0xFFFFFF);
            lf.set(0, 5, 0xCCCCCC);
            A.add("p_leaf", lf);
            Canvas pt(5, 5);
            pt.ellipse(2.5f, 2.5f, 2.2f, 1.5f, 0xFFFFFF);
            A.add("p_petal", pt);
            Canvas sp(4, 4);
            sp.rect(1, 0, 2, 4, 0xFFFFFF);
            sp.rect(0, 1, 4, 2, 0xFFFFFF);
            A.add("p_spark", sp);
            Canvas d(3, 3);
            d.rect(0, 0, 3, 3, 0xFFFFFF);
            A.add("p_dot", d);
        }
        {   // UI icons (16x16)
            Canvas p(16, 16);
            p.rect(1, 2, 14, 12, 0xf6eed7);
            p.rect(2, 3, 12, 8, 0x6ab8f0);
            p.rect(2, 8, 12, 3, 0x4a9a4a);
            p.disc(10, 5.5f, 1.5f, 0xfff0a0);
            p.rect(5, 11, 6, 1, 0xd0c6a8);
            p.outline(0x2a2038);
            A.add("icon_photo", p);
            Canvas l(16, 16);
            l.rect(1, 3, 14, 10, 0xf6eed7);
            l.line(1, 3, 8, 9, 0xc8bc9a);
            l.line(15, 3, 8, 9, 0xc8bc9a);
            l.rect(7, 8, 2, 2, 0xe0384a);
            l.outline(0x2a2038);
            A.add("icon_letter", l);
            Canvas k(16, 16);
            k.rect(4, 7, 8, 7, 0xf0c030);
            k.rect(4, 7, 8, 1, 0xfff0a0);
            k.rect(7, 10, 2, 2, 0x7a5410);
            k.rect(5, 3, 2, 4, 0x9a9eb0);
            k.rect(9, 3, 2, 4, 0x9a9eb0);
            k.rect(5, 2, 6, 2, 0x9a9eb0);
            k.outline(0x2a2038);
            A.add("icon_lock", k);
            Canvas q(16, 16);
            q.rect(7, 3, 2, 9, 0x6a5a8a);
            q.rect(6, 12, 4, 2, 0x6a5a8a);
            q.rect(5, 3, 6, 2, 0x6a5a8a);
            q.rect(9, 5, 2, 2, 0x6a5a8a);
            q.rect(7, 7, 3, 2, 0x6a5a8a);
            A.add("icon_question", q);
        }
    }
};

// ============================================================ Atlas
// Atlas methods manage sprite naming, packing, and the final GPU texture.
// ArtGen only creates canvases; these methods decide where each canvas lives.
std::string Atlas::charName(const std::string& set, int dir, int frame) {
    return set + "_" + std::to_string(dir) + "_" + std::to_string(frame);
}
// Copy a completed canvas into the next available position in the atlas sheet.
// Sprites are packed left-to-right. When the current row is full, packing moves
// down by the height of that row. A one-pixel gap is kept between sprites to
// reduce texture bleeding when the GPU samples near sprite boundaries.
int Atlas::add(const std::string& name, const Canvas& c) {
    // The sheet is square, so its width is also the coordinate limit for Y.
    // sheet_ is assigned by build() before any sprite is registered.
    const int SZ = sheet_->w;
    if (penX_ + c.w + 1 > SZ) { penX_ = 1; penY_ += rowH_ + 1; rowH_ = 0; }
    if (penY_ + c.h + 1 > SZ) { Log::error("Sprite atlas full while adding '%s'", name.c_str()); return 0; }
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            const uint8_t* s = &c.px[((size_t)y * c.w + x) * 4];
            uint8_t* d = &sheet_->px[((size_t)(penY_ + y) * SZ + penX_ + x) * 4];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    // Store normalized texture coordinates alongside the original pixel size.
    // The renderer uses UVs to sample the sprite and w/h to preserve its aspect.
    Spr sp;
    sp.u0 = (float)penX_ / SZ;
    sp.v0 = (float)penY_ / SZ;
    sp.u1 = (float)(penX_ + c.w) / SZ;
    sp.v1 = (float)(penY_ + c.h) / SZ;
    sp.w = c.w;
    sp.h = c.h;
    penX_ += c.w + 1;
    rowH_ = std::max(rowH_, c.h);
    sprites_.push_back(sp);
    int id = (int)sprites_.size() - 1;
    names_[name] = id;
    return id;
}
// Resolve a sprite name to its numeric index. Missing names return -1, and a
// warning is emitted only once per missing name to avoid flooding the log.
int Atlas::id(const std::string& name) const {
    auto it = names_.find(name);
    if (it != names_.end()) return it->second;
    if (!warned_[name]) { warned_[name] = true; Log::warn("Unknown sprite '%s'", name.c_str()); }
    return -1;
}
// Build the complete atlas from scratch. The local sheet remains alive until
// the texture upload finishes, so sheet_ is valid throughout every ArtGen call.
// Sprite zero is reserved as a white fallback texel for untextured geometry.
bool Atlas::build() {
    Canvas sheet(1024, 1024);
    sheet_ = &sheet;
    sheet.set(0, 0, 0xFFFFFF, 255);   // white texel for untextured rects
    sprites_.clear();
    names_.clear();
    sprites_.push_back(Spr{0, 0, 1.f / 1024, 1.f / 1024, 1, 1});   // id 0 = fallback (white)
    penX_ = 2;
    penY_ = 1;
    rowH_ = 0;
    // Generation order is intentional: it provides a predictable sprite index
    // order and keeps all categories grouped in the resulting atlas metadata.
    ArtGen g(*this);
    g.grass();
    g.pathAndCarpet();
    g.water();
    g.walls();
    g.trees();
    g.decor();
    g.chests();
    g.items();
    g.bigProps();
    g.characters();
    g.particles();
    bool ok = tex_.create(sheet.px.data(), 1024, 1024, false, false);
    tex_.hasWhite = true;
    tex_.white = vec2(0.5f / 1024, 0.5f / 1024);
    sheet_ = nullptr;
    Log::info("Sprite atlas: %zu sprites, used height %d/1024", sprites_.size(), penY_ + rowH_);
    return ok;
}

}  // namespace bd
