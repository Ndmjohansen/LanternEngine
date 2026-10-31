// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include "engine/hud_clean.h"
#include "hud_gauge.h"
#include <xxhash.h>
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {
constexpr std::uint8_t kPngMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
constexpr std::uint32_t kLargestSide = 4096;
constexpr std::uint32_t kLargestSource = 16384;

template <typename T> T Le(const Bytes& b, std::size_t at) {
    T v{};
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

std::uint32_t Be32(const Bytes& b, std::size_t at) {
    return std::uint32_t(b[at]) << 24 | std::uint32_t(b[at + 1]) << 16 | std::uint32_t(b[at + 2]) << 8 | b[at + 3];
}

Bytes ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool WriteFile(const fs::path& path, const Bytes& data) {
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) return false;
    }
    std::error_code error;
    fs::rename(temp, path, error);
    return !error;
}

void PutBe32(Bytes& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(std::uint8_t(v >> shift));
}

void Chunk(Bytes& png, const char* type, const Bytes& data) {
    PutBe32(png, std::uint32_t(data.size()));
    const std::size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    PutBe32(png, std::uint32_t(crc32(0, png.data() + start, uInt(png.size() - start))));
}

bool WritePng(const fs::path& path, int w, int h, const std::vector<std::uint32_t>& rgba) {
    Bytes raw;
    raw.reserve(std::size_t(w) * h * 4 + h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const std::uint32_t c = rgba[std::size_t(y) * w + x];
            raw.insert(raw.end(), {std::uint8_t(c), std::uint8_t(c >> 8), std::uint8_t(c >> 16), std::uint8_t(c >> 24)});
        }
    }
    uLongf size = compressBound(uLong(raw.size()));
    Bytes packed(size);
    if (compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 1) != Z_OK) return false;
    packed.resize(size);
    Bytes png(std::begin(kPngMagic), std::end(kPngMagic));
    Bytes header;
    PutBe32(header, std::uint32_t(w));
    PutBe32(header, std::uint32_t(h));
    header.insert(header.end(), {8, 6, 0, 0, 0});
    Chunk(png, "IHDR", header);
    Chunk(png, "IDAT", packed);
    Chunk(png, "IEND", {});
    return WriteFile(path, png);
}

int Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

bool ReadPng(const fs::path& path, Bytes& rgba, std::uint32_t& w, std::uint32_t& h) {
    const Bytes file = ReadFile(path);
    if (file.size() < 33 || std::memcmp(file.data(), kPngMagic, 8) != 0) return false;
    w = Be32(file, 16);
    h = Be32(file, 20);
    const int depth = file[24], type = file[25], interlace = file[28];
    if (depth != 8 || (type != 2 && type != 6) || interlace != 0 || !w || !h || w > kLargestSource || h > kLargestSource)
        return false;
    const int channels = type == 6 ? 4 : 3;
    Bytes packed;
    for (std::size_t at = 8; at + 12 <= file.size();) {
        const std::uint32_t n = Be32(file, at);
        if (at + 12 + n > file.size()) return false;
        if (std::memcmp(file.data() + at + 4, "IDAT", 4) == 0)
            packed.insert(packed.end(), file.begin() + std::ptrdiff_t(at + 8), file.begin() + std::ptrdiff_t(at + 8 + n));
        at += 12 + n;
    }
    const std::size_t stride = std::size_t(w) * channels;
    Bytes raw((stride + 1) * h);
    uLongf size = uLongf(raw.size());
    if (uncompress(raw.data(), &size, packed.data(), uLong(packed.size())) != Z_OK || size != raw.size()) return false;
    Bytes prior(stride, 0), line(stride);
    rgba.assign(std::size_t(w) * h * 4, 255);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t filter = raw[y * (stride + 1)];
        const std::uint8_t* in = raw.data() + y * (stride + 1) + 1;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= std::size_t(channels) ? line[i - channels] : 0, b = prior[i];
            const int c = i >= std::size_t(channels) ? prior[i - channels] : 0;
            int v = in[i];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += Paeth(a, b, c); break;
            default: return false;
            }
            line[i] = std::uint8_t(v);
        }
        for (std::uint32_t x = 0; x < w; ++x)
            for (int k = 0; k < channels; ++k) rgba[(std::size_t(y) * w + x) * 4 + k] = line[std::size_t(x) * channels + k];
        prior.swap(line);
    }
    return true;
}

