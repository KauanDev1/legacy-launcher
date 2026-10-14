# LegacyRecomp — Marco 1: Pipeline + Esqueleto do Runtime

- **Data:** 2026-10-14
- **Status:** Aguardando revisão
- **Escopo:** Marco 1 (M1) do projeto LegacyRecomp

## 1. Objetivo

LegacyRecomp porta o *Minecraft: Xbox 360 Edition* para rodar **nativamente no Linux** via
**recompilação estática** (PowerPC → C++ → x86-64), no mesmo modelo de projetos como o
UnleashedRecomp. O usuário fornece a própria ISO; um launcher extrai o executável, recompila e
compila localmente um binário nativo. O repositório nunca contém arquivos do jogo nem código
derivado deles.

O **M1** entrega a fundação: repositório, documentação, launcher (CLI + GUI) com o pipeline
completo ISO → executável Linux, e um runtime-esqueleto que executa o código recompilado até o
primeiro import de kernel não implementado.

## 2. Decisões tomadas

| Tema | Decisão |
|------|---------|
| Nome | **LegacyRecomp** (binários `legacyrecomp` e `legacyrecomp-game`) |
| Escopo M1 | Pipeline completo + esqueleto do runtime; jogo rodando fica fora |
| Launcher | CLI **e** GUI no M1, sobre a mesma biblioteca |
| Stack | C++20, CMake + Ninja, Clang ≥ 18, SDL2, Dear ImGui |
| Abordagem | Submódulos hedge-dev (XenonRecomp/XenonAnalyse/XenonUtils, XenosRecomp) + runtime próprio, usando UnleashedRecomp como referência |
| Licença | **GPL-3.0** (permite portar trechos do UnleashedRecomp legalmente) |
| Versão alvo | XEX do pacote XBLA **+ Title Update 1** aplicado (ambos presentes no disco) |
| Idioma | Docs em português; código, identificadores e mensagens de commit em inglês |

## 3. Fatos levantados sobre a ISO

Leitura somente-leitura da ISO `Minecraft - Xbox 360 Edition (World).iso` (7,8 GB):

- Formato **XGD2**; partição do jogo (XDVDFS) no offset `0xFD90000`
  (magic `MICROSOFT*XBOX*MEDIA` em `0xFD90000 + 32*2048`).
- Title ID **`584111F7`**.
- Conteúdo da raiz:
  - `Default.xex` (1,6 MB): provável *loader* do disco, **não** o jogo (a confirmar na tarefa 1).
  - `Content/0000000000000000/584111F7/000D0000/49AAD81B…236C58` (118 MB): pacote **STFS**
    tipo `0x000D0000` (Arcade/XBLA): contém o jogo real.
  - `$TitleUpdate/584111F7/tu00000001_00000000` (30 MB): Title Update 1 (STFS com `.xexp`).
  - `$SystemUpdate/`, `AvatarAssetPack`, `media/`, `nxeart`: irrelevantes para o M1.

## 4. Arquitetura

### 4.1 Estrutura do repositório

```
LegacyRecomp/
├── CMakeLists.txt, CMakePresets.json
├── LICENSE, README.md, AGENTS.md, CLAUDE.md, .gitignore
├── thirdparty/            # submódulos fixados em commit
│   ├── XenonRecomp/       #   (inclui XenonAnalyse e XenonUtils)
│   ├── XenosRecomp/       #   (reservado; usado a partir do M3)
│   ├── SDL/ imgui/ fmt/ tomlplusplus/ Catch2/
├── assets/fonts/          # Inter + JetBrains Mono (OFL) da GUI, com fonts.lock
├── config/minecraft-tu1/
│   ├── recomp.toml        # config do XenonRecomp (endereços achados por RE)
│   └── hashes.toml        # SHA-256 esperados de cada arquivo de entrada/intermediário
├── src/
│   ├── common/            # Result<T>, Error, logging, hashing, paths XDG
│   ├── formats/           # libformats: XDVDFS, STFS, XEX2
│   ├── pipeline/          # libpipeline: etapas, estado, progresso
│   ├── launcher/cli/      # legacyrecomp (CLI)
│   ├── launcher/gui/      # legacyrecomp-gui (SDL2 + ImGui)
│   └── runtime/           # libruntime + template do jogo
├── tests/                 # Catch2 (unit) + integração opcional
├── tools/                 # geradores (stubs de imports, kernel-coverage)
├── docs/
└── .claude/skills/xbox360-re/
```

### 4.2 Unidades e fronteiras

