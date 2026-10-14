// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/gui/ui_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>

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

namespace {

bool all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// `raw` == prefix + <digits> + suffix: returns the digits.
std::optional<std::string_view> between(std::string_view raw, std::string_view prefix, std::string_view suffix) {
  if (!raw.starts_with(prefix) || !raw.ends_with(suffix) || raw.size() < prefix.size() + suffix.size())
    return std::nullopt;
  std::string_view mid = raw.substr(prefix.size(), raw.size() - prefix.size() - suffix.size());
  if (!all_digits(mid)) return std::nullopt;
  return mid;
}

}  // namespace

std::string stage_message(pipeline::StageId id, std::string_view raw) {
  using pipeline::StageId;
  if (raw == "start") return "Iniciando…";
  switch (id) {
    case StageId::Extract:
      if (raw.starts_with("package: ")) return "Extraindo o pacote: " + std::string(raw.substr(9));
      if (raw.starts_with("title update: ")) return "Extraindo a atualização: " + std::string(raw.substr(14));
      break;
    case StageId::Patch:
      if (raw == "applying title update") return "Aplicando a atualização do título";
      if (raw == "writing patched image") return "Gravando o executável atualizado";
      break;
    case StageId::Analyse:
      if (raw == "XenonAnalyse: scanning for jump tables") return "XenonAnalyse: procurando tabelas de salto";
      if (raw == "scanning for jump tables (LegacyRecomp)") return "Procurando tabelas de salto (LegacyRecomp)";
      break;
    case StageId::Recompile: {
      constexpr std::string_view kPrefix = "Recompiling functions... ";
      const auto pos = raw.find(kPrefix);
      if (pos != std::string_view::npos) {
        const std::string_view pct = raw.substr(pos + kPrefix.size());
        if (!pct.empty() && pct.back() == '%' && pct.find(' ') == std::string_view::npos)
          return "Recompilando funções… " + std::string(pct);
      }
      break;
    }
    case StageId::Build:
      if (auto n = between(raw, "wrote ", " import stubs")) return std::string(*n) + " stubs de importação gerados";
      if (raw == "configuring the game build") return "Configurando a compilação do jogo";
      if (auto n = between(raw, "compiling with ", " jobs")) return "Compilando com " + std::string(*n) + " processos";
      if (raw.size() > 2 && raw.front() == '[' && raw.back() == ']') {
        const std::string_view inner = raw.substr(1, raw.size() - 2);
        const auto slash = inner.find('/');
        if (slash != std::string_view::npos && all_digits(inner.substr(0, slash)) &&
            all_digits(inner.substr(slash + 1)))
          return "Compilando " + std::string(raw);
      }
      break;
    case StageId::Doctor:
    case StageId::Verify: break;
  }
  return "Em andamento…";
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
