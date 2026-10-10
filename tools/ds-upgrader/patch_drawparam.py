"""DS Upgrader: in-engine graphics patch for DARK SOULS REMASTERED DrawParams.

Usage: python patch_drawparam.py <vanilla DrawParam dir> <output dir>

Global rules apply to every area, then per-area rules are applied on top (they multiply).
Only the entries touched by a rule are re-encoded; every other entry in each .parambnd.dcx
(incl. the vanilla-corrupt ToneMap/ToneCorrect banks in default/a99) stays byte-for-byte.
"""
import sys
from pathlib import Path

from soulstruct.containers import Binder
from soulstruct.darksouls1ptde.params.draw_param import DrawParam, TypedDrawParam
from soulstruct.darksouls1r.params import paramdef


def mul(f, cap=None):
    def apply(v):
        n = v * f
        if isinstance(v, int):
            n = int(round(n))
            if cap is not None:
                n = min(n, max(cap, v))  # never clamp below a vanilla value already over the cap
        return n
    return apply


def lod(f):
    # 9998/9999 rows mean "never switch"; leave them alone
    return lambda v: v if v >= 9000 else v * f


def tone(r=1.0, g=1.0, b=1.0, contrast=1.0, sat=1.0):
    rules = {}
    for ch, k in (("Red", r), ("Green", g), ("Blue", b)):
        if k != 1.0:
            rules[f"Brightness{ch}"] = mul(k)
        if contrast != 1.0:
            rules[f"Contrast{ch}"] = mul(contrast)
    if sat != 1.0:
        rules["SaturationCorrection"] = mul(sat)
    return rules


def fog(dist=1.0, alpha=1.0, r=1.0, g=1.0, b=1.0):
    rules = {}
    if dist != 1.0:
        rules["FogEndDistance"] = mul(dist)
    if alpha != 1.0:
        rules["Alpha"] = mul(alpha, cap=255)
    for ch, k in (("Red", r), ("Green", g), ("Blue", b)):
        if k != 1.0:
            rules[ch] = mul(k, cap=255)
    return rules


# ---------------------------------------------------------------- every area

GLOBAL = {
    # Cast shadows reach ~1.5x farther and fade later: less shadow pop-in.
    "SHADOW_BANK": {
        "ShadowEndDistance": mul(1.5),
        "FadeStartDistance": mul(1.5),
        "FadeDistance": mul(1.5),
    },
    # Torches, bonfires, glowing items: wider and brighter glow.
    "POINT_LIGHT_BANK": {
        "FadeStartDistance": mul(1.4),
        "FadeEndDistance": mul(1.4),
        "PointLightAlpha": mul(1.25, cap=255),
    },
    # Moodier lighting: stronger key light, less flat ambient fill.
    # SpecularLight* is the engine's image-based reflection (env cubemaps SpecularLightTextureID0-3;
    # ENV_LIGHT_TEX_BANK is unused, isUse=0 everywhere): stone, metal, armour and wet floors
    # reflect their surroundings noticeably more.
    "LIGHT_BANK": {
        "BakedLight0Alpha": mul(1.10, cap=255),
        "SpecularBakedLightAlpha": mul(1.30, cap=255),
        "SpecularLightAlpha": mul(1.45, cap=255),
        "DiffuseLightAlpha": mul(0.90),
        "TopDownLightAlpha": mul(0.90),
    },
    # Halve the vanilla far-distance blur so distant scenery stays crisp.
    "DOF_BANK": {
        "FarBlurMagnitude": mul(0.5),
    },
    # Richer atmospheric glow around the sun.
    "LIGHT_SCATTERING_BANK": {
        "MieCoefficient": mul(1.2),
    },
    # Engine bloom: glow starts on slightly dimmer highlights and is stronger; stronger god rays.
    "TONE_MAP_BANK": {
        "NearBloomThreshold": mul(0.85),
        "FarBloomThreshold": mul(0.85),
        "NearBloomMultiplier": mul(1.30, cap=100),
        "FarBloomMultiplier": mul(1.20),
        "LightShaftMagnitude": mul(1.5),
    },
    # Spells, fire, embers, sparks and other particle effects glow brighter (vanilla 1.0 everywhere).
    "TONE_CORRECT_BANK": {
        "SFXMultiplier": mul(1.20),
    },
    # Slightly larger and brighter sun / light flares.
    "LENS_FLARE_BANK": {
        "TextureScale": mul(1.15),
    },
    "LENS_FLARE_EX_BANK": {
        "LensFlareSourceAlpha": mul(1.20),
    },
    # Keep full-detail models twice as far: less LOD popping.
    "LOD_BANK": {
        "lv01_BorderDist": lod(2.0),
        "lv12_BorderDist": lod(2.0),
    },
}

