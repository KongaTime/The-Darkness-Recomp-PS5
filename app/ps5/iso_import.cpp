#include "iso_import.h"
#include "runtime/native/xex_crypto_portable.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace DarkRecomp::Ps5 {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t sector = 2048, maxBytes = 16ull << 30;
constexpr size_t maxEntries = 200000;
constexpr const char* stagingOwner = "The Darkness ISO importer v1\n";
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
uint32_t le32(const uint8_t* b) {
    return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24;
}
uint16_t le16(const uint8_t* b) { return uint16_t(b[0]) | uint16_t(b[1])<<8; }
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    return s;
}
bool pathExists(const fs::path& p) { return fs::symlink_status(p).type() != fs::file_type::not_found; }
void regular(const fs::path& p) {
    require(fs::symlink_status(p).type() == fs::file_type::regular, "Expected a regular file; symlinks are not supported");
}
class Image {
    std::ifstream file;
public:
    uint64_t size;
    explicit Image(const fs::path& p) : file(p, std::ios::binary), size(fs::file_size(p)) {
        regular(p); require(bool(file), "Cannot open ISO");
    }
    void read(uint64_t offset, std::span<uint8_t> data) {
        require(offset <= size && data.size() <= size-offset, "ISO contains an out-of-bounds extent");
        file.clear(); file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        require(bool(file), "ISO read failed or was truncated");
    }
};
void safeName(const std::string& s) {
    require(!s.empty() && s != "." && s != "..", "ISO contains an unsafe filename");
    for (unsigned char c : s)
        require(c >= 32 && c != 127 && c != '/' && c != '\\' && c != ':', "ISO contains an unsafe filename");
}
void directory(const fs::path& p) {
    if (!pathExists(p)) fs::create_directory(p);
    require(fs::symlink_status(p).type() == fs::file_type::directory, "Import path is not a real directory");
}
}

IsoPlan inspectGameIso(const fs::path& path, const VerifyExecutable& verify) {
    require(bool(verify), "Executable verification is required");
    Image image(path);
    uint64_t base = 0;
    std::array<uint8_t,2048> header{};
    bool found = false;
    // XDVDFS partition offsets used for XISO, XGD2, XGD3 and XGD1 images.
    // Format reference: XboxDev/extract-xiso (offsets, media descriptor, entry layout).
    for (uint64_t candidate : {0ull, 0x0fd90000ull, 0x02080000ull, 0x18300000ull}) {
        if (candidate + 0x10000 + header.size() > image.size) continue;
        image.read(candidate + 0x10000, header);
        const std::string magic = "MICROSOFT*XBOX*MEDIA";
        if (std::equal(magic.begin(),magic.end(),header.begin()) &&
            std::equal(magic.begin(),magic.end(),header.end()-magic.size())) {
            base = candidate; found = true; break;
        }
    }
    require(found, "Not a supported Xbox disc ISO (XDVDFS partition not found)");
    IsoPlan plan; plan.source = path; plan.sourceSize = image.size;
    struct Pending { fs::path path; uint32_t sector, size; unsigned depth; };
    std::vector<Pending> dirs{{{},le32(header.data()+20),le32(header.data()+24),0}};
    std::set<std::pair<uint32_t,uint32_t>> tables;
    std::set<std::string> paths;
    while (!dirs.empty()) {
        auto dir = dirs.back(); dirs.pop_back();
        require(dir.depth <= 64, "ISO directories are too deeply nested");
        if (!dir.size) continue;
        require(dir.size <= 16*1024*1024 && tables.emplace(dir.sector,dir.size).second,
                "Invalid, repeated, or oversized ISO directory");
        std::vector<uint8_t> table(dir.size);
        image.read(base + uint64_t(dir.sector)*sector, table);
        std::vector<uint32_t> nodes{0};
        std::map<uint32_t,uint32_t> occupied;
        while (!nodes.empty()) {
            uint32_t at = nodes.back(); nodes.pop_back();
            require(at <= table.size() && table.size()-at >= 14, "Truncated ISO directory entry");
            const auto* b = table.data()+at;
            const uint32_t end = at+14+b[13];
            require(end <= table.size(), "Truncated ISO filename");
            auto next = occupied.lower_bound(at);
            require((next == occupied.end() || next->first >= end) &&
                    (next == occupied.begin() || std::prev(next)->second <= at),
                    "Cyclic or overlapping ISO directory entries");
            occupied.emplace(at,end);
            for (uint32_t child : {uint32_t(le16(b))*4, uint32_t(le16(b+2))*4})
                if (child) nodes.push_back(child);
            std::string name(reinterpret_cast<const char*>(b+14),b[13]);
            safeName(name);
            if (dir.path.empty() && lower(name) == "$systemupdate") continue;
            if (dir.path.empty() && lower(name) == "default.xex") name = "default.xex";
            auto relative = dir.path/name;
            require(relative.generic_string().size() <= 1024 && paths.insert(lower(relative.generic_string())).second,
                    "Duplicate or excessively long ISO path");
            const uint32_t extent = le32(b+4), size = le32(b+8);
            const bool isDir = (b[12]&0x10) != 0;
            const uint64_t offset = base+uint64_t(extent)*sector;
            require(offset <= image.size && size <= image.size-offset, "ISO contains an out-of-bounds file");
            require(plan.entries.size() < maxEntries, "ISO contains too many files");
            plan.entries.push_back({relative,offset,size,isDir});
            if (isDir) dirs.push_back({relative,extent,size,dir.depth+1});
            else {
                require(size <= maxBytes-plan.bytes, "ISO extraction is too large");
                plan.bytes += size;
            }
        }
    }
    const auto xex = std::find_if(plan.entries.begin(),plan.entries.end(),[](const IsoEntry& e) {
        return e.path == "default.xex" && !e.directory;
    });
    require(xex != plan.entries.end(), "ISO has no root default.xex");
    require(xex->size >= 24 && xex->size <= 64*1024*1024, "Invalid game executable size");
    std::vector<uint8_t> executable(xex->size);
    image.read(xex->offset, executable);
    verify(executable);
    plan.executableHash = Native::XexCrypto::Sha256Hex(executable);
    return plan;
}