void Halve(Bytes& pixels, std::uint32_t& w, std::uint32_t& h) {
    const std::uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    Bytes out(std::size_t(nw) * nh * 4);
    for (std::uint32_t y = 0; y < nh; ++y)
        for (std::uint32_t x = 0; x < nw; ++x) {
            int color[3] = {}, alpha = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy)
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint8_t* p = &pixels[(std::size_t(std::min(h - 1, y * 2 + dy)) * w + std::min(w - 1, x * 2 + dx)) * 4];
                    for (int c = 0; c < 3; ++c) color[c] += p[c] * p[3];
                    alpha += p[3];
                }
            std::uint8_t* o = &out[(std::size_t(y) * nw + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = alpha ? std::uint8_t((color[c] + alpha / 2) / alpha) : 0;
            o[3] = std::uint8_t((alpha + 2) / 4);
        }
    pixels.swap(out);
    w = nw;
    h = nh;
}

const std::array<float, 256>& Linear() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[std::size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

std::uint8_t Srgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return std::uint8_t(std::lround(s * 255.0f));
}

// The colour under fully transparent pixels taken from their visible neighbours, grown a pixel a
// pass: whatever the art left there (often black) otherwise upscales into dark halos at soft alpha
// edges.
void Bleed(Bytes& rgba, int width, int height) {
    std::vector<std::uint8_t> known(std::size_t(width) * height);
    for (std::size_t i = 0; i < known.size(); ++i) known[i] = rgba[i * 4 + 3] > 0;
    for (int pass = 0; pass < 12; ++pass) {
        std::vector<std::uint8_t> grown = known;
        bool any = false;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                if (known[i]) continue;
                int sum[3] = {}, count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                        const std::size_t j = std::size_t(sy) * width + sx;
                        if (!known[j]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += rgba[j * 4 + c];
                        ++count;
                    }
                if (!count) continue;
                for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = std::uint8_t((sum[c] + count / 2) / count);
                grown[i] = 1;
                any = true;
            }
        known = std::move(grown);
        if (!any) break;
    }
}

// One separable pass of `kernel` (taps around each output pixel's centre in the source) over float
// RGBA, along x when `horizontal`, on up to 8 threads.
std::vector<float> Pass(const std::vector<float>& src, int width, int height, int out_size, bool horizontal,
                        const std::function<float(float)>& kernel, float support) {
    const int out_w = horizontal ? out_size : width, out_h = horizontal ? height : out_size;
    const int in_size = horizontal ? width : height;
    std::vector<std::vector<std::pair<int, float>>> taps(static_cast<std::size_t>(out_size));
    for (int o = 0; o < out_size; ++o) {
        const float centre = (float(o) + 0.5f) * float(in_size) / float(out_size) - 0.5f;
        const int first = int(std::floor(centre - support)), last = int(std::ceil(centre + support));
        float total = 0.0f;
        for (int s = first; s <= last; ++s) {
            const float w = kernel(float(s) - centre);
            if (w == 0.0f) continue;
            taps[std::size_t(o)].emplace_back(std::clamp(s, 0, in_size - 1), w);
            total += w;
        }
        for (auto& tap : taps[std::size_t(o)]) tap.second /= total;
    }
    std::vector<float> out(std::size_t(out_w) * out_h * 4);
    const auto one = [&](int o, int l) {
        float acc[4] = {};
        for (const auto& [s, w] : taps[std::size_t(o)]) {
            const std::size_t at = horizontal ? (std::size_t(l) * width + s) * 4 : (std::size_t(s) * width + l) * 4;
            for (int c = 0; c < 4; ++c) acc[c] += src[at + c] * w;
        }
        const std::size_t to = horizontal ? (std::size_t(l) * out_w + o) * 4 : (std::size_t(o) * out_w + l) * 4;
        for (int c = 0; c < 4; ++c) out[to + c] = acc[c];
    };
    const int rows = horizontal ? height : out_size, across = horizontal ? out_size : width;
    const auto band = [&](int first, int step) {
        for (int r = first; r < rows; r += step)
            for (int c = 0; c < across; ++c) horizontal ? one(c, r) : one(r, c);
    };
    const unsigned n = std::max(1u, std::min({8u, std::thread::hardware_concurrency(), unsigned(rows)}));
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < n; ++i) pool.emplace_back(band, int(i), int(n));
    band(0, int(n));
    for (auto& t : pool) t.join();
    return out;
}

