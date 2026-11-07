// Renderer.h - OpenGL 3.3 batched 2D renderer, FreeType text and light-map
// compositing. This header describes the drawing interface used by the rest of the game.
#pragma once
#include <glad/gl.h>

#include "Common.h"

namespace bd {

struct Spr { // a sub-rectangle of a texture (normalised UVs) + pixel size
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int w = 0, h = 0;
};

// A Texture is a small wrapper around an OpenGL texture handle. The ownership flag
// matters for atlas regions or shared textures that should not delete the same GPU object.
struct Texture {
    GLuint id = 0;
    int w = 0, h = 0;
    bool owns = true;
    bool hasWhite = false; // contains a solid white texel (used for untextured rects)
    vec2 white{0, 0};
    bool create(const unsigned char *rgba, int w, int h, bool linear, bool mipmap);
    void destroy();
    bool valid() const {
        return id != 0;
    }
};

enum class Blend { Alpha, Additive, Multiply };
enum class Face { Sans, SansBold, Serif, SerifBold, SerifItalic };
enum class Align { Left, Center, Right };

struct Glyph {
    float u0, v0, u1, v1, w, h, bx, by, adv;
};

// Each Font keeps the glyph metrics and the texture containing their rendered pixels.
// The map is keyed by Unicode codepoint, with a question-mark glyph as a safe fallback.
class Font {
  public:
    Texture tex;
    std::unordered_map<uint32_t, Glyph> glyphs;
    float ascent = 0, height = 0;
    const Glyph *get(uint32_t cp) const {
        auto it = glyphs.find(cp);
        if (it != glyphs.end())
            return &it->second;
        it = glyphs.find('?');
        return it == glyphs.end() ? nullptr : &it->second;
    }
};

// Loads PNG/JPG/etc from disk into a GPU texture (large images are
// box-downscaled to <= maxDim).
bool loadTextureFile(const std::string &path, Texture &out, int maxDim = 2048);
// Makes a graceful placeholder image (checkerboard + frame) so a missing photo
// never crashes the game.
void makePlaceholderTexture(Texture &out, int w, int h);

// Renderer is the one place that talks directly to OpenGL for 2D drawing. Game systems
// describe what they want to draw; this class handles batching, projections, lighting and text.
class Renderer {
  public:
    ~Renderer();
    bool init();
    void shutdown();

    // ---- frame / projection
    void beginFrame(int fbW, int fbH, vec4 clear);
    void setWorld(vec2 camCenter, float zoom); // y-down world projection
    void setScreen();                          // pixel projection, origin top-left, y-down
    vec2 viewSize() const {
        return view_;
    } // size of current projection in its units
    vec2 camCenter() const {
        return cam_;
    }
    float zoom() const {
        return zoom_;
    }
    int fbWidth() const {
        return fbW_;
    }
    int fbHeight() const {
        return fbH_;
    }
    void flush();
    void setBlend(Blend b);

    // ---- drawing
    void draw(const Texture &t, const Spr &s, float x, float y, float w, float h,
              vec4 col = vec4(1), bool flipX = false, float rot = 0.f);
    void drawSprite(const Texture &t, const Spr &s, float x, float y, vec4 col = vec4(1),
                    bool flipX = false) {
        draw(t, s, x, y, (float)s.w, (float)s.h, col, flipX, 0);
    }
    void drawRotated(const Texture &t, const Spr &s, float cx, float cy, float w, float h,
                     float rot, vec4 col);
    void image(const Texture &t, float x, float y, float w, float h, vec4 col = vec4(1));
    void rect(float x, float y, float w, float h, vec4 col);
    void rectV(float x, float y, float w, float h, vec4 top, vec4 bottom);
    void rectOutline(float x, float y, float w, float h, float th, vec4 col);
    void rotRect(float cx, float cy, float w, float h, float rot,
                 vec4 col); // untextured rotated rect
    void setWhiteSource(const Texture *atlas) {
        atlas_ = atlas;
    }

    // ---- light map
    void lightBegin(vec3 ambient);
    void lightEnd();
    bool lightmapActive() const {
        return lightActive_;
    }

    // ---- text (y = top of line box)
    Font &font(Face f, int px);
    float textWidth(Face f, int px, const std::string &s);
    float lineHeight(Face f, int px) {
        return font(f, px).height;
    }
    void text(Face f, int px, const std::string &s, float x, float y, vec4 col,
              Align a = Align::Left, int maxChars = -1);
    void textShadow(Face f, int px, const std::string &s, float x, float y, vec4 col,
                    Align a = Align::Left, int maxChars = -1, vec4 shadow = vec4(0, 0, 0, 0.6f));
    std::vector<std::string> wrap(Face f, int px, const std::string &s, float maxW);

    // ---- misc
    bool screenshot(const std::string &path);
    int drawCalls() const {
        return drawCalls_;
    }

  private:
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };
    void bind(const Texture &t);
    void pushQuad(const Vertex *v4);
    bool buildFont(Font &f, Face face, int px);
    unsigned compile(const std::string &vs, const std::string &fs);

    unsigned prog_ = 0;
    int uVP_ = -1;
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    std::vector<Vertex> verts_;
    const Texture *cur_ = nullptr;
    const Texture *atlas_ = nullptr;
    Blend blend_ = Blend::Alpha;
    glm::mat4 vp_{1.f};
    vec2 view_{1, 1}, cam_{0, 0};
    float zoom_ = 1;
    int fbW_ = 1, fbH_ = 1;
    int drawCalls_ = 0;

    // light map
    GLuint lightFbo_ = 0;
    Texture lightTex_;
    int lightW_ = 0, lightH_ = 0;
    bool lightActive_ = false;

    // fonts
    void *ftLib_ = nullptr;
    void *faces_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    std::unordered_map<int, std::unique_ptr<Font>> fonts_;
};

} // namespace bd
