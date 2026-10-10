#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "stb_image.h"

Col lerp(const Col& a, const Col& b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Backdrop mix(const Backdrop& a, const Backdrop& b, float t)
{
    auto f = [t](float x, float y) { return x + (y - x) * t; };
    return {lerp(a.top, b.top, t), lerp(a.bottom, b.bottom, t), lerp(a.fog, b.fog, t), lerp(a.fire, b.fire, t),
            lerp(a.spark, b.spark, t), f(a.fireX, b.fireX), f(a.fireY, b.fireY), f(a.fireAmt, b.fireAmt),
            f(a.sparkAmt, b.sparkAmt), f(a.sparkSpeed, b.sparkSpeed), f(a.fogAmt, b.fogAmt),
            f(a.embers, b.embers),     f(a.ash, b.ash),               f(a.motes, b.motes)};
}

// ---------------------------------------------------------------- shaders

static const char* kVS = R"(#version 330 core
layout(location=0) in vec2 aPos;  layout(location=1) in vec2 aUV;   layout(location=2) in vec4 aCol;
layout(location=3) in vec2 aLocal; layout(location=4) in vec2 aHalf; layout(location=5) in vec4 aP;
layout(location=6) in vec4 aBCol;
uniform vec2 uScreen;
uniform vec2 uXform;  // x: scale around the screen centre, y: vertical offset (window open/close)
out vec2 vUV; out vec4 vCol; out vec2 vLocal; out vec2 vHalf; out vec4 vP; out vec4 vBCol;
void main() {
    vUV = aUV; vCol = aCol; vLocal = aLocal; vHalf = aHalf; vP = aP; vBCol = aBCol;
    vec2 c = uScreen * 0.5;
    vec2 p = (aPos - c) * uXform.x + c + vec2(0.0, uXform.y);
    gl_Position = vec4(p.x / uScreen.x * 2.0 - 1.0, 1.0 - p.y / uScreen.y * 2.0, 0.0, 1.0);
})";

// Noise and the burn front, shared by the UI shader and the burning sheet so both see the same edge.
#define BURN_COMMON R"(
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) { v += a * noise(p); p = p * 2.03 + 17.1; a *= 0.5; }
    return v;
}
// < 0: still there, > 0: gone. px in pixels, top-left origin. inward: the edges go first (crumbling to ash),
// else the middle goes first (a sheet catching from a spark at center).
float burnFront(vec2 px, vec2 screen, vec2 center, float amount, float grain, bool inward) {
    vec2 p = (px - center) / screen.y;
    float dn = length(p) / (length(max(center, screen - center)) / screen.y);
    float b = (inward ? (1.0 - dn) * 0.8 : dn * 0.75) + (fbm(p * grain + 3.7) - 0.5) * 0.55
            + (fbm(p * grain * 2.8) - 0.5) * 0.12;
    return amount * 1.5 - 0.35 - b;
}
)"