// The model's 4x made the pack's 2x, the recipe picked by eye against the alternatives (x4plus,
// Siax, Superscale and Nomos8kSC on the game's own art): 2x2 boxes in linear light (alpha
// weighted), the finest detail turned down 35% (the model over-sharpens the smallest scale), then
// 20% of a plain Lanczos resize of the original mixed in, which keeps the original's look and
// colour. A conservative look, a matter of taste.
Bytes Finish(const Bytes& big, std::uint32_t& w, std::uint32_t& h, const Bytes& original, int original_w, int original_h) {
    constexpr float kFineDetailCut = 0.35f, kPlainShare = 0.20f;
    const auto& linear = Linear();
    const int width = original_w * 2, height = original_h * 2, big_w = int(w);
    Bytes half(std::size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float sum[4] = {}, plain[3] = {};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const std::uint8_t* p = &big[(std::size_t(y * 2 + dy) * big_w + x * 2 + dx) * 4];
                    const float a = float(p[3]) / 255.0f;
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += linear[p[c]] * a;
                        plain[c] += linear[p[c]];
                    }
                    sum[3] += a;
                }
            std::uint8_t* q = &half[(std::size_t(y) * width + x) * 4];
            // Fully transparent: the plain average, so filtering at the art's edge blends towards
            // a colour that belongs to it.
            for (int c = 0; c < 3; ++c) q[c] = Srgb(sum[3] > 0.0f ? sum[c] / sum[3] : plain[c] / 4.0f);
            q[3] = std::uint8_t(std::lround(sum[3] / 4.0f * 255.0f));
        }
    const std::vector<float> base(half.begin(), half.end());
    // The finest band: what a Gaussian of 0.9 output pixels removes.
    const float sigma = 0.9f;
    const auto gauss = [sigma](float x) { return std::exp(-x * x / (2.0f * sigma * sigma)); };
    const std::vector<float> soft =
        Pass(Pass(base, width, height, width, true, gauss, 3.0f * sigma), width, height, height, false, gauss, 3.0f * sigma);
    const auto lanczos = [](float x) {
        x = std::fabs(x);
        if (x < 1e-6f) return 1.0f;
        if (x >= 3.0f) return 0.0f;
        const float pi = 3.14159265f;
        return 3.0f * std::sin(pi * x) * std::sin(pi * x / 3.0f) / (pi * pi * x * x);
    };
    const std::vector<float> plain =
        Pass(Pass(std::vector<float>(original.begin(), original.end()), original_w, original_h, width, true, lanczos, 3.0f),
             width, original_h, height, false, lanczos, 3.0f);
    for (std::size_t i = 0; i < half.size(); ++i) {
        const float detail = base[i] - kFineDetailCut * (base[i] - soft[i]);
        half[i] = std::uint8_t(std::clamp(std::lround((1.0f - kPlainShare) * detail + kPlainShare * plain[i]), 0L, 255L));
    }
    w = std::uint32_t(width);
    h = std::uint32_t(height);
    return half;
}

