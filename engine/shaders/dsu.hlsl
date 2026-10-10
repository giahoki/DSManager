// DSU Engine: ambient occlusion, screen-space reflections and HDR bloom for DARK SOULS REMASTERED.
// Runs inside the game's frame, on the HDR scene, right before the game's own tonemap.
// Edited live: save this file and press F11 in game.

cbuffer DSU : register(b0)
{
    float4 gScreen;       // w, h, 1/w, 1/h of the scene
    float4 gDepthParams;  // game's linearisation: viewZ = w / (d * z + y)
    float4 gProjUV;       // viewXY = viewZ * (uv * xy + zw)
    float4 gSsr;          // intensity, max distance (m), steps, thickness (m)
    float4 gSsr2;         // wall amount, F0, frame index, debug view
    float4 gBloom;        // intensity, threshold, knee, radius
    float4 gSrc;          // texel size + size of the source of a bloom pass
    float4 gWorldUp;      // world up in view space (xyz), view distance treated as sky (w)
    float4 gAo;           // intensity (power), radius (m), slices, steps per side
    float4 gAo2;          // light protect * exposure, max radius (px), blur direction (0 x, 1 y), unused
};

SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);

Texture2D tA : register(t0);
Texture2D tB : register(t1);
Texture2D tC : register(t2);
Texture2D tD : register(t3);
Texture2D tE : register(t4);

struct V2P { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

V2P FullscreenVS(uint id : SV_VertexID)
{
    V2P o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// ---------------------------------------------------------------- helpers

// How much a surface may reflect: 1 on floors, puddles and steps (facing world up), WallAmount elsewhere.
float SurfaceMask(float3 n) { return lerp(gSsr2.x, 1, smoothstep(0.7, 0.92, dot(n, gWorldUp.xyz))); }

float LinearZ(float d) { return gDepthParams.w / (d * gDepthParams.z + gDepthParams.y); }
float3 ViewPos(float2 uv, float z) { return float3(z * (uv * gProjUV.xy + gProjUV.zw), z); }
float2 ViewToUV(float3 p) { return (p.xy / p.z - gProjUV.zw) / gProjUV.xy; }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float IGN(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))); }  // interleaved gradient noise

// ---------------------------------------------------------------- 1. G-buffer from depth
// tA = hardware depth. Out: view-space normal (xyz), linear depth (w).

float4 PrepPS(V2P i) : SV_Target
{
    float2 px = gScreen.zw;
    float zc = LinearZ(tA.SampleLevel(sPoint, i.uv, 0).r);
    float zl = LinearZ(tA.SampleLevel(sPoint, i.uv - float2(px.x, 0), 0).r);
    float zr = LinearZ(tA.SampleLevel(sPoint, i.uv + float2(px.x, 0), 0).r);
    float zu = LinearZ(tA.SampleLevel(sPoint, i.uv - float2(0, px.y), 0).r);
    float zd = LinearZ(tA.SampleLevel(sPoint, i.uv + float2(0, px.y), 0).r);

    float3 p = ViewPos(i.uv, zc);
    // take the neighbour on the same surface (smaller depth step) on each axis: no halos at edges
    float3 dx = abs(zr - zc) < abs(zc - zl) ? ViewPos(i.uv + float2(px.x, 0), zr) - p : p - ViewPos(i.uv - float2(px.x, 0), zl);
    float3 dy = abs(zd - zc) < abs(zc - zu) ? ViewPos(i.uv + float2(0, px.y), zd) - p : p - ViewPos(i.uv - float2(0, px.y), zu);
    float3 n = normalize(cross(dy, dx));
    if (dot(n, p) > 0) n = -n;  // face the camera
    return float4(n, zc);
}

// ---------------------------------------------------------------- 1b. ambient occlusion (GTAO)
// Ground-truth AO (Jimenez et al. 2016, after Intel's XeGTAO): per slice, find the highest horizon on both
// sides in the depth buffer and integrate the visible arc against the cosine of the normal.
// tA = prep. Out: visibility (1 = open, 0 = fully occluded).

static const float PI = 3.14159265;

float FastAcos(float x)
{
    float a = abs(x);
    float r = (-0.156583 * a + PI * 0.5) * sqrt(1 - a);
    return x >= 0 ? r : PI - r;
}

