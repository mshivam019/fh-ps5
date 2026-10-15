// Linux developer tool: LD_PRELOAD into the Forza Linux build to write every
// distinct small 2D texture the game loads as a DDS file named after its content
// hash (the hash the PS5 app's prompt table uses). FORZA_TEXDUMP_DIR selects the
// directory. Never publish the output: it is game artwork.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>

namespace {
using rex::graphics::TextureCache;
using rex::graphics::xenos::TextureFormat;
std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped;

struct BlockFormat {
  TextureFormat format;
  uint32_t block_size;
  uint32_t bytes_per_block;
  const char* four_cc;  // nullptr: 32-bit RGBA
};
constexpr BlockFormat kFormats[] = {
    {TextureFormat::k_5_6_5, 1, 2, nullptr},
    {TextureFormat::k_8_8_8_8, 1, 4, nullptr},
    {TextureFormat::k_DXT1, 4, 8, "DXT1"},
    {TextureFormat::k_DXT2_3, 4, 16, "DXT3"},
    {TextureFormat::k_DXT4_5, 4, 16, "DXT5"},
};

// Guest offsets of each block of the base level, row-major.
struct BlockLayout {
  const BlockFormat* format;
  uint32_t blocks_x, blocks_y;
  std::vector<uint32_t> offsets;
};

bool GetLayout(const TextureCache::GuestTextureLoad& load, BlockLayout& layout) {
  if (load.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked || load.base_packed) {
    return false;
  }
  layout.format = nullptr;
  for (const BlockFormat& candidate : kFormats) {
    if (candidate.format == load.format) layout.format = &candidate;
  }
  if (!layout.format) return false;
  const uint32_t block = layout.format->block_size, bpb = layout.format->bytes_per_block;
  const uint32_t bpb_log2 = bpb == 2 ? 1 : bpb == 4 ? 2 : bpb == 8 ? 3 : 4;
  layout.blocks_x = (load.width + block - 1) / block;
  layout.blocks_y = (load.height + block - 1) / block;
  const uint32_t pitch_blocks = load.row_pitch_bytes / bpb;
  layout.offsets.resize(size_t(layout.blocks_x) * layout.blocks_y);
  for (uint32_t y = 0; y < layout.blocks_y; ++y) {
    for (uint32_t x = 0; x < layout.blocks_x; ++x) {
      const uint32_t offset =
          load.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                           int32_t(x), int32_t(y), pitch_blocks, bpb_log2))
                     : y * load.row_pitch_bytes + x * bpb;
      if (offset + bpb > load.base_size_bytes) return false;
      layout.offsets[size_t(y) * layout.blocks_x + x] = offset;
    }
  }
  return true;
}

// Guest blocks <-> host-endian linear blocks (the swap is its own inverse).
void CopyBlocks(const TextureCache::GuestTextureLoad& load, const BlockLayout& layout,
                uint8_t* linear, bool to_guest) {
  const uint32_t bpb = layout.format->bytes_per_block;
  for (size_t i = 0; i < layout.offsets.size(); ++i) {
    uint8_t* guest = load.guest_data + layout.offsets[i];
    uint8_t* host = linear + i * bpb;
    rex::graphics::texture_conversion::CopySwapBlock(load.endianness, to_guest ? guest : host,
                                                     to_guest ? host : guest, bpb);
  }
}

void Put32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

void Dump(const TextureCache::GuestTextureLoad& load, const BlockLayout& layout,
          const std::vector<uint8_t>& linear, uint64_t hash, const std::string& dir) {
  {
    std::lock_guard lock(g_dump_mutex);
    if (g_dumped.empty()) {
      std::error_code error;
      std::filesystem::create_directories(dir, error);
    }
    if (!g_dumped.insert(hash).second) return;
  }
  std::vector<uint8_t> out(128 + linear.size());
  std::memcpy(out.data() + 128, linear.data(), linear.size());
  std::memcpy(out.data(), "DDS ", 4);
  Put32(out, 4, 124);
  Put32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000);
  Put32(out, 12, load.height);
  Put32(out, 16, load.width);
  Put32(out, 76, 32);
  if (layout.format->four_cc) {
    Put32(out, 80, 0x4);
    std::memcpy(out.data() + 84, layout.format->four_cc, 4);
  } else if (layout.format->bytes_per_block == 2) {
    Put32(out, 80, 0x40);
    Put32(out, 88, 16);
    Put32(out, 92, 0xF800);
    Put32(out, 96, 0x07E0);
    Put32(out, 100, 0x001F);
  } else {
    Put32(out, 80, 0x1 | 0x40);
    Put32(out, 88, 32);
    Put32(out, 92, 0x000000FF);
    Put32(out, 96, 0x0000FF00);
    Put32(out, 100, 0x00FF0000);
    Put32(out, 104, 0xFF000000);
  }
  Put32(out, 108, 0x1000);
  char name[96];
  std::snprintf(name, sizeof(name), "/%016llx_%u_%ux%u.dds", static_cast<unsigned long long>(hash),
                unsigned(load.format), load.width, load.height);
  if (FILE* file = std::fopen((dir + name).c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
  }
}

bool OnGuestTextureLoad(const TextureCache::GuestTextureLoad& load) {
  static const std::string dir = [] {
    const char* v = std::getenv("FORZA_TEXDUMP_DIR");
    return std::string(v ? v : "");
  }();
  if (dir.empty() || load.width > 1280 || load.height > 1280) return false;
  thread_local BlockLayout layout;
  thread_local std::vector<uint8_t> linear;
  if (!GetLayout(load, layout)) return false;
  linear.resize(layout.offsets.size() * layout.format->bytes_per_block);
  CopyBlocks(load, layout, linear.data(), false);
  Dump(load, layout, linear, XXH3_64bits(linear.data(), linear.size()), dir);
  return false;
}

__attribute__((constructor)) void Register() { TextureCache::SetGuestTextureLoadHook(OnGuestTextureLoad); }
}  // namespace