std::uint32_t Rgb565(std::uint16_t c) {
    const std::uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (r * 255 / 31) | (g * 255 / 63) << 8 | (b * 255 / 31) << 16 | 0xff000000u;
}

void Bc1Block(const std::uint8_t* b, std::uint32_t out[16]) {
    const std::uint16_t c0 = std::uint16_t(b[0] | b[1] << 8), c1 = std::uint16_t(b[2] | b[3] << 8);
    std::uint32_t p[4] = {Rgb565(c0), Rgb565(c1), 0, 0};
    const auto mix = [](std::uint32_t a, std::uint32_t c, int wa, int wc, int d) {
        std::uint32_t r = 0;
        for (int s = 0; s < 24; s += 8) r |= ((((a >> s) & 0xff) * wa + ((c >> s) & 0xff) * wc) / d) << s;
        return r | 0xff000000u;
    };
    if (c0 > c1) {
        p[2] = mix(p[0], p[1], 2, 1, 3);
        p[3] = mix(p[0], p[1], 1, 2, 3);
    } else {
        p[2] = mix(p[0], p[1], 1, 1, 2);
    }
    const std::uint32_t bits = std::uint32_t(b[4] | b[5] << 8 | b[6] << 16 | std::uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (2 * i)) & 3];
}

bool DecodeTexture(const Bytes& tpf, std::uint32_t offset, int format, int width, int height, std::vector<std::uint32_t>& pixels) {
    pixels.assign(std::size_t(width) * height, 0u);
    if (format == 28 || format == 29 || format == 87 || format == 91) {
        const int tiles_wide = (width + 7) / 8;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const std::size_t index = (std::size_t(y / 8) * tiles_wide + x / 8) * 64 + BbHudGauge::TileIndex(x % 8, y % 8);
                if (offset + index * 4 + 4 > tpf.size()) continue;
                std::uint32_t c = Le<std::uint32_t>(tpf, offset + index * 4);
                if (format == 87 || format == 91) c = (c & 0xff00ff00u) | (c & 0xff) << 16 | (c >> 16 & 0xff);
                pixels[std::size_t(y) * width + x] = c;
            }
        return true;
    }
    const bool bc1 = format == 71 || format == 72, bc7 = format == 98 || format == 99;
    if (!bc1 && !bc7) return false;
    const int bytes = bc1 ? 8 : 16;
    const int blocks_wide = (width + 3) / 4, blocks_high = (height + 3) / 4, tiles_wide = (blocks_wide + 7) / 8;
    std::uint32_t texels[16];
    for (int by = 0; by < blocks_high; ++by)
        for (int bx = 0; bx < blocks_wide; ++bx) {
            const std::size_t index = (std::size_t(by / 8) * tiles_wide + bx / 8) * 64 + BbHudGauge::TileIndex(bx % 8, by % 8);
            if (offset + (index + 1) * bytes > tpf.size()) continue;
            if (bc1)
                Bc1Block(tpf.data() + offset + index * bytes, texels);
            else
                BbHudGauge::DecodeBc7Block(tpf.data() + offset + index * bytes, texels);
            for (int t = 0; t < 16; ++t) {
                const int x = bx * 4 + t % 4, y = by * 4 + t / 4;
                if (x < width && y < height) pixels[std::size_t(y) * width + x] = texels[t];
            }
        }
    return true;
}

template <typename Work>
void RunWorkers(std::size_t count, const Work& work) {
    std::atomic<std::size_t> next{0};
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < count; i = next++) work(i);
        });
    for (std::thread& thread : pool) thread.join();
}