| Unidade | Responsabilidade | Depende de |
|---------|------------------|------------|
| `common` | `Result<T>`/`Error`, log, SHA-256, resolução de paths | — |
| `formats` | Parsing puro de bytes → estruturas. Interface de leitura via `IReader` (random-access), sem UI nem estado global | `common` |
| `pipeline` | Orquestra etapas, cache/estado, progresso, invocação de ferramentas externas | `common`, `formats`, XenonUtils, binários XenonAnalyse/XenonRecomp |
| `launcher/cli` | Parse de argumentos, barra de progresso no terminal | `pipeline` |
| `launcher/gui` | Seletor de ISO, progresso, botão Jogar | `pipeline`, SDL2, ImGui |
| `runtime` | Memória do guest, contexto PPC, kernel HLE (stubs), plataforma SDL2, `main` | `common`, SDL2 |

O `runtime` não conhece o launcher. O launcher apenas instala `libruntime.a`, seus headers e um
**template CMake do jogo**, e os usa na etapa `build`.

### 4.3 Interfaces principais (esboço)

```cpp
namespace lr {
struct Error { ErrorCode code; std::string message; std::string hint; };
template <class T> class Result;   // valor ou Error; sem exceções na fronteira pública
}

namespace lr::formats {
struct IReader { virtual Result<void> read(uint64_t off, std::span<std::byte> dst) = 0;
                 virtual uint64_t size() const = 0; };
class XdvdfsImage;   // open(IReader&) → detecta XGD1/2/3; list(path), open_file(path) → IReader
class StfsPackage;   // open(IReader&) → header (content type, title id), list(), open_file()
class Xex2File;      // parse(bytes) → headers, seções, imports (por biblioteca), entry point
}

namespace lr::pipeline {
enum class StageId { Doctor, Extract, Verify, Patch, Analyse, Recompile, Build };
struct Progress { StageId stage; float fraction; std::string message; };
struct ProgressSink { virtual void on_progress(const Progress&) = 0;
                      virtual bool cancelled() const = 0; };
struct Options { std::filesystem::path iso, root; std::set<StageId> force; };
Result<void> run(const Options&, ProgressSink&);
Result<void> launch(const Options&);   // etapa "run"
}
```

## 5. Pipeline

| # | Etapa | Entrada → Saída | Notas |
|---|-------|-----------------|-------|
| 0 | `doctor` | — | clang ≥ 18, cmake ≥ 3.25, ninja, espaço livre, RAM. Falha com dica de instalação (ex.: `pacman -S clang lld cmake ninja`). |
| 1 | `extract` | ISO → `work/input/default.xex`, `work/input/default.xexp`, `game/…` | XDVDFS → pacote STFS `000D0000` (xex + assets) e STFS do TU1 (xexp). |
| 2 | `verify` | arquivos → ok/erro | SHA-256 contra `hashes.toml`. Divergência → `UnsupportedVersion` listando os hashes encontrados. |
| 3 | `patch` | xex + xexp → `work/image/default_tu1.xex` (descriptografado/descomprimido) | Via XenonUtils. |
| 4 | `analyse` | imagem → `work/analysis/switch_tables.toml` | XenonAnalyse. |
| 5 | `recompile` | imagem + `recomp.toml` + switch tables → `work/generated/*.cpp` | XenonRecomp. |
| 6 | `build` | gerado + `libruntime.a` + template → `bin/legacyrecomp-game` | CMake/Ninja; compila só o código gerado. |
| 7 | `run` | — | Executa o jogo com `--data <root>/game`. |

Recompilação de shaders (XenosRecomp) está **fora do M1**; o lugar da etapa fica reservado.

### 5.1 Pastas e estado

- Raiz: `$XDG_DATA_HOME/legacyrecomp` (padrão `~/.local/share/legacyrecomp`), sobrescrita por `--root`.
- `game/` (assets), `work/` (intermediários + `logs/<etapa>.log`), `bin/`.
- `state.json`: por etapa, hashes de entrada/saída e versão da ferramenta. Uma etapa é pulada
  quando os hashes de entrada batem e as saídas existem; `--force <etapa>` refaz aquela etapa e
  as seguintes.

### 5.2 CLI

```
legacyrecomp doctor
legacyrecomp install --iso <arquivo.iso> [--root DIR] [--force STAGE] [--jobs N]
legacyrecomp build   [--root DIR]          # re-roda a partir de recompile
legacyrecomp run     [--root DIR] [-- args do jogo]
```

### 5.3 GUI