float4 AoPS(V2P i) : SV_Target
{
    float4 g = tA.SampleLevel(sPoint, i.uv, 0);
    float z = g.w;
    if (z <= 0 || z > gWorldUp.w || g.x != g.x) return 1;

    float3 p = ViewPos(i.uv, z);
    float3 n = g.xyz;
    float3 v = normalize(-p);

    float radius = gAo.y;
    float radiusPx = radius / (z * abs(gProjUV.y)) * gScreen.y;  // world radius projected to pixels
    float fade = saturate(radiusPx / 3 - 0.5);                      // far away the radius is under a pixel or two
    if (fade <= 0) return 1;
    radiusPx = min(radiusPx, gAo2.y);

    // falloff: full weight up to ~40% of the radius, none at the radius
    float falloffRange = 0.615 * radius;
    float falloffMul = -1 / falloffRange;
    float falloffAdd = (radius - falloffRange) / falloffRange + 1;

    int slices = (int)gAo.z, steps = (int)gAo.w;
    float noiseSlice = IGN(i.pos.xy);
    float noiseStep = IGN(i.pos.xy + float2(17.0, 31.0));
    float minS = 1.3 / radiusPx;  // never sample the pixel itself

    float vis = 0;
    [loop] for (int sl = 0; sl < slices; ++sl) {
        float phi = (sl + noiseSlice) * PI / slices;
        float2 omega = float2(cos(phi), -sin(phi)) * radiusPx * gScreen.zw;  // screen y goes down
        float3 dir = float3(cos(phi), sin(phi), 0);
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pnLen = length(pn);
        float sgn = sign(dot(ortho, pn));
        float cosN = saturate(dot(pn, v) / max(pnLen, 1e-4));
        float nAng = sgn * FastAcos(cosN);
        float low0 = cos(nAng + PI * 0.5), low1 = cos(nAng - PI * 0.5);
        float h0c = low0, h1c = low1;

        [loop] for (int st = 0; st < steps; ++st) {
            float s = (st + noiseStep) / steps;
            s = s * s + minS;
            float2 off = omega * s;
            float2 uv0 = i.uv + off, uv1 = i.uv - off;
            float3 d0 = ViewPos(uv0, tA.SampleLevel(sPoint, uv0, 0).w) - p;
            float3 d1 = ViewPos(uv1, tA.SampleLevel(sPoint, uv1, 0).w) - p;
            float l0 = length(d0), l1 = length(d1);
            float c0 = dot(d0 / l0, v), c1 = dot(d1 / l1, v);
            float w0 = saturate(l0 * falloffMul + falloffAdd) * all(uv0 == saturate(uv0));
            float w1 = saturate(l1 * falloffMul + falloffAdd) * all(uv1 == saturate(uv1));
            h0c = max(h0c, lerp(low0, c0, w0));
            h1c = max(h1c, lerp(low1, c1, w1));
        }

        float h0 = -FastAcos(h1c), h1 = FastAcos(h0c);
        h0 = nAng + max(h0 - nAng, -PI * 0.5);
        h1 = nAng + min(h1 - nAng, PI * 0.5);
        float sinN = sin(nAng);
        float arc0 = (cosN + 2 * h0 * sinN - cos(2 * h0 - nAng)) / 4;
        float arc1 = (cosN + 2 * h1 * sinN - cos(2 * h1 - nAng)) / 4;
        vis += pnLen * (arc0 + arc1);
    }
    vis = saturate(vis / slices);
    vis = pow(vis, gAo.x);
    return lerp(1, vis, fade);
}

// Depth-aware 9-tap blur, run once along x and once along y. tA = AO, tB = prep.
float4 AoBlurPS(V2P i) : SV_Target
{
    float2 dir = (gAo2.z < 0.5 ? float2(1, 0) : float2(0, 1)) * gScreen.zw;
    float zc = tB.SampleLevel(sPoint, i.uv, 0).w;
    float sum = 0, wsum = 0;
    [unroll] for (int k = -4; k <= 4; ++k) {
        float2 uv = i.uv + dir * k;
        float zs = tB.SampleLevel(sPoint, uv, 0).w;
        float w = exp(-k * k / 12.0) * exp(-abs(zs - zc) / max(zc * 0.03, 1e-3));
        sum += tA.SampleLevel(sPoint, uv, 0).r * w;
        wsum += w;
    }
    return sum / max(wsum, 1e-4);
}

// ---------------------------------------------------------------- 2. reflections
// tA = scene HDR, tB = prep. Out: reflected colour (rgb), confidence (a).

