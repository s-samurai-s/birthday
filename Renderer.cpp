#include "Renderer.h"

#include <array>
#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace bd {

static const int MAX_QUADS = 4096;

// ============================================================ Texture
// Texture owns the OpenGL texture handle and remembers its dimensions. The helper
// functions below keep image loading and fallback creation out of gameplay code.
bool Texture::create(const unsigned char *rgba, int w_, int h_, bool linear, bool mipmap) {
    destroy();
    w = w_;
    h = h_;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    if (mipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    owns = true;
    return id != 0;
}
void Texture::destroy() {
    if (id && owns)
        glDeleteTextures(1, &id);
    id = 0;
    w = h = 0;
    hasWhite = false;
}

// Load an image as RGBA because the sprite renderer expects one consistent format.
// Very large photos are reduced before they reach the GPU, which helps keep memory
// use reasonable when the game displays personal pictures in the memory screens.
bool loadTextureFile(const std::string &path, Texture &out, int maxDim) {
    int w = 0, h = 0, n = 0;
    unsigned char *data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data)
        return false;
    std::vector<unsigned char> small;
    unsigned char *use = data;
    if (w > maxDim || h > maxDim) { // simple box downscale
        float sc = (float)maxDim / (float)std::max(w, h);
        int nw = std::max(1, (int)(w * sc)), nh = std::max(1, (int)(h * sc));
        small.resize((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                int x0 = x * w / nw, x1 = std::max(x0 + 1, (x + 1) * w / nw);
                int y0 = y * h / nh, y1 = std::max(y0 + 1, (y + 1) * h / nh);
                unsigned acc[4] = {0, 0, 0, 0}, cnt = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        const unsigned char *p = data + ((size_t)yy * w + xx) * 4;
                        for (int c = 0; c < 4; c++)
                            acc[c] += p[c];
                        cnt++;
                    }
                for (int c = 0; c < 4; c++)
                    small[((size_t)y * nw + x) * 4 + c] = (unsigned char)(acc[c] / cnt);
            }
        use = small.data();
        w = nw;
        h = nh;
    }
    bool ok = out.create(use, w, h, true, true);
    stbi_image_free(data);
    return ok;
}

void makePlaceholderTexture(Texture &out, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool chk = ((x / 24) + (y / 24)) & 1;
            float g = (float)y / h;
            unsigned char r = (unsigned char)(chk ? 120 + 40 * g : 150 + 40 * g);
            unsigned char b = (unsigned char)(chk ? 170 + 30 * g : 190 + 30 * g);
            unsigned char gg = (unsigned char)(chk ? 110 : 135);
            bool border = x < 6 || y < 6 || x >= w - 6 || y >= h - 6;
            size_t i = ((size_t)y * w + x) * 4;
            px[i] = border ? 250 : r;
            px[i + 1] = border ? 200 : gg;
            px[i + 2] = border ? 90 : b;
            px[i + 3] = 255;
        }
    out.create(px.data(), w, h, true, true);
}

// ============================================================ shaders
// The renderer uses a deliberately small shader pair: the vertex shader transforms
// each sprite, and the fragment shader combines its texture with the sprite tint.
static const char *kVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uVP;
out vec2 vUV; out vec4 vColor;
void main(){ vUV=aUV; vColor=aColor; gl_Position=uVP*vec4(aPos,0.0,1.0); })";
static const char *kFS = R"(#version 330 core
in vec2 vUV; in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main(){ vec4 t=texture(uTex,vUV)*vColor; if(t.a<=0.003) discard; FragColor=t; })";

unsigned Renderer::compile(const std::string &vs, const std::string &fs) {
    auto mk = [](GLenum type, const std::string &src) -> GLuint {
        GLuint s = glCreateShader(type);
        const char *c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, 1024, nullptr, log);
            Log::error("Shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint v = mk(GL_VERTEX_SHADER, vs), f = mk(GL_FRAGMENT_SHADER, fs);
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        Log::error("Shader link error");
        return 0;
    }
    return p;
}

