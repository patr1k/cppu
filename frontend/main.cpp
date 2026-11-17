// Minimal SDL2 host for the cppu library: owns the memory, ticks the PPU
// one frame's worth of dots at a time, and blits the framebuffer.

#include <SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>

#include "cppu/ppu.hpp"

namespace {

constexpr int kScale = 4;

// DMG-style green palette, indexed by shade 0-3 (ARGB8888).
constexpr std::array<std::uint32_t, 4> kPalette = {
    0xFF9BBC0F, 0xFF8BAC0F, 0xFF306230, 0xFF0F380F,
};

} // namespace

int main(int /*argc*/, char* /*argv*/[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("cppu",
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

    // 8 KiB of VRAM (0x8000-0x9FFF), owned by the host.
    std::array<std::uint8_t, 0x2000> vram{};
    cppu::PPU ppu(vram.data());

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

        for (int i = 0; i < cppu::kDotsPerFrame; ++i) {
            ppu.tick();
        }

        const std::uint8_t* fb = ppu.framebuffer();
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = kPalette[fb[i] & 0x3];
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