float4 SsrPS(V2P i) : SV_Target
{
    float4 g = tB.SampleLevel(sPoint, i.uv, 0);
    float z = g.w;
    float maxDist = gSsr.y;
    if (z <= 0 || z > gWorldUp.w || g.x != g.x) return 0;  // sky and invalid pixels reflect nothing

    float3 p = ViewPos(i.uv, z);
    float3 n = g.xyz;
    if (SurfaceMask(n) < 0.01) return 0;  // walls, characters: not traced at all
    float3 v = normalize(p);
    float3 r = reflect(v, n);
    if (r.z < -0.2) return 0;  // rays toward the camera leave the screen

    int steps = (int)gSsr.z;
    float thickness = gSsr.w;
    float jitter = IGN(i.pos.xy + gSsr2.z * 5.588238);

    // march in view space with growing steps (finer near the surface)
    float t = 0, prevT = 0;
    float2 hitUV = 0;
    bool hit = false;
    [loop] for (int s = 1; s <= steps; ++s) {
        float k = (s + jitter) / steps;
        t = maxDist * k * k;
        float3 q = p + r * t + n * 0.02 * z;
        if (q.z < 0.05) break;
        float2 uv = ViewToUV(q);
        if (any(uv < 0) || any(uv > 1)) break;
        float sceneZ = tB.SampleLevel(sPoint, uv, 0).w;
        float dz = q.z - sceneZ;
        if (dz > 0 && dz < thickness * (1 + t * 0.05)) {
            // binary refine between the previous and this step
            float a = prevT, b = t;
            [unroll] for (int j = 0; j < 6; ++j) {
                float m = (a + b) * 0.5;
                float3 qm = p + r * m + n * 0.02 * z;
                float2 um = ViewToUV(qm);
                if (qm.z - tB.SampleLevel(sPoint, um, 0).w > 0) b = m; else a = m;
            }
            hitUV = ViewToUV(p + r * b + n * 0.02 * z);
            t = b;
            hit = true;
            break;
        }
        prevT = t;
    }
    if (!hit) return 0;

    float4 hg = tB.SampleLevel(sPoint, hitUV, 0);
    if (dot(hg.xyz, r) > 0.3) return 0;           // hit the back of a surface
    float2 edge = saturate((0.5 - abs(hitUV - 0.5)) * 8);
    float conf = edge.x * edge.y;
    conf *= 1 - saturate(t / maxDist);              // fade with distance along the ray
    float3 c = tA.SampleLevel(sLinear, hitUV, 0).rgb;
    c = c / (1 + Luma(c) * 0.05);                    // tame fireflies
    return float4(c * conf, conf);
}

// ---------------------------------------------------------------- 3. bloom

float3 Prefilter(float3 c)
{
    float br = max(c.r, max(c.g, c.b));
    float knee = gBloom.z * gBloom.y + 1e-4;
    float soft = clamp(br - gBloom.y + knee, 0, 2 * knee);
    soft = soft * soft / (4 * knee);
    return c * max(soft, br - gBloom.y) / max(br, 1e-4);
}

