// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/gui/ui_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace lr::gui {

StageText stage_text(pipeline::StageId id) {
  switch (id) {
    case pipeline::StageId::Doctor: return {"Verificar o sistema", "Ferramentas, CPU e espaço em disco"};
    case pipeline::StageId::Extract: return {"Extrair a ISO", "Copia os arquivos do jogo da imagem"};
    case pipeline::StageId::Verify: return {"Conferir os arquivos", "Compara os hashes com a versão esperada"};
    case pipeline::StageId::Patch: return {"Aplicar a atualização", "Aplica a atualização do título ao executável"};
    case pipeline::StageId::Analyse: return {"Analisar o executável", "Localiza funções e tabelas de salto"};
    case pipeline::StageId::Recompile: return {"Gerar o código C++", "Recompila o código PowerPC para C++"};
    case pipeline::StageId::Build: return {"Compilar o jogo", "Compila o executável nativo para Linux"};
  }
  return {"?", ""};
}

StatusPill derive_status(bool installing, bool has_error, bool build_complete) {
  if (installing) return StatusPill::Installing;
  if (has_error) return StatusPill::Error;
  if (build_complete) return StatusPill::Ready;
  return StatusPill::NotInstalled;
}

std::string_view status_label(StatusPill pill) {
  switch (pill) {
    case StatusPill::NotInstalled: return "Não instalado";
    case StatusPill::Installing: return "Instalando…";
    case StatusPill::Ready: return "Pronto para jogar";
    case StatusPill::Error: return "Erro";
  }
  return "";
}

std::string abbreviate_home(const std::filesystem::path& path, const char* home) {
  std::string p = path.string();
  if (!home) return p;
  std::string h = home;
  while (h.size() > 1 && h.back() == '/') h.pop_back();
  if (h.empty() || h == "/") return p;
  if (p == h) return "~";
  if (p.size() > h.size() && p.compare(0, h.size(), h) == 0 && p[h.size()] == '/') return "~" + p.substr(h.size());
  return p;
}

float approach_progress(float displayed, float target, float dt_seconds, float rate) {
  if (target <= displayed) return target;
  const float next = displayed + (target - displayed) * (1.0f - std::exp(-rate * std::max(dt_seconds, 0.0f)));
  return target - next < 0.005f ? target : next;
}

float ui_scale(float dpi, float framebuffer_scale, const char* env_override) {
  if (env_override && *env_override) {
    char* end = nullptr;
    const float v = std::strtof(env_override, &end);
    if (end && *end == '\0' && v >= 0.5f && v <= 4.0f) return v;
  }
  const float display = dpi > 0.0f ? dpi / 96.0f : 1.0f;
  const float s = display / std::max(framebuffer_scale, 1.0f);
  return std::clamp(std::round(s * 4.0f) / 4.0f, 1.0f, 3.0f);
}

LogTone classify_log_line(std::string_view line) {
  std::string lower(line);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  auto has = [&](std::string_view needle) { return lower.find(needle) != std::string::npos; };
  if (has("[error]") || has("error:") || lower.starts_with("failed:") || has("cmake error")) return LogTone::Error;
  if (has("[warn]") || has("warning:") || has("cmake warning")) return LogTone::Warning;
  return LogTone::Normal;
}

}  // namespace lr::gui
