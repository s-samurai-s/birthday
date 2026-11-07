// Atlas.h
// Procedural pixel-art sprite atlas. All sprites are generated in code, so the
// game does not need a separate collection of image files.
#pragma once

#include "Renderer.h"

namespace bd {

// ============================================================ Canvas
// Temporary RGBA drawing surface used while constructing individual sprites.
// Coordinates start at the top-left corner, with X increasing right and Y down.
struct Canvas {
    int w, h;
    std::vector<uint8_t> px; // Pixel data in RGBA order.

    // Allocate a transparent canvas of the requested dimensions.
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 0) {}

    // Check whether a pixel coordinate belongs to this canvas.
    bool in(int x, int y) const {
        return x >= 0 && y >= 0 && x < w && y < h;
    }

    // Set a pixel, optionally with an alpha value from 0 to 255.
    void set(int x, int y, uint32_t rgb, int a = 255);

    // Blend a colour over the existing pixel using the supplied alpha [0, 1].
    void blend(int x, int y, uint32_t rgb, float a);

    // Read a pixel in 0xAARRGGBB format. Invalid coordinates return transparent.
    uint32_t get(int x, int y) const;

    // Read only the alpha channel. Out-of-bounds coordinates are transparent.
    int alpha(int x, int y) const {
        return in(x, y) ? px[((size_t)y * w + x) * 4 + 3] : 0;
    }

    // Draw filled rectangles, circles, ellipses, and straight lines.
    void rect(int x, int y, int rw, int rh, uint32_t rgb, int a = 255);
    void disc(float cx, float cy, float r, uint32_t rgb);
    void ellipse(float cx, float cy, float rx, float ry, uint32_t rgb, int a = 255);
    void line(int x0, int y0, int x1, int y1, uint32_t rgb);

    // Add a one-pixel outline around opaque pixels.
    void outline(uint32_t rgb);

    // Reset every pixel to transparent black.
    void clear() {
        std::fill(px.begin(), px.end(), 0);
    }
};

// ============================================================ Atlas
// Owns the generated sprite metadata and the GPU texture containing all sprites.
class Atlas {
public:
    // Generate every sprite, pack them into one sheet, and upload the texture.
    bool build();

    // Access the texture used by the renderer.
    const Texture& tex() const {
        return tex_;
    }

    // Find a sprite by name. Returns -1 when the name does not exist.
    int id(const std::string& name) const;

    // Access sprite metadata directly by its numeric index.
    const Spr& spr(int id) const {
        return sprites_[id];
    }

    // Find a sprite by name, falling back to sprite zero if it is unknown.
    const Spr& get(const std::string& name) const {
        return sprites_[std::max(0, id(name))];
    }

    // Return the sprite index used for the soft glow texture.
    int glow() const {
        return glowId_;
    }

    // Return the number of registered sprites, including the fallback sprite.
    int count() const {
        return (int)sprites_.size();
    }

    // Character sprite naming convention: <set>_<direction>_<frame>.
    // Direction and frame are both expected to be in the range 0..3.
    static std::string charName(const std::string& set, int dir, int frame);

private:
    // Copy a canvas into the atlas sheet and register its texture coordinates.
    int add(const std::string& name, const Canvas& c);

    Texture tex_;
    std::vector<Spr> sprites_;
    std::unordered_map<std::string, int> names_;

    // Mutable because id() is const but records which missing names were warned.
    mutable std::unordered_map<std::string, bool> warned_;

    // Packing state for the next sprite. penX_/penY_ identify the next position,
    // while rowH_ stores the tallest sprite in the current row.
    Canvas* sheet_ = nullptr;
    int penX_ = 1;
    int penY_ = 1;
    int rowH_ = 0;
    int glowId_ = 0;

    // ArtGen is defined only in Atlas.cpp and needs access to add() and glowId_.
    friend struct ArtGen;
};

} // namespace bd
