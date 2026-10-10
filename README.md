# DSM — Dark Souls Mod Manager

On launch DSM opens a world selection screen: **Dark Souls Remastered**, **Dark Souls III** and **Elden Ring** (Steam). Each game has its own mods (`mods\<game>`) and its own backup (`data\<id>`).

Dark Souls III and Elden Ring are protected by EasyAntiCheat: mods for them are for offline play only. Elden Ring only reads loose files through ModEngine 2, so its support is still experimental.

The interface is in Russian; the labels below are given in English with the Russian text in parentheses.

DSM is portable: it is a single `DSM.exe` (fonts and images are embedded). Everything it creates lives next to it; nothing is written to AppData or the registry:

```
DSM\
├─ DSM.exe
├─ data\    what is enabled, what is installed, backups of the original game files
└─ mods\    mods, per game
```

The folder can be moved as a whole.

When built from source, the exe ends up in `bin\`:

- `mods\` lives in the project root and is copied to `bin\mods` on every build. Put new mods into the root `mods\`: the copy in `bin\` is lost when the build is cleaned.
- `data\` lives next to the exe (`bin\data`). "Clean Solution" deletes `bin\` and `build\` entirely, including `data\`. Before cleaning, press "Restore original" ("вернуть оригинал") in DSM, otherwise the backups of the original game files are lost with it.

## Adding a mod

```
mods\
└─ Dark Souls Remastered\
   └─ Mod name\
      ├─ mod.json        name, version, author, description (optional)
      └─ files\          everything here is copied into the game folder under the same paths
```

For example, `files\param\DrawParam\a10_DrawParam.parambnd.dcx` replaces `param\DrawParam\a10_DrawParam.parambnd.dcx` in the game, and `files\dxgi.dll` goes to the game's root folder.

`mod.json`:

```json
{
  "name": "Name",
  "version": "1.0",
  "author": "Author",
  "description": "What the mod does."
}
```

After adding a mod, press "refresh list" ("обновить список") in the manager.

### Patches instead of game files

So that a mod can be shared without any game files, a modified file can be replaced by a patch: it holds only your edits, and when applying, DSM takes the original file from the player's game and builds the modded one from it. DCX archives are patched unpacked, so the patch stays small.

```
mods\<game>\<mod>\patches\param\DrawParam\a10_DrawParam.parambnd.dcx.dsmpatch
```

To make a patch: `DSM.exe --make-patch <original> <modified> <file.dsmpatch>`. If the player has a different version of the file, DSM reports that the patch does not fit it and breaks nothing.

## Using it

- **The bonfire to the left of a mod** turns it on and off. On the right is its state: "lit" ("зажжён") — the mod is in the game, "faded" ("угас") — it is off, "awaiting apply" ("ждёт применения") / "still in game" ("ещё в игре") — the changes have not been applied yet.
- **The arrows** (shown on hover) change the order. If two mods change the same file, the one lower in the list wins; such mods are marked as "conflict" ("конфликт").
- **The main button** follows the state. With pending changes it is **Apply** ("применить"): installs the checked mods and removes the unchecked ones. Otherwise it acts on the mod selected in the list: **Install** ("установить"), **Remove** ("удалить") if it is in the game, or **Update** ("обновить") if the mod folder holds a newer version than the one installed.
- **Updates**: DSM remembers which version of each mod it installed. When a mod's `mod.json` gets another `version`, the mod is marked "update available" ("есть обновление") until you update it.
- **Restore original** ("вернуть оригинал") rolls every game file back to what it was before the mods and turns all mods off.
- **Play** ("играть") launches the game through Steam.
- The game card at the top: if the game was not found automatically, click it and pick the folder with `DarkSoulsRemastered.exe`.

The game must be closed while the manager applies or restores mods.

## Backups

Before replacing a game file, the manager copies the original to `data\dsr\backup\` (under the same paths as in the game). A file that already matches the modded one is not saved as an original. If Steam updated or restored a file, the manager notices and does not overwrite the new version with the old backup.

The state (what is enabled, what is installed) is kept in `data\dsr\state.json`.

## Development

- Visual Studio 2026, project `DSM.vcxproj` (x64, Release / Debug).
- GLFW 3.4 + OpenGL 3.3, the interface is drawn from scratch: an SDF shader for panels, frames and glow, fonts via stb_truetype.
- A new game is added with one line in `games()` in `src/game.cpp`.
- Fonts and logos are embedded into the exe as resources (`src/DSM.rc`, sources in `assets/`). New logo: a file in `assets/icons`, an `RCDATA` line in `DSM.rc` and its name in the game's `logo` field.
- The version is set in `src/version.h`; the installer (`DSM.iss`, Inno Setup 6) reads it from the built exe and puts `DSM-Setup-<version>.exe` into `dist\`.
- Command line for testing: `DSM.exe --cli status|apply|restore|enable <mod>|disable <mod> [--game dsr|ds3|er]`.
- `--game <id>` opens a game right away, skipping the selection screen.
- Screenshot without a window: `DSM.exe --screenshot out.png --shot-time 3`.

## Third-party components

- GLFW — zlib/libpng license
- stb_truetype, stb_image, stb_image_write — public domain / MIT
- nlohmann/json — MIT
- miniz — MIT
- Cormorant Garamond and Cormorant SC fonts — SIL Open Font License
- in the DS Upgrader mod: ReShade — BSD-3-Clause; SMAA and CAS from SweetFX, Glamarye Fast Effects, prod80 shaders, Deband — MIT

The full texts of all these licenses are in `THIRD-PARTY-LICENSES.md`. If you distribute DS Upgrader separately from DSM, include that file.

## License

DSM's code is licensed under **PolyForm Noncommercial 1.0.0** (`LICENSE.md`):

- you may use, study, modify, build on and share it — for noncommercial purposes;
- you may not sell DSM or works based on it, or use them for commercial purposes;
- when distributing, keep `LICENSE.md` and the `Required Notice` line naming the author.

The license covers only DSM's own code and design. Third-party components keep their own licenses (listed above). Game names and logos (for example, `assets/icons/DS R logo.png`) belong to FromSoftware and Bandai Namco and are not covered by this license.
