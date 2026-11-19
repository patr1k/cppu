// Runs a Game Boy ROM (meant for dmg-acid2) on Peanut-GB with cppu as the
// PPU, and shows cppu's framebuffer.
//
//   cppu_acid2 <rom.gb>                          open a window (Esc quits)
//   cppu_acid2 <rom.gb> --frames N --out f.bmp   run N frames headless and
//                                                save the last one as a BMP
//
// The window and the BMP use the same grayscale shades as dmg-acid2's
// reference image, so the two can be compared directly.

#include <SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cppu/ppu.hpp"
#include "cppu_bridge.h"
#include "peanut_host.h"

namespace {

constexpr int kScale = 4;

// Shade 0 (lightest) to 3 (darkest), as RGB. Matches the shades in
// dmg-acid2's img/reference-dmg.png.
constexpr std::array<std::uint32_t, 4> kShades = {
    0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000,
};

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

// Writes cppu's framebuffer as an uncompressed 24-bit BMP.
bool write_bmp(const char* path, const std::uint8_t* fb) {
    constexpr int w = cppu::kScreenWidth;
    constexpr int h = cppu::kScreenHeight;
    constexpr int row_bytes = w * 3; // 480, already a multiple of 4
    constexpr int data_size = row_bytes * h;

    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    auto put16 = [&](int v) { std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); };
    auto put32 = [&](int v) { put16(v & 0xFFFF); put16((v >> 16) & 0xFFFF); };

    std::fputs("BM", f);
    put32(14 + 40 + data_size); put32(0); put32(14 + 40);       // file header
    put32(40); put32(w); put32(h); put16(1); put16(24);          // info header
    put32(0); put32(data_size); put32(2835); put32(2835); put32(0); put32(0);

    for (int y = h - 1; y >= 0; --y) { // BMP rows go bottom-up
        for (int x = 0; x < w; ++x) {
            const std::uint32_t rgb = kShades[fb[y * w + x] & 3];
            std::fputc(static_cast<int>(rgb & 0xFF), f);         // B
            std::fputc(static_cast<int>((rgb >> 8) & 0xFF), f);  // G
            std::fputc(static_cast<int>((rgb >> 16) & 0xFF), f); // R
        }
    }
    return std::fclose(f) == 0;
}

int run_headless(int frames, const char* out_path) {
    for (int i = 0; i < frames; ++i) {
        host_run_frame();
    }
    if (!write_bmp(out_path, cppu_framebuffer())) {
        std::fprintf(stderr, "couldn't write %s\n", out_path);
        return 1;
    }
    std::printf("wrote frame %d to %s\n", frames, out_path);
    return 0;
}

int run_window() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("cppu - dmg-acid2",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cppu::kScreenWidth * kScale, cppu::kScreenHeight * kScale, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, cppu::kScreenWidth, cppu::kScreenHeight);
    if (!window || !renderer || !texture) {
        std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return 1;
    }

    std::array<std::uint32_t, cppu::kScreenWidth * cppu::kScreenHeight> pixels{};
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT ||
                (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
            }
        }

        host_run_frame();

        const std::uint8_t* fb = cppu_framebuffer();
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = 0xFF000000u | kShades[fb[i] & 3];
        }
        SDL_UpdateTexture(texture, nullptr, pixels.data(),
            cppu::kScreenWidth * static_cast<int>(sizeof(std::uint32_t)));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "usage: cppu_acid2 <rom.gb>\n"
        "       cppu_acid2 <rom.gb> --frames N --out frame.bmp\n");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage();
        return 2;
    }

    int frames = 0;
    const char* out_path = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if ((frames > 0) != (out_path != nullptr)) {
        std::fprintf(stderr, "--frames and --out go together\n");
        return 2;
    }

    std::vector<std::uint8_t> rom;
    if (!read_file(argv[1], rom)) {
        std::fprintf(stderr, "couldn't read %s\n", argv[1]);
        return 1;
    }
    char error[128];
    if (!host_init(rom.data(), rom.size(), error, sizeof(error))) {
        std::fprintf(stderr, "%s\n", error);
        return 1;
    }

    return out_path ? run_headless(frames, out_path) : run_window();
}