// ============================================================ init
// Set up the shader program, the shared quad buffers and the font system. Sprites are
// batched into one buffer and sent to OpenGL together, rather than issuing a draw call
// for every single tile or particle.
bool Renderer::init() {
    // shaders: prefer shaders/*.vert|frag on disk (so they can be modded), else
    // embedded copy
    std::string vs, fs;
    if (!util::readFile(Paths::file("shaders/sprite.vert"), vs) ||
        !util::readFile(Paths::file("shaders/sprite.frag"), fs)) {
        vs = kVS;
        fs = kFS;
    }
    prog_ = compile(vs, fs);
    if (!prog_)
        prog_ = compile(kVS, kFS);
    if (!prog_)
        return false;
    uVP_ = glGetUniformLocation(prog_, "uVP");
    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * MAX_QUADS * 4, nullptr, GL_STREAM_DRAW);
    std::vector<uint32_t> idx(MAX_QUADS * 6);
    for (int i = 0; i < MAX_QUADS; i++) {
        uint32_t b = i * 4;
        uint32_t *p = &idx[i * 6];
        p[0] = b;
        p[1] = b + 1;
        p[2] = b + 2;
        p[3] = b;
        p[4] = b + 2;
        p[5] = b + 3;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)16);
    verts_.reserve(MAX_QUADS * 4);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    setBlend(Blend::Alpha);

    // ---- FreeType
    FT_Library lib;
    if (FT_Init_FreeType(&lib)) {
        Log::error("FreeType init failed");
        return false;
    }
    ftLib_ = lib;
    static const char *names[5] = {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSerif.ttf",
                                   "DejaVuSerif-Bold.ttf", "DejaVuSerif-Italic.ttf"};
    static const char *sysDirs[] = {"/usr/share/fonts/truetype/dejavu/",
                                    "/usr/share/fonts/dejavu/",
                                    "/usr/share/fonts/TTF/",
                                    "/Library/Fonts/",
                                    "/System/Library/Fonts/Supplemental/",
                                    "C:/Windows/Fonts/"};
    static const char *sysFallbacks[] = {"Arial Unicode.ttf", "Arial.ttf", "arial.ttf",
                                         "segoeui.ttf", "Georgia.ttf"};
    void *firstFace = nullptr;
    for (int i = 0; i < 5; i++) {
        FT_Face face = nullptr;
        std::string path = Paths::file(std::string("assets/fonts/") + names[i]);
        if (FT_New_Face(lib, path.c_str(), 0, &face)) {
            face = nullptr;
            for (auto d : sysDirs) {
                if (!FT_New_Face(lib, (std::string(d) + names[i]).c_str(), 0, &face))
                    break;
                face = nullptr;
            }
        }
        if (!face)
            for (auto d : sysDirs) {
                for (auto fb : sysFallbacks)
                    if (!FT_New_Face(lib, (std::string(d) + fb).c_str(), 0, &face))
                        goto found;
                face = nullptr;
            }
    found:
        if (face) {
            FT_Select_Charmap(face, FT_ENCODING_UNICODE);
            if (!firstFace)
                firstFace = face;
        }
        faces_[i] = face;
    }
    if (!firstFace) {
        Log::error("No usable font found. Put DejaVu*.ttf in assets/fonts/");
        return false;
    }
    for (int i = 0; i < 5; i++)
        if (!faces_[i])
            faces_[i] = firstFace;
    return true;
}

