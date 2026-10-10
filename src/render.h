#pragma once
#include "gl.h"
#include "util.h"

#include <string>
#include <vector>

struct Col {
    float r = 0, g = 0, b = 0, a = 0;
    Col withA(float na) const { return {r, g, b, na}; }
    Col mul(float k) const { return {r, g, b, a * k}; }
};
inline Col rgb(int r, int g, int b, float a = 1.f) { return {r / 255.f, g / 255.f, b / 255.f, a}; }
Col lerp(const Col& a, const Col& b, float t);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    float r() const { return x + w; }
    float b() const { return y + h; }
};

#include "stb_truetype.h"

// Everything the animated background needs; each game has its own.
struct Backdrop {
    Col top, bottom;          // sky gradient
    Col fog;                  // drifting fog colour
    Col fire;                 // the light source (bonfire / Erdtree glow)
    Col spark;                // embers / motes
    float fireX = 0, fireY = -0.6f, fireAmt = 1;  // position in screen units (y up, -0.5..0.5)
    float sparkAmt = 1, sparkSpeed = 1, fogAmt = 1;
    float embers = 1, ash = 0, motes = 0;  // which particles fill the air: rising embers, falling ash, motes of grace
};

// How a game's screen burns in when you enter it: the edge of the burning sheet.
struct BurnStyle {
    Col rim = rgb(232, 110, 36), rimHot = rgb(255, 214, 140);  // the glowing edge
    float grain = 5;        // how ragged the edge is
    float charWidth = 0.1f; // the charred band before the edge
    float specks = 1;       // sparks smouldering in the char
};
Backdrop mix(const Backdrop& a, const Backdrop& b, float t);

struct Image {
    GLuint tex = 0;
    int w = 0, h = 0;
    explicit operator bool() const { return tex != 0; }
};

struct Font {
    float size = 0, ascent = 0, descent = 0;
    std::vector<stbtt_packedchar> glyphs;  // indexed like Renderer::codepoints_
};

class Renderer {
public:
    bool init(float scale);  // fonts come from the exe's resources
    void begin(int width, int height, float time);
    void end();

    // fonts
    Font body, caption, italic, heading, title, big, button;

    // primitives (pixel coordinates, top-left origin)
    void rect(const Rect& r, Col fill, float radius = 0, float feather = 1);
    void frame(const Rect& r, Col border, float width, float radius = 0, Col fill = {});
    void gradV(const Rect& r, Col top, Col bottom, float radius = 0);
    void gradH(const Rect& r, Col left, Col right);
    void shadow(const Rect& r, float radius, float spread, Col c);
    void glow(float cx, float cy, float radius, Col c);
    void diamond(float cx, float cy, float size, Col c);
    void triangle(float cx, float cy, float size, Col c, bool up);
    Image loadImage(const std::vector<unsigned char>& png);
    // feather: the picture melts into the background over this many pixels from its edge
    void image(const Image& img, const Rect& r, float alpha = 1, float radius = 0, float feather = 1,
               float crop = 0);  // crop: fraction cut from every side of the picture

    float text(const Font& f, float x, float y, const std::string& s, Col c, float tracking = 0);
    float measure(const Font& f, const std::string& s, float tracking = 0) const;
    std::string ellipsize(const Font& f, const std::string& s, float maxW, float tracking = 0) const;
    std::vector<std::string> wrap(const Font& f, const std::string& s, float maxW) const;

    void background(float alpha, const Backdrop& b);
    // a dark sheet over the screen (alpha `sheet`) that burns away from (cx, cy) as burn goes 0..1
    void burnSheet(float cx, float cy, float burn, float sheet, const BurnStyle& s);
    void setClip(const Rect* r);
    void setAlpha(float a) { flush(); alpha_ = a; }
    // applies on top of setAlpha / to every quad: used for the window open/close animation
    void setMasterAlpha(float a) { flush(); master_ = a; }
    void setTransform(float scale, float dy) { flush(); scale_ = scale; dy_ = dy; }
    // Applies to everything drawn after it. amount 0..1.
    // Ash: crumbles to ash from the edges in towards (cx, cy).
    // Reveal: shows only what a burnSheet with the same centre, amount and grain has burnt open.
    enum class Dissolve { None, Ash, Reveal };
    void setDissolve(Dissolve mode, float amount = 0, float cx = 0, float cy = 0, float grain = 5)
    {
        flush();
        disMode_ = mode; dissolve_ = amount; disX_ = cx; disY_ = cy; disGrain_ = grain;
    }

    int width = 0, height = 0;
    float time = 0;

private:
    struct Vtx { float x, y, u, v, r, g, b, a, lx, ly, hw, hh, radius, border, feather, mode, br, bg, bb, ba; };
    std::vector<Vtx> verts_;
    GLuint prog_ = 0, bgProg_ = 0, sheetProg_ = 0, vao_ = 0, bgVao_ = 0, vbo_ = 0, atlas_ = 0;
    int atlasW_ = 0, atlasH_ = 0;
    GLuint curTex_ = 0;
    float alpha_ = 1, master_ = 1, scale_ = 1, dy_ = 0, dissolve_ = 0, disX_ = 0, disY_ = 0, disGrain_ = 5;
    Dissolve disMode_ = Dissolve::None;
    std::vector<int> codepoints_;
    int indexOf(uint32_t cp) const;

    void quad(const Rect& r, Col c, float radius, float border, float feather, float mode, Col bc,
              float u0 = 0, float v0 = 0, float u1 = 0, float v1 = 0);
    void flush();
};
