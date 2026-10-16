// PlayStation button prompts for title 4D5309C9 (Forza Horizon).
//
// The game draws each Xbox button prompt from its own small DXT5 texture. The
// texture cache calls OnGuestTextureLoad before it uploads a guest texture;
// textures whose untiled contents match a known Xbox prompt are redrawn in
// guest memory as the matching PlayStation glyph, which is drawn here (no game
// or console artwork is shipped).
//
// With forza_texture_dump_dir set, every distinct 2D texture in a supported
// format is also written there as a DDS file named after the same content hash,
// for identifying more prompts.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(forza_playstation_prompts, true, "Forza",
                    "Replace the game's Xbox button prompts with PlayStation glyphs");
REXCVAR_DEFINE_STRING(forza_texture_dump_dir, "", "Forza",
                      "Debug: write each distinct loaded 2D texture as DDS into this directory");

namespace {
using rex::graphics::TextureCache;
using rex::graphics::xenos::TextureFormat;

enum class Glyph { kCross, kCircle, kSquare, kTriangle, kL1, kR1, kL2, kR2, kL3, kHome };

struct KnownPrompt {
  uint64_t hash;  // XXH3 of the untiled, host-endian block data
  Glyph glyph;
};
// Xbox prompts seen in the menus, loading screens and free roam (all DXT5).
constexpr KnownPrompt kKnownPrompts[] = {
    {0x516f4b2027ff9c6b, Glyph::kCross},     // A 32x32
    {0xf572e2bd3a590fcd, Glyph::kCircle},    // B 32x32
    {0xc676c5e691ada7de, Glyph::kSquare},    // X 32x32
    {0xdff3bb908d936fad, Glyph::kTriangle},  // Y 32x32
    {0x237c4d1e3b962547, Glyph::kR2},        // RT 32x32
};

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

// ---- Glyph drawing ---------------------------------------------------------

struct Rgba {
  float r, g, b, a;
};

Rgba Over(Rgba top, Rgba bottom) {
  const float a = top.a + bottom.a * (1 - top.a);
  if (a <= 0) return {0, 0, 0, 0};
  auto mix = [&](float t, float b) { return (t * top.a + b * bottom.a * (1 - top.a)) / a; };
  return {mix(top.r, bottom.r), mix(top.g, bottom.g), mix(top.b, bottom.b), a};
}

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
  return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

struct Point {
  float x, y;
};
float PolylineDistance(float px, float py, const Point* points, size_t count) {
  float d = 1e9f;
  for (size_t i = 0; i + 1 < count; ++i) {
    d = std::min(d, SegmentDistance(px, py, points[i].x, points[i].y, points[i + 1].x,
                                    points[i + 1].y));
  }
  return d;
}

constexpr Rgba kCrossBlue{0.49f, 0.70f, 0.91f, 1};
constexpr Rgba kCircleRed{1.00f, 0.40f, 0.40f, 1};
constexpr Rgba kSquarePink{0.85f, 0.60f, 0.77f, 1};
constexpr Rgba kTriangleGreen{0.25f, 0.89f, 0.63f, 1};
constexpr Rgba kLabelGray{0.88f, 0.88f, 0.88f, 1};

// Distance to one character's strokes; the character spans x0..x0+0.36, -0.28..0.28.
float CharDistance(char c, float x0, float x, float y) {
  const float x1 = x0 + 0.36f, top = -0.28f, bottom = 0.28f, mid = 0.0f;
  switch (c) {
    case 'L': {
      const Point l[] = {{x0, top}, {x0, bottom}, {x1, bottom}};
      return PolylineDistance(x, y, l, 3);
    }
    case 'R': {
      const Point bowl[] = {{x0, bottom}, {x0, top}, {x1 - 0.07f, top}, {x1, top + 0.07f},
                            {x1, mid - 0.07f}, {x1 - 0.07f, mid}, {x0, mid}};
      const Point leg[] = {{x0 + 0.14f, mid}, {x1, bottom}};
      return std::min(PolylineDistance(x, y, bowl, 7), PolylineDistance(x, y, leg, 2));
    }
    case '1': {
      const Point one[] = {{x0 + 0.06f, top + 0.10f}, {x0 + 0.20f, top}, {x0 + 0.20f, bottom}};
      const Point base[] = {{x0 + 0.04f, bottom}, {x1 - 0.02f, bottom}};
      return std::min(PolylineDistance(x, y, one, 3), PolylineDistance(x, y, base, 2));
    }
    case '2': {
      const Point two[] = {{x0, top + 0.09f}, {x0 + 0.08f, top}, {x1 - 0.08f, top},
                           {x1, top + 0.09f}, {x1, mid - 0.06f}, {x0, bottom}, {x1, bottom}};
      return PolylineDistance(x, y, two, 7);
    }
  }
  return 1e9f;
}

// A two-character label ("R2"), centred, at 70% of the character box.
float LabelDistance(char first, char second, float x, float y) {
  constexpr float k = 0.7f;
  return k * std::min(CharDistance(first, -0.43f, x / k, y / k),
                      CharDistance(second, 0.07f, x / k, y / k));
}

// Distance from a point (in the glyph's unit box) to its symbol strokes.
float SymbolDistance(Glyph glyph, float x, float y) {
  switch (glyph) {
    case Glyph::kCross: {
      const float s = 0.27f;
      return std::min(SegmentDistance(x, y, -s, -s, s, s), SegmentDistance(x, y, -s, s, s, -s));
    }
    case Glyph::kCircle:
      return std::abs(std::hypot(x, y) - 0.30f);
    case Glyph::kSquare: {
      const float s = 0.26f;
      const Point square[] = {{-s, -s}, {s, -s}, {s, s}, {-s, s}, {-s, -s}};
      return PolylineDistance(x, y, square, 5);
    }
    case Glyph::kTriangle: {
      const Point triangle[] = {{0, -0.33f}, {0.31f, 0.21f}, {-0.31f, 0.21f}, {0, -0.33f}};
      return PolylineDistance(x, y, triangle, 4);
    }
    case Glyph::kL1: return LabelDistance('L', '1', x, y);
    case Glyph::kR1: return LabelDistance('R', '1', x, y);
    case Glyph::kL2: return LabelDistance('L', '2', x, y);
    case Glyph::kR2: return LabelDistance('R', '2', x, y);
    case Glyph::kHome:
      return 1e9f;  // a plain round button
    case Glyph::kL3: {
      // "L3" inside the round button.
      const Point l[] = {{-0.32f, -0.22f}, {-0.32f, 0.22f}, {-0.07f, 0.22f}};
      const Point three_top[] = {{0.07f, -0.22f}, {0.31f, -0.22f}, {0.31f, 0.0f}, {0.12f, 0.0f}};
      const Point three_bottom[] = {{0.31f, 0.0f}, {0.31f, 0.22f}, {0.07f, 0.22f}};
      return std::min({PolylineDistance(x, y, l, 3), PolylineDistance(x, y, three_top, 4),
                       PolylineDistance(x, y, three_bottom, 3)});
    }
  }
  return 1e9f;
}

Rgba SymbolColor(Glyph glyph) {
  switch (glyph) {
    case Glyph::kCross: return kCrossBlue;
    case Glyph::kCircle: return kCircleRed;
    case Glyph::kSquare: return kSquarePink;
    case Glyph::kTriangle: return kTriangleGreen;
    case Glyph::kL1:
    case Glyph::kR1:
    case Glyph::kL2:
    case Glyph::kR2:
    case Glyph::kL3:
    case Glyph::kHome: return kLabelGray;
  }
  return kLabelGray;
}

// One sample in pixel units; the shape fills the texture with a small margin.
Rgba Sample(Glyph glyph, float px, float py, float width, float height) {
  const bool label = glyph == Glyph::kL1 || glyph == Glyph::kR1 || glyph == Glyph::kL2 ||
                     glyph == Glyph::kR2;
  const float size = std::min(width, height);
  const float cx = width * 0.5f, cy = height * 0.5f;
  // Unit coordinates: the shorter side spans -0.5..0.5.
  const float x = (px - cx) / size, y = (py - cy) / size;
  const float half_w = label ? (width / size) * 0.5f - 0.04f : 0.46f;
  const float half_h = 0.46f;
  float shape;  // signed distance to the button outline, negative inside
  if (label) {
    // A pill when the texture is wide, a rounded square (trigger) when square.
    const float r = width > height ? half_h * 0.55f : half_h * 0.35f;
    const float qx = std::max(std::abs(x) - (half_w - r), 0.f);
    const float qy = std::max(std::abs(y) - (half_h - r), 0.f);
    shape = std::hypot(qx, qy) - r;
  } else {
    shape = std::hypot(x, y) - half_w;
  }
  if (shape > 0) return {0, 0, 0, 0};
  // Dark body, lighter towards the top, with a light rim.
  const float shade = 0.10f + 0.12f * std::clamp(0.5f - y, 0.f, 1.f);
  Rgba color{shade, shade, shade, 1};
  if (shape > -0.045f) color = {0.72f, 0.72f, 0.74f, 1};
  const float stroke = label ? 0.055f : glyph == Glyph::kL3 ? 0.05f : 0.06f;
  if (SymbolDistance(glyph, x, y) < stroke) color = SymbolColor(glyph);
  return color;
}

std::vector<Rgba> DrawGlyph(Glyph glyph, uint32_t width, uint32_t height) {
  constexpr int kSamples = 4;
  std::vector<Rgba> pixels(size_t(width) * height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      Rgba sum{0, 0, 0, 0};
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          const Rgba s = Sample(glyph, x + (sx + 0.5f) / kSamples, y + (sy + 0.5f) / kSamples,
                                float(width), float(height));
          sum.r += s.r * s.a;
          sum.g += s.g * s.a;
          sum.b += s.b * s.a;
          sum.a += s.a;
        }
      }
      Rgba& out = pixels[size_t(y) * width + x];
      out.a = sum.a / (kSamples * kSamples);
      if (sum.a > 0) out = {sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, out.a};
    }
  }
  return pixels;
}

