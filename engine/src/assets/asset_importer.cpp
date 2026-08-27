#include "neon/assets/asset_importer.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "neon/assets/image_decode.hpp"

namespace neon::assets {

namespace {

constexpr char kBc1Magic[4] = {'N', 'B', 'C', '1'};

bool HasImageSuffix(const std::string& p) {
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = p.substr(dot);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
}

} // namespace

bool ReadBakedTexture(const std::string& path, int& width, int& height,
                      std::vector<uint8_t>& bc1) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    const std::streamsize size = in.tellg();
    if (size < 12) return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (std::memcmp(bytes.data(), kBc1Magic, 4) != 0) return false;
    width = static_cast<int>(bytes[4]) | (static_cast<int>(bytes[5]) << 8) |
            (static_cast<int>(bytes[6]) << 16) | (static_cast<int>(bytes[7]) << 24);
    height = static_cast<int>(bytes[8]) | (static_cast<int>(bytes[9]) << 8) |
             (static_cast<int>(bytes[10]) << 16) | (static_cast<int>(bytes[11]) << 24);
    if (width <= 0 || height <= 0) return false;
    bc1.assign(bytes.begin() + 12, bytes.end());
    return !bc1.empty();
}

bool WriteBakedTexture(const std::string& path, int width, int height,
                       const std::vector<uint8_t>& bc1) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(kBc1Magic, 4);
    uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
    out.write(reinterpret_cast<const char*>(&w), 4);
    out.write(reinterpret_cast<const char*>(&h), 4);
    if (!bc1.empty())
        out.write(reinterpret_cast<const char*>(bc1.data()),
                  static_cast<std::streamsize>(bc1.size()));
    return out.good();
}

ImportReport ImportProjectTextures(const std::string& projectDir) {
    ImportReport report;
    const std::string assetsDir = projectDir + "/assets";
    const std::string cacheDir = projectDir + "/import_cache";
    std::error_code ec;
    if (!std::filesystem::exists(assetsDir, ec)) return report;
    std::filesystem::create_directories(cacheDir, ec);

    for (std::filesystem::recursive_directory_iterator it(assetsDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string abs = it->path().string();
        if (!HasImageSuffix(abs)) continue;
        const std::string rel = std::filesystem::relative(it->path(), projectDir).string();
        std::ifstream in(abs, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            report.errors.push_back("cannot open '" + abs + "'");
            continue;
        }
        const std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), size);

        DecodedImage img = DecodeImageBytes(bytes, /*compressBc1=*/true,
                                            /*flipVertically=*/false);
        if (img.bc1.empty()) {
            ++report.skippedCount; // alpha-bearing / unreadable -> stays source
            continue;
        }
        const std::string outPath = cacheDir + "/" + rel + ".nbc1";
        std::filesystem::create_directories(
            std::filesystem::path(outPath).parent_path(), ec);
        if (WriteBakedTexture(outPath, img.width, img.height, img.bc1)) {
            ++report.bakedCount;
        } else {
            report.errors.push_back("cannot write '" + outPath + "'");
        }
    }
    return report;
}

} // namespace neon::assets
