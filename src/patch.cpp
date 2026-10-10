#include "patch.h"

#include "i18n.h"

#include <cstring>
#include <fstream>
#include <json.hpp>
#include <miniz.h>

using json = nlohmann::json;

Bytes readBytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

bool writeBytes(const fs::path& p, const Bytes& data)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return bool(f);
}

uint64_t hashBytes(const Bytes& data)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// ---------------------------------------------------------------- DCX

static uint32_t be32(const Bytes& b, size_t at)
{
    return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3];
}

static void putBe32(Bytes& b, size_t at, uint32_t v)
{
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
}

// Layout (all big-endian): "DCX\0" ... "DCS\0" @0x18 {unpacked size @0x1C, packed size @0x20}
// "DCP\0" "DFLT" @0x24 ... "DCA\0" {header size 8} then the zlib stream.
static const size_t kUnpackedAt = 0x1C, kPackedAt = 0x20;

bool dcxUnpack(const Bytes& file, Bytes& header, Bytes& data, std::string& err)
{
    if (file.size() < 0x50 || std::memcmp(file.data(), "DCX\0", 4) != 0) { err = tr("не DCX", "not a DCX"); return false; }
    if (std::memcmp(file.data() + 0x28, "DFLT", 4) != 0) { err = tr("DCX не в формате DFLT", "DCX is not DFLT"); return false; }
    size_t dca = 0;
    for (size_t i = 0x2C; i + 8 <= std::min<size_t>(file.size(), 0x100); ++i)
        if (std::memcmp(file.data() + i, "DCA\0", 4) == 0) { dca = i; break; }
    if (!dca) { err = tr("в DCX нет блока DCA", "DCX has no DCA block"); return false; }
    size_t start = dca + be32(file, dca + 4);
    if (start > file.size()) { err = tr("повреждённый DCX", "damaged DCX"); return false; }

    header.assign(file.begin(), file.begin() + start);
    data.resize(be32(file, kUnpackedAt));
    mz_ulong outLen = (mz_ulong)data.size();
    int rc = mz_uncompress(data.data(), &outLen, file.data() + start, (mz_ulong)(file.size() - start));
    if (rc != MZ_OK || outLen != data.size()) { err = tr("не удалось распаковать DCX", "couldn't unpack the DCX"); return false; }
    return true;
}

Bytes dcxPack(const Bytes& header, const Bytes& data)
{
    mz_ulong cap = mz_compressBound((mz_ulong)data.size());
    Bytes out(header.size() + cap);
    std::memcpy(out.data(), header.data(), header.size());
    mz_ulong packed = cap;
    mz_compress2(out.data() + header.size(), &packed, data.data(), (mz_ulong)data.size(), 9);
    out.resize(header.size() + packed);
    putBe32(out, kUnpackedAt, (uint32_t)data.size());
    putBe32(out, kPackedAt, (uint32_t)packed);
    return out;
}

// ---------------------------------------------------------------- patches

static std::string toHex(const unsigned char* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 15];
    }
    return s;
}

static bool fromHex(const std::string& s, Bytes& out)
{
    if (s.size() % 2) return false;
    out.resize(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        out[i] = uint8_t(v(s[i * 2]) << 4 | v(s[i * 2 + 1]));
    }
    return true;
}

static bool isDcx(const Bytes& b) { return b.size() >= 4 && std::memcmp(b.data(), "DCX\0", 4) == 0; }

bool makePatch(const Bytes& original, const Bytes& modified, std::string& patchJson, std::string& err)
{
    bool dcx = isDcx(original) && isDcx(modified);
    Bytes hdrA, hdrB, a = original, b = modified;
    if (dcx && (!dcxUnpack(original, hdrA, a, err) || !dcxUnpack(modified, hdrB, b, err))) return false;
    if (a.size() != b.size()) {
        err = tr("размеры файлов отличаются: такие патчи пока не поддерживаются", "the files differ in size: such patches aren't supported yet");
        return false;
    }

    json edits = json::array();
    for (size_t i = 0; i < a.size();) {
        if (a[i] == b[i]) { ++i; continue; }
        size_t start = i, end = i + 1, same = 0;
        for (size_t j = end; j < a.size() && same <= 8; ++j) {  // join edits separated by a few equal bytes
            if (a[j] != b[j]) { end = j + 1; same = 0; }
            else ++same;
        }
        edits.push_back({start, toHex(b.data() + start, end - start)});
        i = end;
    }

    json j;
    j["format"] = "dsm-patch-1";
    j["dcx"] = dcx;
    j["size"] = a.size();
    j["base"] = hashHex(hashBytes(a));
    j["result"] = hashHex(hashBytes(b));
    j["edits"] = edits;
    patchJson = j.dump(1);
    return true;
}

bool applyPatch(const std::string& patchJson, const Bytes& original, Bytes& result, std::string& err)
{
    json j = json::parse(patchJson, nullptr, false);
    if (j.is_discarded() || j.value("format", "") != "dsm-patch-1") { err = tr("не патч DSM", "not a DSM patch"); return false; }

    bool dcx = j.value("dcx", false);
    Bytes header, data = original;
    if (dcx && !dcxUnpack(original, header, data, err)) return false;
    if (hashHex(hashBytes(data)) == j.value("result", "")) {  // already patched
        result = original;
        return true;
    }
    if (data.size() != j.value("size", size_t(0)) || hashHex(hashBytes(data)) != j.value("base", "")) {
        err = tr("файл игры изменён или другой версии — проверь целостность файлов в Steam", "the game file is changed or another version: verify the game files in Steam");
        return false;
    }

    Bytes chunk;
    for (auto& e : j["edits"]) {
        size_t at = e[0].get<size_t>();
        if (!fromHex(e[1].get<std::string>(), chunk) || at + chunk.size() > data.size()) {
            err = tr("повреждённый патч", "damaged patch");
            return false;
        }
        std::memcpy(data.data() + at, chunk.data(), chunk.size());
    }
    if (hashHex(hashBytes(data)) != j.value("result", "")) { err = tr("патч применился неправильно", "the patch applied wrongly"); return false; }

    result = dcx ? dcxPack(header, data) : data;
    return true;
}