Uma janela ImGui com: seleção da ISO (arrastar e soltar, diálogo do `zenity`/`kdialog` ou caminho
digitado), botão Instalar, lista de
etapas com status/progresso, painel de log rolável, botões Cancelar e Jogar. O pipeline roda numa
thread de trabalho; a UI lê os eventos de uma fila protegida por mutex.

Visual (Task 15b): tema escuro neutro com destaque verde, fontes Inter (interface) e JetBrains Mono
(registro), ambas SIL OFL 1.1, versionadas em `assets/fonts/` (origem e SHA-256 em
`assets/fonts/fonts.lock`) e instaladas em `share/legacyrecomp/fonts/`; sem elas a GUI usa a fonte
padrão do ImGui. Layout: cabeçalho com selo de estado; à esquerda, o cartão "Imagem do jogo"
(área de soltar a ISO, Procurar…, digitar caminho, pasta de dados) e o cartão Jogar; à direita, o
cartão "Instalação" com as etapas em lista vertical (ícones desenhados, barra de progresso na
etapa em andamento, alerta de erro inline); embaixo, o painel "Registro" recolhível, que abre
sozinho quando a instalação falha. Tamanhos escalam com o DPI da tela; o código fica em
`launcher/gui/{theme,widgets,ui_model}` (`ui_model` é puro e testado). Uso e opções: `docs/gui.md`.

## 6. Runtime (esqueleto do M1)

1. **Memória do guest:** reserva de 4 GB (`mmap PROT_NONE` + commit sob demanda); seções do
   XEX mapeadas no endereço base original; helpers `be<T>` para acesso big-endian.
2. **CPU:** `PPCContext` do XenonRecomp; pilha do guest e PCR/TLS mínimos.
3. **Kernel:** `tools/gen_imports` lê a tabela de imports do XEX (via `formats::Xex2File`) e gera
   um stub por import (`xboxkrnl.exe`, `xam.xex`, …). O stub padrão registra o nome e aborta com
   `UnimplementedImport`. Uma implementação real é uma função registrada que substitui o stub.
   O mesmo gerador escreve `docs/kernel-coverage.md` (implementados vs. total).
4. **Plataforma:** SDL2 abre uma janela e mantém o loop de eventos numa thread própria; o
   código do guest roda na thread principal do guest.
5. **Saída esperada no M1:** execução do entry point recompilado até o primeiro import não
   implementado, com relatório: nome do import, endereço de chamada e trilha dos imports
   chamados até ali.

## 7. Tratamento de erros

- APIs públicas retornam `Result<T>`; nada de exceções atravessando fronteiras de módulo.
- `Error{code, message, hint}`: todo erro mostrado ao usuário traz uma dica acionável.
- Ferramentas externas: código de saída ≠ 0 → `ToolFailed` com o caminho do log e as últimas
  20 linhas.
- Cancelamento cooperativo: as etapas checam `cancelled()` entre unidades de trabalho e deixam o
  estado consistente (a etapa cancelada fica marcada como incompleta).
- Runtime: os sinais `SIGSEGV`/`SIGILL` são capturados para imprimir o endereço do guest
  correspondente antes de abortar.

## 8. Testes

- **Unitários (Catch2), sem arquivos do jogo:**
  - `formats`: imagens XDVDFS/STFS e cabeçalhos XEX2 **sintéticos** montados em memória pelos
    próprios testes; casos de erro (magic inválido, offsets fora do limite, árvore cíclica).
  - `pipeline`: cache/retomada/`--force`/cancelamento usando etapas falsas.
  - `runtime`: memória do guest, `be<T>`, despacho de stubs.
- **Integração (opcional):** `LEGACYRECOMP_ISO=/caminho ctest -L integration` roda o pipeline
  real; sem a variável, o teste é pulado.
- Cada commit passa por `ctest -L unit`.

## 9. Documentação e processo

- `docs/`: `architecture.md`, `pipeline.md`, `formats/{xdvdfs,stfs,xex2}.md`,
  `reverse-engineering/` (guia Ghidra + XEXLoaderWV, tabela de endereços com a forma como cada
  um foi encontrado), `kernel-coverage.md`, `roadmap.md`, `adr/0001-static-recompilation.md`,
  `adr/0002-gpl-3.md`, `superpowers/{specs,plans}/`.
- **`AGENTS.md` canônico** (comandos, regras, convenções); o `CLAUDE.md` importa `@AGENTS.md` e
  acrescenta só o que é específico do Claude.
