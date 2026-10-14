// SPDX-License-Identifier: MIT
#include <SDL.h>

#include <algorithm>
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
#include "launcher/gui/theme.h"
#include "launcher/gui/ui_model.h"
#include "pipeline/pipeline.h"

namespace {

void usage() {
  std::fputs(
      "usage: legacyrecomp-gui [--root <dir>] [--iso <file>] [--install] [--screenshot <file.bmp>]\n"
      "  --root        data root (default: $LEGACYRECOMP_ROOT, then $XDG_DATA_HOME/legacyrecomp)\n"
      "  --iso         prefill the ISO field\n"
      "  --install     start the install immediately\n"
      "  --screenshot  save the window to a BMP once and exit (after the install, if started)\n"
      "environment: LEGACYRECOMP_UI_SCALE=<0.5..4> overrides the UI scale (default: from the display DPI)\n",
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

// Horizontal DPI of the first display, 0 when unknown.
float display_dpi() {
  float ddpi = 0, hdpi = 0, vdpi = 0;
  return SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 ? hdpi : 0.0f;
}

// `w`x`h` (window coordinates) scaled by `scale`, but never larger than the display's usable area.
SDL_Point scaled_size(int w, int h, float scale) {
  SDL_Point p{static_cast<int>(static_cast<float>(w) * scale), static_cast<int>(static_cast<float>(h) * scale)};
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
    p.x = std::min(p.x, usable.w);
    p.y = std::min(p.y, usable.h);
  }
  return p;
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
  // Created at the unscaled size first: the UI scale depends on the renderer's framebuffer scale.
  const SDL_Point initial = scaled_size(1100, 720, 1.0f);
  SDL_Window* window = SDL_CreateWindow("LegacyRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, initial.x,
                                        initial.y, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* renderer =
      window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED) : nullptr;
  if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!window || !renderer) {
    std::fprintf(stderr, "cannot create the window: %s\n", SDL_GetError());
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  // Window sizes are in window coordinates. Where those are logical (Wayland, sdl2-compat on SDL3) the
  // framebuffer scale already carries the display scale, so ui_scale only adds what it does not cover.
  float fb_scale = 1.0f;
  {
    int win_w = 0, win_h = 0, out_w = 0, out_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    if (win_w > 0 && out_w > 0) fb_scale = static_cast<float>(out_w) / static_cast<float>(win_w);
  }
  const float ui_scale = lr::gui::ui_scale(display_dpi(), fb_scale, std::getenv("LEGACYRECOMP_UI_SCALE"));
  if (ui_scale != 1.0f) {
    const SDL_Point size = scaled_size(1100, 720, ui_scale);
    SDL_SetWindowSize(window, size.x, size.y);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
  const SDL_Point minimum = scaled_size(900, 600, ui_scale);
  SDL_SetWindowMinimumSize(window, minimum.x, minimum.y);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);
  {
    auto resources = lr::pipeline::find_resources();
    if (!resources) std::fprintf(stderr, "warning: resources not found (%s); using the default font\n",
                                 resources.error().message.c_str());
    lr::gui::theme::setup(resources ? resources.value() / "fonts" : std::filesystem::path(), ui_scale, fb_scale);
  }

  int rc = 0;
  {
    lr::gui::App app({lr::resolve_data_root(root_flag, std::getenv("LEGACYRECOMP_ROOT"),
                                            std::getenv("XDG_DATA_HOME"), std::getenv("HOME")),
                      iso ? *iso : std::filesystem::path()});
    if (autostart) app.start_install();

    bool running = true;
    int settled_frames = 0;
    int pending_frames = 3;  // frames still to draw after the last input (ImGui reacts one frame late)
    auto handle = [&](SDL_Event& ev) {
      ImGui_ImplSDL2_ProcessEvent(&ev);
      if (ev.type == SDL_QUIT) running = false;
      if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
      if (ev.type == SDL_DROPFILE) {
        if (!app.installing()) app.set_iso(ev.drop.file);  // the running install keeps its ISO
        SDL_free(ev.drop.file);
      }
      pending_frames = 3;
    };
    while (running) {
      SDL_Event ev;
      // Idle: sleep until input (or 100 ms, to pick up worker/dialog results); animating: one frame per vsync.
      if (!screenshot && pending_frames == 0 && !app.animating() && SDL_WaitEventTimeout(&ev, 100)) handle(ev);
      while (SDL_PollEvent(&ev)) handle(ev);
      if (pending_frames > 0) --pending_frames;
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      app.frame();
      ImGui::Render();
      SDL_SetRenderDrawColor(renderer, (lr::gui::theme::kBackground >> 16) & 0xFF,
                             (lr::gui::theme::kBackground >> 8) & 0xFF, lr::gui::theme::kBackground & 0xFF, 255);
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