// modes: 0 rounded rect (+border), 1 glyph, 2 image (edges feathered inward), 3 vertical gradient, 4 radial glow, 5 diamond,
//        6 horizontal gradient, 7/8 triangle pointing up/down
static const char* kFS = R"(#version 330 core
in vec2 vUV; in vec4 vCol; in vec2 vLocal; in vec2 vHalf; in vec4 vP; in vec4 vBCol;
uniform sampler2D uTex; uniform float uAlpha;
uniform vec2 uScreen;
uniform int uDisMode;  // Renderer::Dissolve
uniform float uDissolve, uDisGrain; uniform vec2 uDisCenter;
out vec4 frag;
)" BURN_COMMON R"(
vec4 crumble(vec4 c) {
    vec2 px = vec2(gl_FragCoord.x, uScreen.y - gl_FragCoord.y);
    float e = burnFront(px, uScreen, uDisCenter, uDissolve, uDisGrain, uDisMode == 1);
    if (uDisMode == 2) return vec4(c.rgb, c.a * smoothstep(-0.004, 0.004, e));  // its edge is drawn by burnSheet
    float keep = 1.0 - smoothstep(-0.004, 0.004, e);
    float crust = smoothstep(-0.05, -0.004, e);
    vec3 ash = vec3(0.30, 0.28, 0.26);
    c.rgb = mix(c.rgb, ash * 0.5, crust * 0.6);                         // grey crust before it goes
    c.rgb = mix(c.rgb, vec3(0.62, 0.58, 0.53), exp(-abs(e) * 90.0) * 0.8);  // pale ash at the very edge
    return vec4(c.rgb, c.a * keep);
}
float sdRound(vec2 p, vec2 b, float r) {
    r = min(r, min(b.x, b.y));
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    int mode = int(vP.w + 0.5);
    vec4 c;
    if (mode == 1) {
        c = vec4(vCol.rgb, vCol.a * texture(uTex, vUV).r);
    } else if (mode == 2) {
        vec4 tx = texture(uTex, vUV);
        float d = sdRound(vLocal, vHalf, vP.x);
        float a = 1.0 - smoothstep(-max(vP.z, 1.0), 0.0, d);
        c = vec4(tx.rgb * vCol.rgb, tx.a * vCol.a * a);
    } else if (mode == 4) {
        float rr = length(vLocal) / max(vHalf.x, 1.0);
        c = vec4(vCol.rgb, vCol.a * exp(-rr * rr * 4.0));
    } else if (mode == 7 || mode == 8) {
        vec2 p = vec2(vLocal.x, mode == 7 ? -vLocal.y : vLocal.y);
        float r = vHalf.x, k = sqrt(3.0);
        p.y += r * 0.3;
        p.x = abs(p.x) - r; p.y = p.y + r / k;
        if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
        p.x -= clamp(p.x, -2.0 * r, 0.0);
        float d = -length(p) * sign(p.y);
        c = vec4(vCol.rgb, vCol.a * (1.0 - smoothstep(-0.6, 0.6, d)));
    } else if (mode == 5) {
        float d = (abs(vLocal.x) + abs(vLocal.y) - vHalf.x) * 0.70710678;
        c = vec4(vCol.rgb, vCol.a * (1.0 - smoothstep(-0.6, 0.6, d)));
    } else {
        float d = sdRound(vLocal, vHalf, vP.x);
        float f = max(vP.z, 1.0);
        float a = 1.0 - smoothstep(-f * 0.5, f * 0.5, d);
        vec4 fill = vCol;
        if (mode == 3) fill = mix(vCol, vBCol, clamp(vLocal.y / vHalf.y * 0.5 + 0.5, 0.0, 1.0));
        else if (mode == 6) fill = mix(vCol, vBCol, clamp(vLocal.x / vHalf.x * 0.5 + 0.5, 0.0, 1.0));
        else if (vP.y > 0.0) fill = mix(vCol, vBCol, smoothstep(-vP.y - 0.75, -vP.y + 0.25, d));
        c = vec4(fill.rgb, fill.a * a);
    }
    if (uDisMode != 0) c = crumble(c);
    frag = vec4(c.rgb, c.a * uAlpha);
})";

