// Common.h - shared helpers for math, random numbers, logging and file paths.
// This is included by most systems, so keep the common types and small utilities here.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bd {

using glm::ivec2;
using glm::vec2;
using glm::vec3;
using glm::vec4;

constexpr int TILE = 16; // one tile = 16 world pixels
constexpr float PI = 3.14159265358979f;

// Convert a regular 0xRRGGBB colour into the 0-to-1 values GLM expects.
inline vec4 rgba(uint32_t hex, float a = 1.f) {
    return vec4(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}
inline vec4 withAlpha(vec4 c, float a) {
    c.a *= a;
    return c;
}

inline float clamp01(float x) {
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}
inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}
inline float smoothstepf(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
inline float easeOutCubic(float t) {
    t = clamp01(t);
    float u = 1 - t;
    return 1 - u * u * u;
}
inline float easeInOut(float t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}
inline float easeOutBack(float t) {
    t = clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
}
// frame-rate independent exponential smoothing factor
inline float expSmooth(float rate, float dt) {
    return 1.f - std::exp(-rate * dt);
}

// small deterministic RNG (xorshift64*)
// A tiny deterministic random-number generator. Supplying the same seed gives the
// same sequence, which is handy for repeatable map decoration and generated audio.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
    uint32_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float f() {
        return (next() >> 8) * (1.0f / 16777216.0f);
    }
    float range(float a, float b) {
        return a + (b - a) * f();
    }
    int i(int lo, int hi) {
        return lo + (int)(next() % (uint32_t)(hi - lo + 1));
    } // inclusive
    bool chance(float p) {
        return f() < p;
    }
};
inline uint32_t hash2(int x, int y, uint32_t seed = 0) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Basic axis-aligned rectangle used for simple overlap and point-in-box checks.
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool overlaps(const Rect &o) const {
        return x < o.x + o.w && x + w > o.x && y < o.y + o.h && y + h > o.y;
    }
    bool contains(vec2 p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }
};

// ---------------------------------------------------------------- logging
// Shared logging entry points. Warnings are retained so they can be shown in-game.
struct Log {
    static void info(const char *fmt, ...);
    static void warn(const char *fmt,
                     ...); // also kept in warnings() for the in-game report
    static void error(const char *fmt, ...);
    static std::vector<std::string> &warnings();
};

// ---------------------------------------------------------------- files
// General file and string helpers used by config loading, saves and UI text.
namespace util {
bool readFile(const std::string &path, std::string &out);
bool writeFile(const std::string &path,
               const std::string &data); // atomic-ish (tmp + rename)
bool fileExists(const std::string &path);
std::string trim(const std::string &s);
std::string replaceAll(std::string s, const std::string &from, const std::string &to);
std::vector<std::string> splitLines(const std::string &s);
// UTF-8 helpers
uint32_t nextCodepoint(const std::string &s, size_t &i);
size_t utf8Length(const std::string &s);
std::string utf8Substr(const std::string &s,
                       size_t codepoints); // first N codepoints
std::string toLower(std::string s);
} // namespace util

// Locates the game's root folder (the one holding birthday_config.json / assets
// / shaders).
// All game paths are resolved from one root so assets and saves do not depend on
// the directory from which the executable was started.
namespace Paths {
void init(const char *argv0);
const std::string &root();
std::string file(const std::string &relative); // root + "/" + relative (accepts
                                               // absolute paths too)
std::string saves();                           // writable folder for save files
} // namespace Paths

} // namespace bd