// ---- BC3 encoding ----------------------------------------------------------

uint16_t To565(const float c[3]) {
  auto q = [](float v, int max) { return uint32_t(std::lround(std::clamp(v, 0.f, 1.f) * max)); };
  return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}
void From565(uint16_t v, float c[3]) {
  c[0] = float(v >> 11) / 31;
  c[1] = float((v >> 5) & 63) / 63;
  c[2] = float(v & 31) / 31;
}

// Encodes one 4x4 block of straight-alpha pixels as BC3 (host byte order).
void EncodeBc3Block(const Rgba texels[16], uint8_t out[16]) {
  // Alpha: 8-value interpolation between the block's extremes.
  float amin = 1, amax = 0;
  for (int i = 0; i < 16; ++i) amin = std::min(amin, texels[i].a), amax = std::max(amax, texels[i].a);
  const uint8_t a0 = uint8_t(std::lround(amax * 255)), a1 = uint8_t(std::lround(amin * 255));
  out[0] = a0;
  out[1] = a1;
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 16; ++i) {
    uint64_t index = 0;
    if (a0 != a1) {
      const float t = (float(a0) - texels[i].a * 255) / float(a0 - a1);  // 0 at a0, 1 at a1
      const int step = std::clamp(int(std::lround(t * 7)), 0, 7);
      // Palette order: 0 = a0, 1 = a1, 2..7 = interpolated from a0 to a1.
      index = step == 0 ? 0 : step == 7 ? 1 : uint64_t(step + 1);
    }
    alpha_bits |= index << (3 * i);
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(alpha_bits >> (8 * i));

  // Color: endpoints at the extremes along the axis of greatest spread among
  // visible texels; four-color mode (c0 > c1).
  float lo[3] = {1, 1, 1}, hi[3] = {0, 0, 0};
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    if (texels[i].a < 0.02f) continue;
    any = true;
    const float c[3] = {texels[i].r, texels[i].g, texels[i].b};
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
  }
  if (!any) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
  // Pick the diagonal of the bounding box that follows the texels' correlation.
  float axis[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  for (int k = 1; k < 3; ++k) {
    float cov = 0;
    for (int i = 0; i < 16; ++i) {
      if (texels[i].a < 0.02f) continue;
      const float c[3] = {texels[i].r, texels[i].g, texels[i].b};
      cov += (c[0] - (lo[0] + hi[0]) * 0.5f) * (c[k] - (lo[k] + hi[k]) * 0.5f);
    }
    if (cov < 0) std::swap(lo[k], hi[k]), axis[k] = -axis[k];
  }
  uint16_t c0 = To565(hi), c1 = To565(lo);
  if (c0 < c1) std::swap(c0, c1);
  float e0[3], e1[3];
  From565(c0, e0);
  From565(c1, e1);
  uint32_t color_bits = 0;
  if (c0 != c1) {
    float palette[4][3];
    for (int k = 0; k < 3; ++k) {
      palette[0][k] = e0[k];
      palette[1][k] = e1[k];
      palette[2][k] = (2 * e0[k] + e1[k]) / 3;
      palette[3][k] = (e0[k] + 2 * e1[k]) / 3;
    }
    for (int i = 0; i < 16; ++i) {
      const float c[3] = {texels[i].r, texels[i].g, texels[i].b};
      int best = 0;
      float best_d = 1e9f;
      for (int p = 0; p < 4; ++p) {
        const float d = (c[0] - palette[p][0]) * (c[0] - palette[p][0]) +
                        (c[1] - palette[p][1]) * (c[1] - palette[p][1]) +
                        (c[2] - palette[p][2]) * (c[2] - palette[p][2]);
        if (d < best_d) best_d = d, best = p;
      }
      color_bits |= uint32_t(best) << (2 * i);
    }
  }
  std::memcpy(out + 8, &c0, 2);
  std::memcpy(out + 10, &c1, 2);
  std::memcpy(out + 12, &color_bits, 4);
}