Renderer::~Renderer() {
    shutdown();
}
void Renderer::shutdown() {
    if (!prog_ && !ftLib_)
        return;
    fonts_.clear();
    lightTex_.destroy();
    if (lightFbo_)
        glDeleteFramebuffers(1, &lightFbo_);
    lightFbo_ = 0;
    if (ftLib_) {
        // faces may alias firstFace; FT_Done_FreeType releases every face of the
        // library
        FT_Done_FreeType((FT_Library)ftLib_);
        ftLib_ = nullptr;
    }
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    if (ebo_)
        glDeleteBuffers(1, &ebo_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
    if (prog_)
        glDeleteProgram(prog_);
    prog_ = 0;
    vbo_ = ebo_ = vao_ = 0;
}

// ============================================================ frame
// These functions prepare each frame and switch between world-space and screen-space
// coordinates. UI elements use screen coordinates; the map and characters use world coordinates.
void Renderer::beginFrame(int fbW, int fbH, vec4 clear) {
    fbW_ = std::max(1, fbW);
    fbH_ = std::max(1, fbH);
    drawCalls_ = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbW_, fbH_);
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    setBlend(Blend::Alpha);
    cur_ = nullptr;
    verts_.clear();
    lightActive_ = false;
    setScreen();
}
void Renderer::setWorld(vec2 c, float z) {
    flush();
    cam_ = c;
    zoom_ = z;
    view_ = vec2(fbW_ / z, fbH_ / z);
    vp_ = glm::ortho(c.x - view_.x * 0.5f, c.x + view_.x * 0.5f, c.y + view_.y * 0.5f,
                     c.y - view_.y * 0.5f);
    glUniformMatrix4fv(uVP_, 1, GL_FALSE, glm::value_ptr(vp_));
}
void Renderer::setScreen() {
    flush();
    view_ = vec2((float)fbW_, (float)fbH_);
    zoom_ = 1;
    vp_ = glm::ortho(0.f, (float)fbW_, (float)fbH_, 0.f);
    glUniformMatrix4fv(uVP_, 1, GL_FALSE, glm::value_ptr(vp_));
}
void Renderer::setBlend(Blend b) {
    if (b == blend_) {
        if (b == Blend::Alpha && verts_.empty()) {
        } else
            return;
    }
    flush();
    blend_ = b;
    switch (b) {
    case Blend::Alpha:
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case Blend::Additive:
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case Blend::Multiply:
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        break;
    }
}
void Renderer::flush() {
    if (verts_.empty() || !cur_) {
        verts_.clear();
        return;
    }
    glBindTexture(GL_TEXTURE_2D, cur_->id);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, verts_.size() * sizeof(Vertex), verts_.data());
    glDrawElements(GL_TRIANGLES, (GLsizei)(verts_.size() / 4 * 6), GL_UNSIGNED_INT, nullptr);
    drawCalls_++;
    verts_.clear();
}
void Renderer::bind(const Texture &t) {
    if (cur_ && cur_->id == t.id) {
        cur_ = &t;
        return;
    }
    flush();
    cur_ = &t;
}
void Renderer::pushQuad(const Vertex *v) {
    if (verts_.size() >= (size_t)MAX_QUADS * 4)
        flush();
    verts_.insert(verts_.end(), v, v + 4);
}