- **Regras invioláveis:** nada do jogo ou derivado dele no git; testes passando antes de cada
  commit; docs atualizadas junto com o código.
- **Commits:** Conventional Commits, pequenos (um por tarefa); `main` + branches de feature.
- **README:** banner, aviso legal (dump do próprio disco; sem afiliação com Mojang/Microsoft),
  quick start, diagrama Mermaid do pipeline, tabela de status, créditos (hedge-dev, Xenia).
- **Skill `.claude/skills/xbox360-re/SKILL.md`:** fluxo para achar os endereços exigidos pelo
  XenonRecomp (`savegpr`/`restgpr`, `savefpr`/`restfpr`, `savevmx`/`restvmx`,
  `setjmp`/`longjmp`), registrar as descobertas em `docs/reverse-engineering/` e validar recompilações.

## 10. Critérios de aceite do M1

1. `legacyrecomp install --iso <ISO do repo>` completa as etapas 0–6 nesta ISO.
2. A GUI executa a mesma instalação mostrando progresso por etapa.
3. Uma segunda execução pula todas as etapas (cache); `--force recompile` refaz 5–6.
4. `legacyrecomp run` inicia o jogo, executa o entry point recompilado e termina com o
   relatório do primeiro import não implementado (sem crash não diagnosticado).
5. `ctest -L unit` passa; `git ls-files` não contém nenhum arquivo do jogo ou derivado.
6. Docs, README, AGENTS.md, CLAUDE.md e a skill de RE existem e refletem o estado real.

## 11. Fora do escopo do M1

Recompilação de shaders, GPU/Vulkan, áudio, input, saves, rede, Windows/macOS e Flatpak.
(O AppImage entrou no M1 por decisão de 2026-10-14 — ver §14.)

## 12. Riscos e incógnitas

| Risco | Mitigação |
|-------|-----------|
| O `recomp.toml` exige endereços específicos da versão, encontrados por RE | Tarefa dedicada com Ghidra + XEXLoaderWV; documentar cada endereço e como foi achado |
| Jump tables mal detectadas pelo XenonAnalyse | Overrides manuais no `recomp.toml`; erro do XenonRecomp aponta o endereço |
| O pacote XBLA pode exigir um tratamento de STFS diferente do esperado (LIVE vs CON, hash tables) | O leitor STFS cobre os dois e é testado com fixtures sintéticas |
| Compilar o código gerado é pesado (tempo/RAM) | O gerador divide em vários `.cpp`; `--jobs` configurável; o `doctor` avisa se a RAM for baixa |
| O `.xexp` do TU1 pode não casar com o XEX extraído | A etapa `verify` checa os hashes antes do `patch`; o erro informa qual arquivo diverge |
| Pinagem dos submódulos quebrar com updates upstream | Submódulos fixados por commit; atualização só via PR dedicado |

## 13. Roadmap posterior

M2 kernel HLE (threads, arquivos, sincronização) → M3 GPU (Vulkan + XenosRecomp) →
M4 áudio, input e saves → M5 jogável (menu e mundo).

## 14. Adendo (2026-10-14): distribuição em AppImage

Decisão do usuário: o LegacyRecomp é distribuído como **AppImage autossuficiente** e isso entra no M1
(Task 17 do plano).

- O AppImage leva o launcher (CLI + GUI), os recursos de `share/legacyrecomp/` **e o toolchain** usado
  para compilar o jogo na máquina do usuário: o usuário só baixa, aponta a ISO e joga, sem instalar
  pacotes.
- Toolchain embutido: **Zig** (`zig c++`, que traz clang, libc++ e headers/objetos da glibc e
  compila mirando uma glibc antiga, ex. 2.31, para rodar em qualquer distro) + `cmake` + `ninja`
  estáticos + headers e `.so` do SDL2. Plano B, se o Zig não compilar o código gerado com as flags
  necessárias: LLVM oficial + sysroot mínimo de uma distro antiga.
- Dentro do AppImage (`$APPDIR` definido) o `doctor` e a etapa `build` usam o toolchain embutido; fora
  dele, continuam usando o do sistema.
- Critério de aceite adicional: `packaging/appimage/build.sh` gera `LegacyRecomp-x86_64.AppImage`;
  numa pasta de dados limpa, `./LegacyRecomp-x86_64.AppImage install --iso <ISO>` e `run -- --headless`
  produzem o mesmo resultado do M1 (código 3 em `NtAllocateVirtualMemory`) **com o PATH sem
  clang/cmake/ninja**.