# ---------------------------------------------------------------- per area (multiplies on top of GLOBAL)

AREAS = {
    # Undead Burg / Parish, Depths, Firelink: warm late-afternoon sun, a touch more contrast.
    "a10": {
        "TONE_CORRECT_BANK": tone(r=1.02, b=0.98, contrast=1.03),
        "FOG_BANK": fog(alpha=1.10),
        "TONE_MAP_BANK": {"LightShaftMagnitude": mul(1.2)},
    },
    # Painted World of Ariamis: cold, washed-out, snowy haze.
    "a11": {
        "TONE_CORRECT_BANK": tone(r=0.97, b=1.04, sat=0.85),
        "FOG_BANK": fog(dist=0.80, alpha=1.20, b=1.08),
    },
    # Darkroot Garden / Basin, Oolacile: misty green forest, sun through the trees.
    "a12": {
        "TONE_CORRECT_BANK": tone(sat=1.05),
        "FOG_BANK": fog(dist=0.80, alpha=1.20, g=1.06),
        "TONE_MAP_BANK": {"LightShaftMagnitude": mul(1.3)},
    },
    # Catacombs, Tomb of the Giants, Ash Lake: darker, light only from sources.
    "a13": {
        "TONE_CORRECT_BANK": tone(r=0.92, g=0.92, b=0.92, contrast=1.05),
        "FOG_BANK": fog(r=0.85, g=0.85, b=0.85),
    },
    # Blighttown, Demon Ruins, Lost Izalith: thick, hot, saturated air.
    "a14": {
        "TONE_CORRECT_BANK": tone(r=1.03, contrast=1.04, sat=1.08),
        "FOG_BANK": fog(dist=0.85, alpha=1.15),
    },
    # Sen's Fortress, Anor Londo: golden sun, clearer air, strong rays.
    "a15": {
        "TONE_CORRECT_BANK": tone(r=1.05, g=1.02, b=0.95),
        "FOG_BANK": fog(alpha=0.90),
        "TONE_MAP_BANK": {"LightShaftMagnitude": mul(1.4)},
        "LIGHT_SCATTERING_BANK": {"MieCoefficient": mul(1.15)},
    },
    # New Londo Ruins, Valley of Drakes: cold teal, heavy mist.
    "a16": {
        "TONE_CORRECT_BANK": tone(r=0.96, b=1.05, sat=0.85),
        "FOG_BANK": fog(dist=0.80, alpha=1.20, r=0.92, b=1.10),
    },
    # Duke's Archives, Crystal Cave: cool and crisp.
    "a17": {
        "TONE_CORRECT_BANK": tone(b=1.04, contrast=1.04, sat=0.95),
    },
    # Kiln of the First Flame, Undead Asylum: ashen, desaturated.
    "a18": {
        "TONE_CORRECT_BANK": tone(contrast=1.06, sat=0.80),
    },
}


def rules_for(area: str, ptype: str) -> list[dict]:
    return [r for r in (GLOBAL.get(ptype), AREAS.get(area, {}).get(ptype)) if r]


def patch_file(src: Path, dst: Path) -> dict:
    area = src.name.split("_")[0]
    binder = Binder.from_path(src)
    stats = {}
    for entry in binder.entries:
        try:
            ptype = DrawParam.detect_param_type(entry.data)
        except Exception:
            continue
        rule_sets = rules_for(area, ptype)
        if not rule_sets:
            continue
        try:
            param = entry.to_binary_file(TypedDrawParam(getattr(paramdef, ptype)))
        except Exception:
            print(f"  ! {src.name}:{entry.name} unreadable (vanilla-corrupt), left untouched")
            continue

        # Round-trip guard: unmodified re-encode must equal the original bytes, except the
        # header strings offset (u32 @0), which vanilla writes inconsistently; we restore it.
        orig = bytes(entry.data)
        if bytes(param.to_bytes())[4:] != orig[4:]:
            print(f"  ! {src.name}:{entry.name} does not round-trip, left untouched")
            continue

        for row in param.rows.values():
            for rules in rule_sets:
                for field, fn in rules.items():
                    setattr(row, field, fn(getattr(row, field)))
        new = bytes(param.to_bytes())
        entry.data = orig[:4] + new[4:]
        stats[ptype] = stats.get(ptype, 0) + len(param.rows)
    dst.parent.mkdir(parents=True, exist_ok=True)
    binder.write(dst)
    return stats


def main():
    src_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    for src in sorted(src_dir.glob("*.parambnd.dcx")):
        stats = patch_file(src, out_dir / src.name)
        print(f"{src.name}: " + ", ".join(f"{k}={v}" for k, v in sorted(stats.items())))


if __name__ == "__main__":
    main()