// ============================================================ drawing
// All of the drawing helpers eventually add vertices to the same quad batch. The batch
// is flushed when its texture or blend mode changes, or when it reaches its capacity.
void Renderer::draw(const Texture &t, const Spr &s, float x, float y, float w, float h, vec4 c,
                    bool flipX, float rot) {
    bind(t);
    float u0 = flipX ? s.u1 : s.u0, u1 = flipX ? s.u0 : s.u1;
    Vertex v[4];
    if (rot == 0.f) {
        v[0] = {x, y, u0, s.v0, c.r, c.g, c.b, c.a};
        v[1] = {x + w, y, u1, s.v0, c.r, c.g, c.b, c.a};
        v[2] = {x + w, y + h, u1, s.v1, c.r, c.g, c.b, c.a};
        v[3] = {x, y + h, u0, s.v1, c.r, c.g, c.b, c.a};
    } else {
        float cx = x + w * 0.5f, cy = y + h * 0.5f, cs = std::cos(rot), sn = std::sin(rot);
        float hx[4] = {-w / 2, w / 2, w / 2, -w / 2}, hy[4] = {-h / 2, -h / 2, h / 2, h / 2};
        float uu[4] = {u0, u1, u1, u0}, vv[4] = {s.v0, s.v0, s.v1, s.v1};
        for (int i = 0; i < 4; i++)
            v[i] = {cx + hx[i] * cs - hy[i] * sn,
                    cy + hx[i] * sn + hy[i] * cs,
                    uu[i],
                    vv[i],
                    c.r,
                    c.g,
                    c.b,
                    c.a};
    }
    pushQuad(v);
}
void Renderer::drawRotated(const Texture &t, const Spr &s, float cx, float cy, float w, float h,
                           float rot, vec4 c) {
    draw(t, s, cx - w / 2, cy - h / 2, w, h, c, false, rot);
}
void Renderer::image(const Texture &t, float x, float y, float w, float h, vec4 c) {
    Spr s;
    s.w = t.w;
    s.h = t.h;
    draw(t, s, x, y, w, h, c);
}
void Renderer::rect(float x, float y, float w, float h, vec4 c) {
    const Texture *t = (cur_ && cur_->hasWhite) ? cur_ : atlas_;
    if (!t)
        return;
    bind(*t);
    float u = t->white.x, v = t->white.y;
    Vertex q[4] = {{x, y, u, v, c.r, c.g, c.b, c.a},
                   {x + w, y, u, v, c.r, c.g, c.b, c.a},
                   {x + w, y + h, u, v, c.r, c.g, c.b, c.a},
                   {x, y + h, u, v, c.r, c.g, c.b, c.a}};
    pushQuad(q);
}
void Renderer::rectV(float x, float y, float w, float h, vec4 a, vec4 b) {
    const Texture *t = (cur_ && cur_->hasWhite) ? cur_ : atlas_;
    if (!t)
        return;
    bind(*t);
    float u = t->white.x, v = t->white.y;
    Vertex q[4] = {{x, y, u, v, a.r, a.g, a.b, a.a},
                   {x + w, y, u, v, a.r, a.g, a.b, a.a},
                   {x + w, y + h, u, v, b.r, b.g, b.b, b.a},
                   {x, y + h, u, v, b.r, b.g, b.b, b.a}};
    pushQuad(q);
}
void Renderer::rectOutline(float x, float y, float w, float h, float th, vec4 c) {
    rect(x, y, w, th, c);
    rect(x, y + h - th, w, th, c);
    rect(x, y + th, th, h - 2 * th, c);
    rect(x + w - th, y + th, th, h - 2 * th, c);
}
void Renderer::rotRect(float cx, float cy, float w, float h, float rot, vec4 c) {
    const Texture *t = (cur_ && cur_->hasWhite) ? cur_ : atlas_;
    if (!t)
        return;
    bind(*t);
    float u = t->white.x, v = t->white.y, cs = std::cos(rot), sn = std::sin(rot);
    float hx[4] = {-w / 2, w / 2, w / 2, -w / 2}, hy[4] = {-h / 2, -h / 2, h / 2, h / 2};
    Vertex q[4];
    for (int i = 0; i < 4; i++)
        q[i] = {
            cx + hx[i] * cs - hy[i] * sn, cy + hx[i] * sn + hy[i] * cs, u, v, c.r, c.g, c.b, c.a};
    pushQuad(q);
}