void extractGameIso(const IsoPlan& plan, const fs::path& destination, const ImportProgress& progress) {
    require(!destination.filename().empty(), "Invalid import destination");
    const auto parent = destination.parent_path();
    require(fs::is_directory(parent), "Import destination parent does not exist");
    require(!pathExists(destination) || (fs::symlink_status(destination).type() == fs::file_type::directory && fs::is_empty(destination)),
            "Game folder already contains files; it will not be overwritten");
    Image image(plan.source);
    require(image.size == plan.sourceSize, "ISO changed after inspection");
    std::error_code ec;
    const auto space = fs::space(parent,ec);
    if (!ec) require(space.available >= plan.bytes + (64ull<<20), "Not enough free space to extract the ISO");
    fs::path stage = destination; stage += ".importing";
    const auto owner = stage/".darkness-import-owner";
    if (pathExists(stage)) {
        require(fs::symlink_status(stage).type() == fs::file_type::directory, "Import staging path is not a directory");
        regular(owner);
        require(fs::file_size(owner) <= 128, "Invalid import staging marker");
        std::ifstream in(owner);
        std::string tag((std::istreambuf_iterator<char>(in)),{});
        require(tag == stagingOwner, "Unrecognized staging folder; it will not be modified");
        // Only our own interrupted staging tree is removed. remove_all does not follow symlinks.
        fs::remove_all(stage);
    }
    directory(stage);
    { std::ofstream out(owner); out << stagingOwner; out.close(); require(bool(out), "Cannot create import marker"); }
    std::vector<uint8_t> buffer(1<<20);
    uint64_t done = 0;
    for (const auto& e : plan.entries) {
        fs::path target = stage;
        for (const auto& part : e.path.parent_path()) { target /= part; directory(target); }
        target /= e.path.filename();
        if (e.directory) { directory(target); continue; }
        require(!pathExists(target), "Conflicting extraction path");
        std::ofstream out(target,std::ios::binary);
        require(bool(out), "Cannot create extracted file");
        for (uint64_t offset = 0; offset < e.size;) {
            size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(),e.size-offset));
            image.read(e.offset+offset,std::span(buffer).first(count));
            out.write(reinterpret_cast<char*>(buffer.data()),count);
            require(bool(out), "File write failed (disk full or storage unavailable)");
            offset += count; done += count;
            if (progress) progress(done,plan.bytes,e.path.generic_string());
        }
        out.close(); require(bool(out), "Cannot finish extracted file");
    }
    // Detect a changed ISO before publishing, including same-size edits to default.xex.
    {
        const auto path = stage/"default.xex";
        require(fs::file_size(path) <= 64*1024*1024, "Invalid extracted executable");
        std::ifstream in(path,std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
        require(!in.bad() && Native::XexCrypto::Sha256Hex(bytes) == plan.executableHash,
                "ISO changed during extraction; executable verification failed");
    }
    // Owner stays inside the published tree: a power loss before rename never looks installed.
    if (pathExists(destination)) fs::remove(destination); // previously verified empty, never recursive
    fs::rename(stage,destination);
    if (progress) progress(plan.bytes,plan.bytes,"Import complete");
}

std::vector<fs::path> findGameIsos(const std::vector<fs::path>& roots) {
    std::vector<fs::path> result;
    for (const auto& root : roots) {
        std::error_code ec;
        fs::directory_iterator it(root,ec), end;
        while (!ec && it != end) {
            const auto path = it->path();
            if (lower(path.extension().string()) == ".iso" && it->symlink_status(ec).type() == fs::file_type::regular)
                result.push_back(path);
            if (result.size() >= 64) break;
            it.increment(ec);
        }
        if (result.size() >= 64) break;
    }
    std::sort(result.begin(),result.end());
    result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
}
