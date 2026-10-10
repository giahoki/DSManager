"""DS Upgrader: in-engine material patch for DARK SOULS REMASTERED (mtd\\*.mtdbnd.dcx).

Usage: python patch_mtd.py <vanilla mtd dir> <output dir>

Makes the engine's own water reflections (FRPG_Water_Reflect / FRPG_Water_Env shaders)
stronger and visible at more view angles. soulstruct reads MTDs but cannot write them, so
values are overwritten in place: only the float bytes of the touched params change, every
other byte of every MTD stays vanilla.
"""
import struct
import sys
from pathlib import Path

from soulstruct.containers import Binder
from soulstruct.base.models.mtd import MTD, MTDParam

# ---------------------------------------------------------------- remember where each value lives

_offsets: dict[str, tuple[int, str]] = {}
_orig_reader = MTDParam.from_mtd_reader.__func__
_FMT = {"int": "i", "int2": "2i", "float": "f", "float2": "2f", "float3": "3f", "float4": "4f"}


def _reader_with_offsets(cls, reader):
    param = _orig_reader(cls, reader)
    fmt = _FMT.get(param.param_type.value)
    if fmt:
        # value is followed by the 0x04 marker byte, align to 4, then 4 pad bytes
        _offsets[param.name] = (reader.position - 8 - struct.calcsize("<" + fmt), fmt)
    return param


MTDParam.from_mtd_reader = classmethod(_reader_with_offsets)

# ---------------------------------------------------------------- rules

WATER_SHADERS = ("FRPG_Water_Reflect.spx", "FRPG_Water_Env.spx")


def water(name: str, p: dict) -> dict:
    """New values for one water material, or {} to leave it vanilla."""
    scale, bias, power = p["g_FresnelScale"], p["g_FresnelBias"], p["g_FresnelPow"]
    # Reflection switched off on purpose (lava, opaque pools, "No reflect" variants): keep.
    if scale <= 0.0 or "lava" in name.lower() or "no reflect" in name.lower():
        return {}
    new = {
        # stronger reflection, but never past full mirror (or past a vanilla value already above it)
        "g_FresnelScale": min(scale * 1.3, max(1.0, scale)),
        # a little reflection even looking straight down
        "g_FresnelBias": min(bias + 0.05, max(1.0, bias)),
    }
    # lower exponent -> the reflective rim reaches further toward the camera
    if power > 1.0:
        new["g_FresnelPow"] = max(1.0, power * 0.8)
    # sharper sun/torch highlights on the surface
    if p.get("g_SpecularPower", 0.0) >= 10.0:
        new["g_SpecularPower"] = p["g_SpecularPower"] * 1.5
    return new


def patch_file(src: Path, dst: Path) -> int:
    binder = Binder.from_path(src)
    touched = 0
    for entry in binder.entries:
        _offsets.clear()
        mtd = MTD.from_bytes(entry.data)
        if mtd.shader_path.split("\\")[-1] not in WATER_SHADERS:
            continue
        values = {p.name: p.value for p in mtd.params}
        new = water(entry.name, values)
        if not new:
            continue
        data = bytearray(entry.data)
        for name, value in new.items():
            off, fmt = _offsets[name]
            assert struct.unpack_from("<" + fmt, data, off)[0] == struct.unpack("<" + fmt, struct.pack("<" + fmt, values[name]))[0], name
            struct.pack_into("<" + fmt, data, off, value)
        entry.data = bytes(data)
        touched += 1
        print(f"  {entry.name}: " + ", ".join(f"{k[2:]} {values[k]:.3g}->{v:.3g}" for k, v in new.items()))
    dst.parent.mkdir(parents=True, exist_ok=True)
    binder.write(dst)
    return touched


def main():
    src_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    for src in sorted(src_dir.glob("*.mtdbnd.dcx")):
        n = patch_file(src, out_dir / src.name)
        print(f"{src.name}: {n} water materials")


if __name__ == "__main__":
    main()