static const char* kBgVS = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// Ash and fog, a dying bonfire below; embers rising, ash falling or motes of grace hanging in the air.
static const char* kBgFS = R"(#version 330 core
uniform vec2 uRes; uniform float uTime; uniform float uAlpha;
uniform vec3 uTop, uBottom, uFog, uFire, uSpark;
uniform vec2 uFirePos;
uniform float uFireAmt, uSparkAmt, uSparkSpeed, uFogAmt;
uniform float uEmbers, uAsh, uMotes;
out vec4 frag;
)" BURN_COMMON R"(
void main() {
    vec2 uv = gl_FragCoord.xy / uRes;
    vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
    float t = uTime;

    vec3 col = mix(uBottom, uTop, uv.y);

    // slow layered fog
    float f1 = fbm(p * 1.6 + vec2(t * 0.012, t * 0.004));
    float f2 = fbm(p * 3.4 - vec2(t * 0.018, -t * 0.007) + f1 * 1.5);
    col += uFog * uFogAmt * smoothstep(0.40, 0.95, f2) * (0.45 + 0.55 * uv.y);
    col += uFog * uFogAmt * 0.4 * smoothstep(0.5, 0.8, f1);

    // the bonfire: low, warm, breathing
    float flick = 0.82 + 0.12 * noise(vec2(t * 2.7, 1.0)) + 0.06 * noise(vec2(t * 9.0, 5.0));
    vec2 g = p - uFirePos;
    col += uFire * exp(-dot(g * vec2(0.75, 1.5), g * vec2(0.75, 1.5)) * 2.6) * 0.30 * uFireAmt * flick;
    col += mix(uFire, vec3(1.0), 0.25) * exp(-dot(g, g) * 26.0) * 0.14 * uFireAmt * flick;

    // embers rising from the fire
    if (uEmbers > 0.01)
    for (int i = 0; i < 56; i++) {
        float fi = float(i);
        float speed = (0.028 + 0.045 * hash(vec2(fi, 1.3))) * uSparkSpeed;
        float life = fract(t * speed + hash(vec2(fi, 7.1)));
        float drift = (hash(vec2(fi, 9.2)) - 0.5);
        vec2 e = vec2((hash(vec2(fi, 3.7)) - 0.5) * 0.85 + drift * life * life * 0.5
                      + sin(t * 0.7 + fi * 1.7) * 0.025 * life,
                      -0.62 + life * 1.3);
        float d = length(p - e);
        float size = 0.0016 + 0.0022 * hash(vec2(fi, 5.5));
        float fade = smoothstep(0.0, 0.08, life) * (1.0 - life) * (1.0 - life);
        float tw = 0.55 + 0.45 * sin(t * (6.0 + 5.0 * hash(vec2(fi, 2.2))) + fi * 3.1);
        float e1 = size * size / (d * d + size * size * 0.35);
        col += uSpark * fade * tw * e1 * 0.55 * uSparkAmt * uEmbers;
    }

    // ash coming down on a slow wind, flakes tumbling; a few still smoulder at the edge
    if (uAsh > 0.01)
    for (int i = 0; i < 44; i++) {
        float fi = float(i);
        float speed = (0.016 + 0.026 * hash(vec2(fi, 2.9))) * uSparkSpeed;
        float life = fract(t * speed + hash(vec2(fi, 4.4)));
        vec2 e = vec2((hash(vec2(fi, 6.1)) - 0.5) * 1.9 - 0.15 + life * 0.35 + sin(t * 0.6 + fi * 2.3) * 0.03,
                      0.6 - life * 1.25);
        vec2 q = p - e;
        float ang = t * (0.6 + 1.4 * hash(vec2(fi, 8.8))) + fi;
        q = mat2(cos(ang), -sin(ang), sin(ang), cos(ang)) * q;
        q.x /= 0.2 + 0.8 * abs(sin(t * (0.8 + hash(vec2(fi, 3.3))) + fi));  // edge-on, then flat
        float size = 0.0042 + 0.0048 * hash(vec2(fi, 5.9));
        float flake = 1.0 - smoothstep(size * 0.45, size, length(q * vec2(1.0, 1.5)));
        float fade = smoothstep(0.0, 0.1, life) * (1.0 - smoothstep(0.8, 1.0, life));
        float smoulder = step(0.82, hash(vec2(fi, 7.7)));
        vec3 ashCol = mix(vec3(0.40, 0.38, 0.36), uSpark, smoulder * 0.8);
        col += ashCol * flake * fade * 0.75 * uSparkAmt * uAsh;
    }

    // motes of grace: hanging in the air, slowly rising, breathing light
    if (uMotes > 0.01)
    for (int i = 0; i < 38; i++) {
        float fi = float(i);
        float speed = (0.010 + 0.016 * hash(vec2(fi, 1.9))) * uSparkSpeed;
        float life = fract(t * speed + hash(vec2(fi, 3.1)));
        vec2 e = vec2((hash(vec2(fi, 5.3)) - 0.5) * 1.7 + sin(t * 0.35 + fi * 1.3) * 0.06,
                      (hash(vec2(fi, 6.7)) - 0.5) * 0.9 + (life - 0.5) * 0.35 + cos(t * 0.27 + fi) * 0.03);
        float d = length(p - e);
        float size = 0.0018 + 0.0026 * hash(vec2(fi, 8.1));
        float fade = sin(3.14159 * life);
        fade *= fade;
        float pulse = 0.55 + 0.45 * sin(t * (1.1 + hash(vec2(fi, 9.9))) + fi * 2.0);
        float core = size * size / (d * d + size * size * 0.3);
        float halo = exp(-d * d / (size * size * 60.0)) * 0.35;
        col += uSpark * (core * 0.8 + halo) * fade * pulse * uSparkAmt * uMotes;
    }

    // vignette and grain
    float v = smoothstep(1.30, 0.20, length(p * vec2(0.80, 1.0)));
    col *= mix(0.30, 1.0, v);
    col += (hash(gl_FragCoord.xy + fract(t * 7.0) * 113.0) - 0.5) * 0.010;
    frag = vec4(col * uAlpha, 1.0);
})";