float3 KarisAvg(float3 a, float3 b, float3 c, float3 d)
{
    float wa = 1 / (1 + Luma(a)), wb = 1 / (1 + Luma(b)), wc = 1 / (1 + Luma(c)), wd = 1 / (1 + Luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

// 13-tap downsample (Jimenez 2014). tA = source.
float3 Down13(float2 uv, bool karis)
{
    float2 t = gSrc.xy;
    float3 a = tA.SampleLevel(sLinear, uv + t * float2(-2, -2), 0).rgb;
    float3 b = tA.SampleLevel(sLinear, uv + t * float2(0, -2), 0).rgb;
    float3 c = tA.SampleLevel(sLinear, uv + t * float2(2, -2), 0).rgb;
    float3 d = tA.SampleLevel(sLinear, uv + t * float2(-1, -1), 0).rgb;
    float3 e = tA.SampleLevel(sLinear, uv + t * float2(1, -1), 0).rgb;
    float3 f = tA.SampleLevel(sLinear, uv + t * float2(-2, 0), 0).rgb;
    float3 g = tA.SampleLevel(sLinear, uv, 0).rgb;
    float3 h = tA.SampleLevel(sLinear, uv + t * float2(2, 0), 0).rgb;
    float3 i = tA.SampleLevel(sLinear, uv + t * float2(-1, 1), 0).rgb;
    float3 j = tA.SampleLevel(sLinear, uv + t * float2(1, 1), 0).rgb;
    float3 k = tA.SampleLevel(sLinear, uv + t * float2(-2, 2), 0).rgb;
    float3 l = tA.SampleLevel(sLinear, uv + t * float2(0, 2), 0).rgb;
    float3 m = tA.SampleLevel(sLinear, uv + t * float2(2, 2), 0).rgb;
    if (karis) {
        return KarisAvg(d, e, i, j) * 0.5 + KarisAvg(a, b, f, g) * 0.125 + KarisAvg(b, c, g, h) * 0.125
             + KarisAvg(f, g, k, l) * 0.125 + KarisAvg(g, h, l, m) * 0.125;
    }
    return (d + e + i + j) * 0.125 + (a + b + f + g) * 0.03125 + (b + c + g + h) * 0.03125
         + (f + g + k + l) * 0.03125 + (g + h + l + m) * 0.03125;
}

float4 BloomPrefilterPS(V2P i) : SV_Target { return float4(Prefilter(Down13(i.uv, true)), 1); }
float4 BloomDownPS(V2P i) : SV_Target { return float4(Down13(i.uv, false), 1); }

// tA = this level of the down chain, tB = the next (smaller) level of the up chain.
float4 BloomUpPS(V2P i) : SV_Target
{
    float2 t = gSrc.xy;  // texel of tB
    float3 s = tB.SampleLevel(sLinear, i.uv + t * float2(-1, -1), 0).rgb
             + tB.SampleLevel(sLinear, i.uv + t * float2(0, -1), 0).rgb * 2
             + tB.SampleLevel(sLinear, i.uv + t * float2(1, -1), 0).rgb
             + tB.SampleLevel(sLinear, i.uv + t * float2(-1, 0), 0).rgb * 2
             + tB.SampleLevel(sLinear, i.uv, 0).rgb * 4
             + tB.SampleLevel(sLinear, i.uv + t * float2(1, 0), 0).rgb * 2
             + tB.SampleLevel(sLinear, i.uv + t * float2(-1, 1), 0).rgb
             + tB.SampleLevel(sLinear, i.uv + t * float2(0, 1), 0).rgb * 2
             + tB.SampleLevel(sLinear, i.uv + t * float2(1, 1), 0).rgb;
    return float4(tA.SampleLevel(sLinear, i.uv, 0).rgb + s / 16 * gBloom.w, 1);
}

// ---------------------------------------------------------------- 4. final
// tA = scene HDR, tB = prep, tC = reflections, tD = bloom (up chain level 0), tE = ambient occlusion.

float4 FinalPS(V2P i) : SV_Target
{
    float4 scene = tA.SampleLevel(sPoint, i.uv, 0);
    float4 g = tB.SampleLevel(sPoint, i.uv, 0);
    int dbg = (int)gSsr2.w;

    // depth-aware 3x3 filter of the reflections (they are noisy at 1 sample per pixel)
    float4 refl = 0;
    float wsum = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float2 o = float2(x, y) * gScreen.zw * 1.5;
        float zs = tB.SampleLevel(sPoint, i.uv + o, 0).w;
        float w = exp(-abs(zs - g.w) / max(g.w * 0.02, 1e-3)) * (x == 0 && y == 0 ? 2 : 1);
        refl += tC.SampleLevel(sLinear, i.uv + o, 0) * w;
        wsum += w;
    }
    refl /= max(wsum, 1e-4);

    float3 n = g.xyz;
    float3 v = normalize(ViewPos(i.uv, g.w));
    float ndv = saturate(dot(n, -v));
    float f0 = gSsr2.y;
    float fres = f0 + (1 - f0) * pow(1 - ndv, 5);
    float k = gSsr.x * saturate(fres) * SurfaceMask(n);

    float3 bloom = tD.SampleLevel(sLinear, i.uv, 0).rgb;

    // AO darkens what ambient light lights; bright pixels are lit directly (or glow) and keep their light
    float ao = tE.SampleLevel(sPoint, i.uv, 0).r;
    ao = lerp(ao, 1, saturate(Luma(scene.rgb) * gAo2.x));

    if (dbg == 1) return float4(frac(g.www / 10) * (g.w < gWorldUp.w), 1);
    if (dbg == 2) return float4(n * 0.5 + 0.5, 1);
    if (dbg == 3) return float4(refl.rgb * k / max(refl.a, 1e-3), 1);
    if (dbg == 4) return float4(bloom * gBloom.x, 1);
    if (dbg == 5) return float4(ao.xxx, 1);

    float3 c = scene.rgb * ao;
    c = lerp(c, max(c, refl.rgb / max(refl.a, 1e-3)), k * refl.a);  // reflection replaces the dull env spec where found
    c += bloom * gBloom.x;
    return float4(c, scene.a);
}