// `part`: "art" (every sheet but the item pictures), "pictures" (menu_knowledge_*), or empty for all.
int Export(const fs::path& menu, const fs::path& out, const std::string& part) {
    std::error_code error;
    fs::create_directories(out, error);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(menu, error)) {
        if (!entry.is_regular_file()) continue;
        const std::string file = entry.path().filename().string();
        if (!file.ends_with(".tpf.dcx") && !file.ends_with(".tpf")) continue;
        const bool picture = file.starts_with("menu_knowledge");
        if ((part == "art" && picture) || (part == "pictures" && !picture)) continue;
        files.push_back(entry.path());
    }
    std::mutex seen_mutex;
    std::set<std::string> seen;
    std::atomic<int> written{0}, total{0};
    std::atomic<bool> failed{false};
    RunWorkers(files.size(), [&](std::size_t n) {
        std::error_code file_error;
        const bool packed = files[n].filename().string().ends_with(".tpf.dcx");
        const Bytes data = ReadFile(files[n]);
        Bytes tpf;
        if (packed ? !BbHudGauge::InflateDcx(data, tpf) : (tpf = data, false)) return;
        if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0) return;
        const std::uint32_t count = Le<std::uint32_t>(tpf, 8);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t at = 0x10 + std::size_t(i) * 36;
            if (at + 36 > tpf.size()) break;
            const std::uint32_t offset = Le<std::uint32_t>(tpf, at), size = Le<std::uint32_t>(tpf, at + 4);
            const int width = Le<std::uint16_t>(tpf, at + 12), height = Le<std::uint16_t>(tpf, at + 14);
            const int format = int(Le<std::uint32_t>(tpf, at + 32));
            if (width < 16 || height < 16 || offset + std::uint64_t(size) > tpf.size()) continue;
            char key[96];
            std::snprintf(key, sizeof key, "%dx%d_%u_%016llx", width, height, size,
                          static_cast<unsigned long long>(XXH3_64bits(tpf.data() + offset, size)));
            {
                std::scoped_lock lock{seen_mutex};
                if (!seen.insert(key).second) continue;
            }
            const fs::path png = out / (std::string(key) + ".png");
            std::vector<std::uint32_t> pixels;
            if (fs::exists(png, file_error) || !DecodeTexture(tpf, offset, format, width, height, pixels)) {
                if (fs::exists(png, file_error)) ++total;
                continue;
            }
            Bytes bytes(pixels.size() * 4);
            std::memcpy(bytes.data(), pixels.data(), bytes.size());
            Bleed(bytes, width, height);
            std::memcpy(pixels.data(), bytes.data(), bytes.size());
            if (!WritePng(png, width, height, pixels)) {
                std::fprintf(stderr, "could not write %s\n", png.string().c_str());
                failed = true;
                return;
            }
            ++written;
            ++total;
        }
    });
    if (failed) return 1;
    std::printf("exported %d textures (%d new)\n", total.load(), written.load());
    return total > 0 ? 0 : 1;
}

bool WriteBbui(const fs::path& target, const Bytes& pixels, std::uint32_t w, std::uint32_t h) {
    uLongf size = compressBound(uLong(pixels.size()));
    Bytes body(12 + size);
    std::memcpy(body.data(), "BBUI", 4);
    std::memcpy(body.data() + 4, &w, 4);
    std::memcpy(body.data() + 8, &h, 4);
    if (compress2(body.data() + 12, &size, pixels.data(), uLong(pixels.size()), 6) != Z_OK) return false;
    body.resize(12 + size);
    return WriteFile(target, body);
}