// The dark the old screen sank into, burning away like a sheet of paper from where the ember was.
static const char* kSheetFS = R"(#version 330 core
uniform vec2 uRes; uniform float uTime;
uniform vec2 uCenter;  // pixels, top-left origin
uniform float uBurn, uSheet, uGrain, uChar, uSpecks;
uniform vec3 uRim, uRimHot;
out vec4 frag;
)" BURN_COMMON R"(
void main() {
    vec2 px = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y);
    float e = burnFront(px, uRes, uCenter, uBurn, uGrain, false);
    float keep = 1.0 - smoothstep(-0.004, 0.004, e);
    float charred = smoothstep(-uChar, -0.004, e) * keep;

    vec3 sheet = vec3(0.010, 0.009, 0.008) + vec3(0.030, 0.025, 0.021) * fbm(px / uRes.y * 7.0);  // faint fibre
    vec3 col = mix(sheet, mix(vec3(0.05, 0.025, 0.012), uRim * 0.35, charred * charred), charred);
    float A = keep * mix(uSheet, 1.0, charred * 0.9);
    float speck = smoothstep(0.8, 0.95, noise(gl_FragCoord.xy * 0.22 + uTime * 0.6)) * charred * uSpecks;
    col = mix(col, uRimHot, clamp(speck, 0.0, 1.0));

    float rim = exp(-abs(e) * 70.0) * step(0.001, uBurn);
    vec3 rimCol = mix(uRim, uRimHot, exp(-abs(e) * 160.0));
    rimCol = mix(rimCol, vec3(1.0, 0.96, 0.86), exp(-abs(e) * 300.0) * 0.5);
    vec3 C = col * A + rimCol * rim;
    float a = clamp(A + rim * (1.0 - A), 0.0, 1.0);
    frag = vec4(C / max(a, 1e-3), a);
})";

static GLuint compile(const char* vs, const char* fs)
{
    auto one = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            OutputDebugStringA(log);
            fprintf(stderr, "shader: %s\n", log);
        }
        return s;
    };
    GLuint v = one(GL_VERTEX_SHADER, vs), f = one(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "link: %s\n", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return ok ? p : 0;
}

// ---------------------------------------------------------------- fonts

int Renderer::indexOf(uint32_t cp) const
{
    auto it = std::lower_bound(codepoints_.begin(), codepoints_.end(), (int)cp);
    return (it != codepoints_.end() && *it == (int)cp) ? int(it - codepoints_.begin()) : -1;
}

