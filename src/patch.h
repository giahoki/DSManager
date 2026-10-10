#pragma once
#include "util.h"

#include <string>
#include <vector>

// Binary patches that carry only a mod's own changes, so a mod can be shared without the game's files.
// A .dsmpatch (JSON) lists byte edits against the original file; DCX archives are patched in their
// unpacked form and packed again, so the patch stays a few bytes even though the archive is compressed.
//
//   mods/<game>/<mod>/patches/param/DrawParam/a10_DrawParam.parambnd.dcx.dsmpatch
//     -> applied to <game>/param/DrawParam/a10_DrawParam.parambnd.dcx

using Bytes = std::vector<unsigned char>;

constexpr const char* kPatchExt = ".dsmpatch";

Bytes readBytes(const fs::path& p);
bool writeBytes(const fs::path& p, const Bytes& data);
uint64_t hashBytes(const Bytes& data);

// DCX "DFLT" (zlib) containers used by FromSoftware games.
bool dcxUnpack(const Bytes& file, Bytes& header, Bytes& data, std::string& err);
Bytes dcxPack(const Bytes& header, const Bytes& data);

// original + modified -> patch json; original + patch -> modified
bool makePatch(const Bytes& original, const Bytes& modified, std::string& patchJson, std::string& err);
bool applyPatch(const std::string& patchJson, const Bytes& original, Bytes& result, std::string& err);