bool ReadBbui(const fs::path& path, Bytes& pixels, std::uint32_t& w, std::uint32_t& h) {
    std::ifstream in(path, std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (file.size() < 12 || std::memcmp(file.data(), "BBUI", 4) != 0) return false;
    std::memcpy(&w, file.data() + 4, 4);
    std::memcpy(&h, file.data() + 8, 4);
    if (!w || !h || w > 16384 || h > 16384) return false;
    pixels.resize(std::size_t(w) * h * 4);
    uLongf size = uLongf(pixels.size());
    return uncompress(pixels.data(), &size, file.data() + 12, uLong(file.size() - 12)) == Z_OK && size == pixels.size();
}

// The HUD's sheet is drawn from a cleaned copy of its art (HudClean): made here, once, so the game
// only adds the key prompts to it. Without the art, no copy (an earlier build's goes).
bool PackHud(const fs::path& out) {
    Bytes pixels;
    std::uint32_t w = 0, h = 0;
    const std::uint32_t sw = KeyPrompts::kHudWidth, sh = KeyPrompts::kHudHeight;
    if (!ReadBbui(out / HudClean::ArtName(), pixels, w, h) || w % sw != 0 || h != sh * (w / sw)) {
        std::error_code error;
        fs::remove(out / HudClean::kArtFile, error);
        return true;
    }
    HudClean::Clean(pixels, int(w), int(h), int(w / sw), true);
    if (!WriteBbui(out / HudClean::kArtFile, pixels, w, h)) {
        std::fprintf(stderr, "could not write %s\n", (out / HudClean::kArtFile).string().c_str());
        return false;
    }
    std::printf("cleaned the HUD's art\n");
    return true;
}

// `originals`: the exported PNGs the upscaled ones were made from; given, the model's 4x is
// finished at 2x (Finish).
int Pack(const fs::path& in, const fs::path& out, const fs::path& originals) {
    std::error_code error;
    fs::create_directories(out, error);
    std::vector<fs::path> pngs;
    for (const auto& entry : fs::directory_iterator(in, error))
        if (entry.path().extension() == ".png") pngs.push_back(entry.path());
    std::sort(pngs.begin(), pngs.end());
    std::atomic<int> done{0}, failed{0};
    std::atomic<bool> stopped{false};
    std::mutex print_mutex;
    RunWorkers(pngs.size(), [&](std::size_t n) {
        const fs::path& png = pngs[n];
        std::error_code file_error;
        const fs::path target = out / (png.stem().string() + ".bbui");
        if (!fs::exists(target, file_error)) {
            Bytes pixels;
            std::uint32_t w = 0, h = 0;
            if (!ReadPng(png, pixels, w, h)) {
                std::fprintf(stderr, "could not read %s\n", png.string().c_str());
                ++failed;
                return;
            }
            Bytes original;
            std::uint32_t ow = 0, oh = 0;
            if (!originals.empty()) {
                if (!ReadPng(originals / png.filename(), original, ow, oh) || w != ow * 4 || h != oh * 4) {
                    std::fprintf(stderr, "no 4x original for %s\n", png.string().c_str());
                    ++failed;
                    return;
                }
                pixels = Finish(pixels, w, h, original, int(ow), int(oh));
            }
            while (w > kLargestSide || h > kLargestSide) Halve(pixels, w, h);
            if (!WriteBbui(target, pixels, w, h)) {
                std::fprintf(stderr, "could not write %s\n", target.string().c_str());
                stopped = true;
                return;
            }
        }
        std::scoped_lock lock{print_mutex};
        std::printf("packed %d of %zu\n", ++done, pngs.size());
        std::fflush(stdout);
    });
    if (stopped || !PackHud(out)) return 1;
    std::printf("packed %d textures, %d failed\n", done.load(), failed.load());
    return failed == 0 && done > 0 ? 0 : 1;
}
}

int main(int argc, char** argv) {
    std::vector<fs::path> args;
#ifdef _WIN32
    int wide_count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &wide_count)) {
        for (int i = 0; i < wide_count; ++i) args.emplace_back(wide[i]);
        LocalFree(wide);
    }
#else
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    if ((argc == 4 || argc == 5) && args.size() == std::size_t(argc) && std::strcmp(argv[1], "export") == 0)
        return Export(args[2], args[3], argc == 5 ? argv[4] : "");
    if ((argc == 4 || argc == 5) && args.size() == std::size_t(argc) && std::strcmp(argv[1], "pack") == 0)
        return Pack(args[2], args[3], argc == 5 ? args[4] : fs::path());
    std::fprintf(stderr, "usage: bb-ui-4k export <game menu folder> <png folder> [art|pictures]\n"
                         "       bb-ui-4k pack <upscaled png folder> <ui-4k folder> [<exported png folder>]\n");
    return 2;
}
