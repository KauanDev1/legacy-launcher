// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "common/paths.h"
#include "imgui.h"
#include "launcher/gui/app.h"

namespace {

void usage() {
  std::fputs(
      "usage: legacyrecomp-gui [--root <dir>] [--iso <file>] [--install] [--screenshot <file.bmp>]\n"
      "  --root        data root (default: $LEGACYRECOMP_ROOT, then $XDG_DATA_HOME/legacyrecomp)\n"
      "  --iso         prefill the ISO field\n"
      "  --install     start the install immediately\n"
      "  --screenshot  save the window to a BMP once and exit (after the install, if started)\n",
      stderr);
}

bool save_screenshot(SDL_Renderer* r, const char* path) {
  int w = 0, h = 0;
  if (SDL_GetRendererOutputSize(r, &w, &h) != 0) return false;
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return false;
  bool ok = SDL_RenderReadPixels(r, nullptr, s->format->format, s->pixels, s->pitch) == 0 && SDL_SaveBMP(s, path) == 0;
  SDL_FreeSurface(s);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::filesystem::path> root_flag, iso;
  const char* screenshot = nullptr;
  bool autostart = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--root" && i + 1 < argc) root_flag = argv[++i];
    else if (a == "--iso" && i + 1 < argc) iso = argv[++i];
    else if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
    else if (a == "--install") autostart = true;
    else {
      usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("LegacyRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1000, 700,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* renderer =
      window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED) : nullptr;
  if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!window || !renderer) {
    std::fprintf(stderr, "cannot create the window: %s\n", SDL_GetError());
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(window, 900, 600);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  int rc = 0;
  {
    lr::gui::App app({lr::resolve_data_root(root_flag, std::getenv("LEGACYRECOMP_ROOT"),
                                            std::getenv("XDG_DATA_HOME"), std::getenv("HOME")),
                      iso ? *iso : std::filesystem::path()});
    if (autostart) app.start_install();

    bool running = true;
    int settled_frames = 0;
    while (running) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) running = false;
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
        if (ev.type == SDL_DROPFILE) {
          app.set_iso(ev.drop.file);
          SDL_free(ev.drop.file);
        }
      }
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      app.frame();
      ImGui::Render();
      SDL_SetRenderDrawColor(renderer, 24, 24, 28, 255);
      SDL_RenderClear(renderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
      if (screenshot && !app.installing() && ++settled_frames > 10) {
        if (!save_screenshot(renderer, screenshot)) {
          std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
          rc = 1;
        }
        running = false;
      }
      SDL_RenderPresent(renderer);
    }
    // ~App cancels a running install and joins every worker thread before the window goes away.
  }

  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return rc;
}