// ============================================================ light map
// Lights are drawn into a separate texture and composited over the scene. This lets
// torches, the companion and glowing objects brighten nearby pixels without changing
// the colour of the original sprites.
void Renderer::lightBegin(vec3 ambient) {
    flush();
    int lw = std::max(8, (int)std::ceil(view_.x * 0.5f)),
        lh = std::max(8, (int)std::ceil(view_.y * 0.5f));
    if (!lightFbo_ || lw != lightW_ || lh != lightH_) {
        lightTex_.destroy();
        lightTex_.create(nullptr, lw, lh, true, false);
        if (!lightFbo_)
            glGenFramebuffers(1, &lightFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lightTex_.id,
                               0);
        lightW_ = lw;
        lightH_ = lh;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glViewport(0, 0, lightW_, lightH_);
    glClearColor(ambient.r, ambient.g, ambient.b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    lightActive_ = true;
    setBlend(Blend::Additive);
}
void Renderer::lightEnd() {
    if (!lightActive_)
        return;
    flush();
    lightActive_ = false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbW_, fbH_);
    setScreen();
    setBlend(Blend::Multiply);
    Spr s;
    s.u0 = 0;
    s.u1 = 1;
    s.v0 = 1;
    s.v1 = 0;
    s.w = lightW_;
    s.h = lightH_; // FBO is y-flipped
    lightTex_.owns = true;
    draw(lightTex_, s, 0, 0, (float)fbW_, (float)fbH_, vec4(1));
    flush();
    setBlend(Blend::Alpha);
}

// ============================================================ fonts
// FreeType rasterizes the characters into a texture atlas. Once a font is built, text
// is rendered with the same quad batch as sprites, so dialogue and UI stay efficient.
bool Renderer::buildFont(Font &f, Face face, int px) {
    FT_Face ft = (FT_Face)faces_[(int)face];
    if (!ft)
        return false;
    FT_Set_Pixel_Sizes(ft, 0, px);
    f.ascent = ft->size->metrics.ascender / 64.f;
    f.height = ft->size->metrics.height / 64.f;

    std::vector<uint32_t> cps;
    for (uint32_t c = 32; c < 127; c++)
        cps.push_back(c);
    for (uint32_t c = 160; c < 256; c++)
        cps.push_back(c);
    for (uint32_t c : {0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2026u, 0x2022u,
                       0x2665u, 0x2605u, 0x2661u, 0x2606u, 0x263Au, 0x20ACu, 0x0152u, 0x0153u,
                       0x0141u, 0x0142u, 0x2192u, 0x2190u, 0x2191u, 0x2193u})
        cps.push_back(c);

    struct Bm {
        uint32_t cp;
        int w, h, bx, by;
        float adv;
        std::vector<unsigned char> d;
    };
    std::vector<Bm> bms;
    for (uint32_t cp : cps) {
        if (FT_Load_Char(ft, cp, FT_LOAD_RENDER))
            continue;
        FT_GlyphSlot g = ft->glyph;
        Bm b;
        b.cp = cp;
        b.w = g->bitmap.width;
        b.h = g->bitmap.rows;
        b.bx = g->bitmap_left;
        b.by = g->bitmap_top;
        b.adv = g->advance.x / 64.f;
        b.d.assign(g->bitmap.buffer,
                   g->bitmap.buffer + (size_t)std::max(0, b.w) * std::max(0, b.h));
        if (g->bitmap.pitch != b.w && b.w > 0) { // repack rows
            for (int y = 0; y < b.h; y++)
                for (int x = 0; x < b.w; x++)
                    b.d[(size_t)y * b.w + x] = g->bitmap.buffer[(size_t)y * g->bitmap.pitch + x];
        }
        bms.push_back(std::move(b));
    }
    int dim = 128;
    std::vector<std::array<int, 2>> pos;
    for (;; dim *= 2) {
        pos.assign(bms.size(), {0, 0});
        int x = 3, y = 3, rowH = 0;
        bool ok = true;
        for (size_t i = 0; i < bms.size(); i++) {
            if (x + bms[i].w + 1 > dim) {
                x = 1;
                y += rowH + 1;
                rowH = 0;
            }
            if (y + bms[i].h + 1 > dim) {
                ok = false;
                break;
            }
            pos[i] = {x, y};
            x += bms[i].w + 1;
            rowH = std::max(rowH, bms[i].h);
        }
        if (ok || dim >= 4096)
            break;
    }
    std::vector<unsigned char> atlas((size_t)dim * dim * 4, 0);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            for (int c = 0; c < 4; c++)
                atlas[((size_t)y * dim + x) * 4 + c] = 255; // white block
    for (size_t i = 0; i < bms.size(); i++) {
        const Bm &b = bms[i];
        for (int y = 0; y < b.h; y++)
            for (int x = 0; x < b.w; x++) {
                size_t o = ((size_t)(pos[i][1] + y) * dim + pos[i][0] + x) * 4;
                atlas[o] = atlas[o + 1] = atlas[o + 2] = 255;
                atlas[o + 3] = b.d[(size_t)y * b.w + x];
            }
        Glyph g;
        g.u0 = (float)pos[i][0] / dim;
        g.v0 = (float)pos[i][1] / dim;
        g.u1 = (float)(pos[i][0] + b.w) / dim;
        g.v1 = (float)(pos[i][1] + b.h) / dim;
        g.w = (float)b.w;
        g.h = (float)b.h;
        g.bx = (float)b.bx;
        g.by = (float)b.by;
        g.adv = b.adv;
        f.glyphs[b.cp] = g;
    }
    f.tex.create(atlas.data(), dim, dim, true, false);
    f.tex.hasWhite = true;
    f.tex.white = vec2(1.0f / dim, 1.0f / dim);
    return true;
}
Font &Renderer::font(Face face, int px) {
    px = std::max(6, std::min(px, 200));
    int key = (int)face * 1000 + px;
    auto it = fonts_.find(key);
    if (it != fonts_.end())
        return *it->second;
    auto f = std::make_unique<Font>();
    if (!buildFont(*f, face, px))
        Log::error("Font build failed (face %d, %dpx)", (int)face, px);
    Font &ref = *f;
    fonts_[key] = std::move(f);
    return ref;
}
float Renderer::textWidth(Face face, int px, const std::string &s) {
    Font &f = font(face, px);
    float w = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = util::nextCodepoint(s, i);
        if (cp == '\n')
            continue;
        if (const Glyph *g = f.get(cp))
            w += g->adv;
    }
    return w;
}
void Renderer::text(Face face, int px, const std::string &s, float x, float y, vec4 col, Align a,
                    int maxChars) {
    Font &f = font(face, px);
    if (!f.tex.valid())
        return;
    if (a != Align::Left) {
        float w = textWidth(face, px, s);
        x -= (a == Align::Center) ? w * 0.5f : w;
    }
    bind(f.tex);
    float base = y + f.ascent, pen = x;
    size_t i = 0;
    int count = 0;
    while (i < s.size()) {
        if (maxChars >= 0 && count >= maxChars)
            break;
        uint32_t cp = util::nextCodepoint(s, i);
        count++;
        if (cp == '\n')
            continue;
        const Glyph *g = f.get(cp);
        if (!g)
            continue;
        if (g->w > 0) {
            float gx = std::floor(pen + g->bx + 0.5f), gy = std::floor(base - g->by + 0.5f);
            Spr sp;
            sp.u0 = g->u0;
            sp.v0 = g->v0;
            sp.u1 = g->u1;
            sp.v1 = g->v1;
            Vertex v[4] = {{gx, gy, g->u0, g->v0, col.r, col.g, col.b, col.a},
                           {gx + g->w, gy, g->u1, g->v0, col.r, col.g, col.b, col.a},
                           {gx + g->w, gy + g->h, g->u1, g->v1, col.r, col.g, col.b, col.a},
                           {gx, gy + g->h, g->u0, g->v1, col.r, col.g, col.b, col.a}};
            pushQuad(v);
        }
        pen += g->adv;
    }
}
void Renderer::textShadow(Face face, int px, const std::string &s, float x, float y, vec4 col,
                          Align a, int maxChars, vec4 sh) {
    float o = std::max(1.f, px / 14.f);
    sh.a *= col.a;
    text(face, px, s, x + o, y + o, sh, a, maxChars);
    text(face, px, s, x, y, col, a, maxChars);
}
std::vector<std::string> Renderer::wrap(Face face, int px, const std::string &s, float maxW) {
    std::vector<std::string> out;
    for (const std::string &para : util::splitLines(s)) {
        if (para.empty()) {
            out.push_back("");
            continue;
        }
        std::string line, word;
        auto pushWord = [&]() {
            if (word.empty())
                return;
            std::string t = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(face, px, t) > maxW) {
                out.push_back(line);
                line = word;
            } else
                line = t;
            word.clear();
        };
        for (char c : para) {
            if (c == ' ')
                pushWord();
            else
                word += c;
        }
        pushWord();
        out.push_back(line);
    }
    return out;
}

bool Renderer::screenshot(const std::string &path) {
    flush();
    std::vector<unsigned char> px((size_t)fbW_ * fbH_ * 4), flipped((size_t)fbW_ * fbH_ * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW_, fbH_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < fbH_; y++)
        memcpy(&flipped[(size_t)y * fbW_ * 4], &px[(size_t)(fbH_ - 1 - y) * fbW_ * 4],
               (size_t)fbW_ * 4);
    for (size_t i = 3; i < flipped.size(); i += 4)
        flipped[i] = 255;
    return stbi_write_png(path.c_str(), fbW_, fbH_, 4, flipped.data(), fbW_ * 4) != 0;
}

} // namespace bd