bool Renderer::init(float scale)
{
    prog_ = compile(kVS, kFS);
    bgProg_ = compile(kBgVS, kBgFS);
    sheetProg_ = compile(kBgVS, kSheetFS);
    if (!prog_ || !bgProg_ || !sheetProg_) return false;

    glGenVertexArrays(1, &bgVao_);
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const int stride = sizeof(Vtx);
    const int sizes[] = {2, 2, 4, 2, 2, 4, 4};
    size_t off = 0;
    for (int i = 0; i < 7; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, stride, (void*)off);
        off += sizes[i] * sizeof(float);
    }

    for (int c = 32; c < 127; ++c) codepoints_.push_back(c);
    for (int c = 0xA0; c <= 0xFF; ++c) codepoints_.push_back(c);
    for (int c = 0x400; c <= 0x45F; ++c) codepoints_.push_back(c);
    for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2039, 0x203A, 0x2116, 0x2190}) codepoints_.push_back(c);
    std::sort(codepoints_.begin(), codepoints_.end());

    std::vector<unsigned char> semi = resourceBytes(L"FONT_SEMIBOLD");
    std::vector<unsigned char> bold = resourceBytes(L"FONT_BOLD");
    std::vector<unsigned char> ital = resourceBytes(L"FONT_ITALIC");
    std::vector<unsigned char> sc = resourceBytes(L"FONT_SC");
    if (semi.empty() || bold.empty() || ital.empty() || sc.empty()) return false;
    struct Job { Font* font; std::vector<unsigned char>* data; float px; };
    Job jobs[] = {
        {&body, &semi, 23}, {&caption, &semi, 19.5f}, {&italic, &ital, 21}, {&heading, &sc, 18.5f},
        {&title, &sc, 46},  {&big, &bold, 35},     {&button, &sc, 20.5f},
    };

    for (int dim = 2048; dim <= 8192; dim *= 2) {
        std::vector<unsigned char> pixels((size_t)dim * dim);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, pixels.data(), dim, dim, 0, 1, nullptr);
        bool ok = true;
        for (auto& j : jobs) {
            Font& f = *j.font;
            f.size = std::round(j.px * scale);
            f.glyphs.assign(codepoints_.size(), {});
            stbtt_PackSetOversampling(&pc, f.size < 40 ? 2 : 1, 1);
            stbtt_pack_range range{};
            range.font_size = f.size;
            range.array_of_unicode_codepoints = codepoints_.data();
            range.num_chars = (int)codepoints_.size();
            range.chardata_for_range = f.glyphs.data();
            if (!stbtt_PackFontRanges(&pc, j.data->data(), 0, &range, 1)) { ok = false; break; }
            stbtt_fontinfo info;
            stbtt_InitFont(&info, j.data->data(), 0);
            int asc, desc, gap;
            stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
            float s = stbtt_ScaleForPixelHeight(&info, f.size);
            f.ascent = asc * s;
            f.descent = -desc * s;
        }
        stbtt_PackEnd(&pc);
        if (!ok) continue;

        atlasW_ = atlasH_ = dim;
        glGenTextures(1, &atlas_);
        glBindTexture(GL_TEXTURE_2D, atlas_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, dim, dim, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- frame

void Renderer::begin(int w, int h, float t)
{
    scale_ = 1;
    dy_ = 0;
    master_ = 1;
    alpha_ = 1;
    disMode_ = Dissolve::None;
    width = w;
    height = h;
    time = t;
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::end() { flush(); }

void Renderer::background(float alpha, const Backdrop& b)
{
    flush();
    glUseProgram(bgProg_);
    auto u3 = [&](const char* n, const Col& c) { glUniform3f(glGetUniformLocation(bgProg_, n), c.r, c.g, c.b); };
    u3("uTop", b.top);
    u3("uBottom", b.bottom);
    u3("uFog", b.fog);
    u3("uFire", b.fire);
    u3("uSpark", b.spark);
    glUniform2f(glGetUniformLocation(bgProg_, "uFirePos"), b.fireX, b.fireY);
    glUniform1f(glGetUniformLocation(bgProg_, "uFireAmt"), b.fireAmt);
    glUniform1f(glGetUniformLocation(bgProg_, "uSparkAmt"), b.sparkAmt);
    glUniform1f(glGetUniformLocation(bgProg_, "uSparkSpeed"), b.sparkSpeed);
    glUniform1f(glGetUniformLocation(bgProg_, "uFogAmt"), b.fogAmt);
    glUniform1f(glGetUniformLocation(bgProg_, "uEmbers"), b.embers);
    glUniform1f(glGetUniformLocation(bgProg_, "uAsh"), b.ash);
    glUniform1f(glGetUniformLocation(bgProg_, "uMotes"), b.motes);
    glUniform2f(glGetUniformLocation(bgProg_, "uRes"), (float)width, (float)height);
    glUniform1f(glGetUniformLocation(bgProg_, "uTime"), time);
    glUniform1f(glGetUniformLocation(bgProg_, "uAlpha"), alpha);
    glBindVertexArray(bgVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Renderer::burnSheet(float cx, float cy, float burn, float sheet, const BurnStyle& s)
{
    flush();
    glUseProgram(sheetProg_);
    auto u1 = [&](const char* n, float v) { glUniform1f(glGetUniformLocation(sheetProg_, n), v); };
    auto u3 = [&](const char* n, const Col& c) { glUniform3f(glGetUniformLocation(sheetProg_, n), c.r, c.g, c.b); };
    glUniform2f(glGetUniformLocation(sheetProg_, "uRes"), (float)width, (float)height);
    glUniform2f(glGetUniformLocation(sheetProg_, "uCenter"), cx, cy);
    u1("uTime", time);
    u1("uBurn", burn);
    u1("uSheet", sheet);
    u1("uGrain", s.grain);
    u1("uChar", s.charWidth);
    u1("uSpecks", s.specks);
    u3("uRim", s.rim);
    u3("uRimHot", s.rimHot);
    glBindVertexArray(bgVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Renderer::flush()
{
    if (verts_.empty()) return;
    glUseProgram(prog_);
    glUniform2f(glGetUniformLocation(prog_, "uScreen"), (float)width, (float)height);
    glUniform1f(glGetUniformLocation(prog_, "uAlpha"), alpha_ * master_);
    glUniform1i(glGetUniformLocation(prog_, "uDisMode"), (int)disMode_);
    glUniform1f(glGetUniformLocation(prog_, "uDissolve"), dissolve_);
    glUniform1f(glGetUniformLocation(prog_, "uDisGrain"), disGrain_);
    glUniform2f(glGetUniformLocation(prog_, "uDisCenter"), disX_, disY_);
    glUniform2f(glGetUniformLocation(prog_, "uXform"), scale_, dy_);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, curTex_ ? curTex_ : atlas_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(Vtx)), verts_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts_.size());
    verts_.clear();
}

void Renderer::setClip(const Rect* r)
{
    flush();
    if (!r) { glDisable(GL_SCISSOR_TEST); return; }
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)r->x, (GLint)(height - r->b()), (GLsizei)std::max(0.f, r->w), (GLsizei)std::max(0.f, r->h));
}

// ---------------------------------------------------------------- primitives

void Renderer::quad(const Rect& r, Col c, float radius, float border, float feather, float mode, Col bc,
                    float u0, float v0, float u1, float v1)
{
    float pad = (mode == 0 || mode == 3 || mode == 6) ? std::max(feather, 1.f) : 0.f;
    float x0 = r.x - pad, y0 = r.y - pad, x1 = r.r() + pad, y1 = r.b() + pad;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hw = r.w * 0.5f, hh = r.h * 0.5f;
    auto v = [&](float x, float y, float u, float vv) {
        verts_.push_back({x, y, u, vv, c.r, c.g, c.b, c.a, x - cx, y - cy, hw, hh, radius, border, feather, mode,
                          bc.r, bc.g, bc.b, bc.a});
    };
    v(x0, y0, u0, v0); v(x1, y0, u1, v0); v(x1, y1, u1, v1);
    v(x0, y0, u0, v0); v(x1, y1, u1, v1); v(x0, y1, u0, v1);
}

void Renderer::rect(const Rect& r, Col fill, float radius, float feather) { quad(r, fill, radius, 0, feather, 0, {}); }
void Renderer::frame(const Rect& r, Col border, float w, float radius, Col fill)
{
    quad(r, fill.a > 0 ? fill : border.withA(0), radius, w, 1, 0, border);
}
void Renderer::gradV(const Rect& r, Col top, Col bottom, float radius) { quad(r, top, radius, 0, 1, 3, bottom); }
void Renderer::gradH(const Rect& r, Col left, Col right) { quad(r, left, 0, 0, 1, 6, right); }
void Renderer::shadow(const Rect& r, float radius, float spread, Col c)
{
    quad({r.x, r.y + spread * 0.25f, r.w, r.h}, c, radius + spread * 0.5f, 0, spread * 2, 0, {});
}
void Renderer::glow(float cx, float cy, float radius, Col c)
{
    quad({cx - radius, cy - radius, radius * 2, radius * 2}, c, 0, 0, 0, 4, {});
}
void Renderer::diamond(float cx, float cy, float size, Col c)
{
    quad({cx - size, cy - size, size * 2, size * 2}, c, 0, 0, 0, 5, {});
}

Image Renderer::loadImage(const std::vector<unsigned char>& bytes)
{
    Image img;
    if (bytes.empty()) return img;
    int n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &img.w, &img.h, &n, 4);
    if (!px) return img;
    glGenTextures(1, &img.tex);
    glBindTexture(GL_TEXTURE_2D, img.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(px);
    return img;
}

void Renderer::image(const Image& img, const Rect& r, float alpha, float radius, float feather, float crop)
{
    if (!img) return;
    flush();
    curTex_ = img.tex;
    quad(r, {1, 1, 1, alpha}, radius, 0, feather, 2, {}, crop, crop, 1 - crop, 1 - crop);
    flush();
    curTex_ = 0;
}

void Renderer::triangle(float cx, float cy, float size, Col c, bool up)
{
    quad({cx - size, cy - size, size * 2, size * 2}, c, 0, 0, 0, up ? 7.f : 8.f, {});
}

// ---------------------------------------------------------------- text

static uint32_t nextCp(const std::string& s, size_t& i)
{
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

float Renderer::text(const Font& f, float x, float y, const std::string& s, Col c, float tracking)
{
    float px = std::round(x), py = std::round(y + f.ascent);
    float start = px;
    for (size_t i = 0; i < s.size();) {
        int idx = indexOf(nextCp(s, i));
        if (idx < 0) idx = indexOf('?');
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(f.glyphs.data(), atlasW_, atlasH_, idx, &px, &py, &q, 0);
        if (q.x1 > q.x0) quad({q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0}, c, 0, 0, 0, 1, {}, q.s0, q.t0, q.s1, q.t1);
        px += tracking;
    }
    return px - start - (s.empty() ? 0 : tracking);
}

float Renderer::measure(const Font& f, const std::string& s, float tracking) const
{
    float w = 0;
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        int idx = indexOf(nextCp(s, i));
        if (idx < 0) idx = indexOf('?');
        w += f.glyphs[idx].xadvance + tracking;
        ++n;
    }
    return n ? w - tracking : 0;
}

std::string Renderer::ellipsize(const Font& f, const std::string& s, float maxW, float tracking) const
{
    if (measure(f, s, tracking) <= maxW) return s;
    std::string out = s;
    while (!out.empty()) {
        size_t cut = out.size() - 1;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;
        out.resize(cut);
        if (measure(f, out + "…", tracking) <= maxW) return out + "…";
    }
    return "…";
}

std::vector<std::string> Renderer::wrap(const Font& f, const std::string& s, float maxW) const
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t p = 0;
        while (p < para.size()) {
            size_t sp = para.find(' ', p);
            std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
            std::string test = line.empty() ? word : line + " " + word;
            if (!line.empty() && measure(f, test) > maxW) {
                lines.push_back(line);
                line = word;
            } else {
                line = test;
            }
            if (sp == std::string::npos) break;
            p = sp + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}
