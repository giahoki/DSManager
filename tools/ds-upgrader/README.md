# DS Upgrader: building the engine patches

Sources of the `.dsmpatch` files in `mods\Dark Souls Remastered\DS Upgrader\patches\`. The ReShade part of the mod (`files\`) is not built here.

| Script | Game files | What it changes |
|---|---|---|
| `patch_drawparam.py` | `param\DrawParam\*.parambnd.dcx` | shadows, point lights, cubemap reflections (LightBank SpecularLight), engine bloom, particle FX brightness (ToneCorrect SFXMultiplier), flares, god rays, LOD, per-area fog and colour |
| `patch_mtd.py` | `mtd\*.mtdbnd.dcx` | water reflections in 46 water materials (Fresnel scale/bias/power, specular power) |

## Setup

soulstruct from git needs Python 3.13. The PyPI wheel lacks data files.

```
py -3.13 -m venv %USERPROFILE%\dsr-venv
%USERPROFILE%\dsr-venv\Scripts\python -m pip install git+https://github.com/Grimrukh/soulstruct
set PYTHONIOENCODING=utf-8
```

## Build

The input must be the original game files (verify integrity in Steam first, with every mod off in DSM).

```
set G=C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED
python patch_drawparam.py "%G%\param\DrawParam" out\DrawParam
python patch_mtd.py "%G%\mtd" out\mtd
```

Then, for each file: `DSM.exe --make-patch "<original>" "out\...\<file>" "<mod>\patches\<same path>.dsmpatch"`, and bump `version` in `mod.json`.

## Engine facts

- `ENV_LIGHT_TEX_BANK` has `isUse = 0` in every area, so it does nothing. Engine reflections come from `LIGHT_BANK.SpecularLight*`.
- `TONE_CORRECT_BANK.SFXMultiplier` is 1.0 everywhere in vanilla.
- In `default` and `a99`, the ToneMap/ToneCorrect rows are shorter than the paramdef (vanilla data). The script leaves them untouched.
- soulstruct can read MTDs but cannot write them (`MTD.to_writer` is broken), so `patch_mtd.py` rewrites float values in place. The size stays the same, which `dsm-patch-1` requires.