void Substitute(const TextureCache::GuestTextureLoad& load, const BlockLayout& layout, Glyph glyph) {
  const std::vector<Rgba> pixels = DrawGlyph(glyph, load.width, load.height);
  std::vector<uint8_t> linear(layout.offsets.size() * 16);
  for (uint32_t by = 0; by < layout.blocks_y; ++by) {
    for (uint32_t bx = 0; bx < layout.blocks_x; ++bx) {
      Rgba texels[16];
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = std::min(bx * 4 + i % 4, load.width - 1);
        const uint32_t y = std::min(by * 4 + i / 4, load.height - 1);
        texels[i] = pixels[size_t(y) * load.width + x];
      }
      EncodeBc3Block(texels, linear.data() + (size_t(by) * layout.blocks_x + bx) * 16);
    }
  }
  CopyBlocks(load, layout, linear.data(), true);
}

// The loading screens' controls diagram (416x256 8888): an Xbox 360 pad. Its
// face buttons and shoulder labels are covered with PlayStation glyphs at the
// positions below (texel units); the outline stays.
constexpr uint64_t kControlsDiagram = 0xa5f1d55f3edcc6c2;
struct Overlay {
  Glyph glyph;
  float cx, cy, width, height;
};
constexpr Overlay kDiagramOverlays[] = {
    {Glyph::kTriangle, 336, 103, 36, 36}, {Glyph::kSquare, 299, 131, 36, 36},
    {Glyph::kCircle, 366, 130, 36, 36},   {Glyph::kCross, 330, 157, 36, 36},
    {Glyph::kL2, 93, 29, 30, 17},         {Glyph::kL1, 87, 45, 30, 15},
    {Glyph::kR2, 322, 29, 30, 17},        {Glyph::kR1, 328, 45, 30, 15},
    {Glyph::kHome, 209, 128, 44, 44},  // covers the guide button's Xbox mark
};

void OverlayDiagram(const TextureCache::GuestTextureLoad& load, const BlockLayout& layout,
                    std::vector<uint8_t>& linear) {
  for (const Overlay& overlay : kDiagramOverlays) {
    const uint32_t w = uint32_t(overlay.width), h = uint32_t(overlay.height);
    const std::vector<Rgba> glyph = DrawGlyph(overlay.glyph, w, h);
    const int x0 = int(overlay.cx - overlay.width / 2), y0 = int(overlay.cy - overlay.height / 2);
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const int tx = x0 + int(x), ty = y0 + int(y);
        if (tx < 0 || ty < 0 || tx >= int(load.width) || ty >= int(load.height)) continue;
        uint8_t* texel = linear.data() + (size_t(ty) * layout.blocks_x + tx) * 4;  // R G B A
        const Rgba under{texel[0] / 255.f, texel[1] / 255.f, texel[2] / 255.f, texel[3] / 255.f};
        const Rgba out = Over(glyph[size_t(y) * w + x], under);
        const float channels[4] = {out.r, out.g, out.b, out.a};
        for (int c = 0; c < 4; ++c) {
          texel[c] = uint8_t(std::lround(std::clamp(channels[c], 0.f, 1.f) * 255));
        }
      }
    }
  }
  CopyBlocks(load, layout, linear.data(), true);
}

// ---- Hook ------------------------------------------------------------------

std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped;

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
  const std::string& dump_dir = REXCVAR_GET(forza_texture_dump_dir);
  const bool prompt_size = load.format == TextureFormat::k_DXT4_5 && load.width <= 128 &&
                           load.height <= 128;
  const bool diagram_size = load.format == TextureFormat::k_8_8_8_8 && load.width == 416 &&
                            load.height == 256;
  const bool replace = REXCVAR_GET(forza_playstation_prompts) && (prompt_size || diagram_size);
  if (!replace && dump_dir.empty()) return false;
  thread_local BlockLayout layout;
  thread_local std::vector<uint8_t> linear;
  if (!GetLayout(load, layout)) {
    if (!dump_dir.empty()) {
      REXLOG_INFO("[forza] texture not dumped: format={} {}x{} dim={} packed={} at {:08X}",
                  unsigned(load.format), load.width, load.height, unsigned(load.dimension),
                  load.base_packed, load.base_address);
    }
    return false;
  }
  linear.resize(layout.offsets.size() * layout.format->bytes_per_block);
  CopyBlocks(load, layout, linear.data(), false);
  const uint64_t hash = XXH3_64bits(linear.data(), linear.size());
  if (!dump_dir.empty()) Dump(load, layout, linear, hash, dump_dir);
  if (!replace) return false;
  if (diagram_size) {
    if (hash != kControlsDiagram) return false;
    OverlayDiagram(load, layout, linear);
    return true;
  }
  for (const KnownPrompt& prompt : kKnownPrompts) {
    if (prompt.hash == hash) {
      Substitute(load, layout, prompt.glyph);
      return true;
    }
  }
  return false;
}

const bool g_registered = [] {
  TextureCache::SetGuestTextureLoadHook(OnGuestTextureLoad);
  return true;
}();
}  // namespace
