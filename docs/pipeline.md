# Pipeline de instalação

`legacyrecomp install --iso <arquivo>` (ou o botão **Instalar** da GUI, ver `docs/gui.md`) executa sete
etapas em ordem. Cada etapa tem um *fingerprint* (hash das suas entradas) e uma lista de saídas; o estado
fica em `work/state.toml`. Tudo roda na máquina do usuário e nada do jogo sai dela.

## Etapas

| # | Etapa | O que faz | Saídas (relativas à pasta de dados) |
|---|---|---|---|
| 1 | `doctor` | Confere ferramentas (clang, lld, cmake, ninja, SDL2; no AppImage, os arquivos do toolchain embutido), CPU com AVX, 8 GiB livres e RAM | nenhuma |
| 2 | `extract` | Lê a ISO (XDVDFS), acha o pacote STFS do jogo e o do TU1 e extrai `default.xex`, `default.xexp` e os dados do jogo | `work/input/package.xex`, `work/input/tu.xexp`, `game/.extracted` |
| 3 | `verify` | Compara o SHA-256 dos arquivos extraídos com `config/minecraft-tu1/game.toml` | nenhuma |
| 4 | `patch` | Aplica o `.xexp` ao XEX (XenonUtils `XexPatcher`), gerando um XEX2 sem criptografia e sem compressão | `work/image/default_tu1.xex` |
| 5 | `analyse` | Roda o XenonAnalyse e a varredura de `switch` própria (jump tables) | `work/analysis/switch_tables.toml`, `work/analysis/functions.toml` |
| 6 | `recompile` | Roda o XenonRecomp com `config/minecraft-tu1/recomp.toml`; falha se houver qualquer linha `ERROR:` (o XenonRecomp sai com 0 mesmo gerando código errado) | `work/generated/ppc_recomp_shared.h`, `ppc_func_mapping.cpp`, `ppc_config.h` (mais ~215 `ppc_recomp.*.cpp`) |
| 7 | `build` | Gera os stubs de importação, compila o C++ gerado com o `main.cpp` e as bibliotecas do runtime (template CMake/Ninja) e instala o binário. Usa o clang/cmake/ninja do sistema ou, no AppImage, o toolchain embutido (ver "Toolchain") | `bin/legacyrecomp-game` |

O código gerado tem cerca de 160 MB, o `recompile` + `build` levam alguns minutos e usam bastante RAM por
job (use `--jobs <n>` para limitar).

## Toolchain

`lr::pipeline::resolve_toolchain` (`src/pipeline/toolchain.h`) escolhe o compilador da etapa `build`:

- **Embutido** (AppImage): se existe `share/legacyrecomp/toolchain/`, usa `toolchain/bin/c++` (`zig c++
  -target x86_64-linux-gnu.2.31`), `toolchain/cmake/bin/cmake`, `toolchain/bin/ninja` e o SDL2 estático de
  `toolchain/sdl2/`. O `doctor` só confere se esses arquivos estão lá; se faltar algum, o erro é `ToolMissing`
  com a dica de que o AppImage está corrompido. Detalhes em `docs/packaging.md`.
- **Sistema** (instalação a partir do código-fonte): `clang`, `clang++`, `cmake` e `ninja` do `PATH` e o SDL2
  do sistema. O `doctor` confere as versões (clang++ ≥ 18, cmake ≥ 3.25) e o `pkg-config sdl2`.

## Layout da pasta de dados

Precedência: `--root` > variável `LEGACYRECOMP_ROOT` > `$XDG_DATA_HOME/legacyrecomp` (ou
`~/.local/share/legacyrecomp`).

```
<root>/
  game/                  dados extraídos do jogo (pertence ao LegacyRecomp: é apagado a cada extract)
  bin/legacyrecomp-game  executável compilado
  work/
    state.toml           estado das etapas
    logs/                log de cada ferramenta externa
    input/               package.xex, tu.xexp
    image/               default_tu1.xex (imagem final)
    analysis/            switch_tables.toml, functions.toml
    generated/           C++ gerado pelo XenonRecomp
    build/               árvore de build do jogo
```

**Não use um tmpfs (disco em RAM, como `/tmp` em várias distros) como pasta de dados.** O código gerado e a
compilação ocupam vários GiB; num tmpfs isso consome a mesma RAM de que o compilador precisa e a
instalação falha por falta de memória. Use um disco com pelo menos 8 GiB livres.

## `state.toml`, cache e `--force`

```toml
[stages.extract]
complete = true
fingerprint = "..."
```

- Uma etapa é pulada quando está `complete`, o fingerprint atual é igual ao guardado e todas as saídas
  existem. Segunda execução de `install` sem mudanças não roda nenhuma etapa.
- Ao começar uma etapa, ela é marcada como não completa; só `mark_complete` depois do sucesso. Falha ou
  cancelamento nunca deixa uma etapa "pronta".
- Uma etapa suja (re-executada) suja todas as seguintes: `--force recompile` roda `recompile` e `build`;
  `--force extract` roda tudo depois do `doctor`.
- `--force <etapa[,etapa]>` pode ser repetido. Nomes: `doctor`, `extract`, `verify`, `patch`, `analyse`,
  `recompile`, `build`.
- O carregamento é tolerante: chave ausente ou com tipo errado significa "não completo" e só causa
  re-execução. TOML ilegível é `InvalidFormat`.
- O fingerprint do `doctor` é constante (`doctor-1`): ele é cacheado como as outras etapas, então mudanças
  no ambiente (instalar/remover clang) só são notadas se uma etapa posterior rodar e falhar, ou com
  `--force doctor`. `legacyrecomp doctor` sempre confere o sistema.
- `legacyrecomp build` roda `recompile` + `build` sem ISO. Sem `--iso`, o `extract` só vale se já estiver
  completo com as saídas presentes; senão o erro pede `legacyrecomp install --iso <arquivo>`.
  `--force extract` sem `--iso` é erro de uso.

## Códigos de erro e de saída

`lr::ErrorCode` aparece como `error: <mensagem>` e, quando há, `hint: <sugestão>`.

| Saída da CLI | Significado |
|---|---|
| 0 | sucesso |
| 1 | erro de pipeline (qualquer `ErrorCode` abaixo) |
| 2 | erro de uso (argumentos inválidos) |
| 130 | cancelado (Ctrl+C, SIGTERM ou SIGHUP); etapas concluídas continuam em cache, rode de novo para continuar |

Cancelamento na CLI (`install`/`build`): as ferramentas rodam em grupo de processos próprio e não recebem o
Ctrl+C do terminal. O primeiro SIGINT, SIGTERM ou SIGHUP (terminal fechado) marca o cancelamento: a etapa em
curso manda SIGTERM ao grupo da ferramenta (SIGKILL após 3 s) e a CLI sai com 130. Um segundo sinal mata o
grupo da ferramenta com SIGKILL e encerra a CLI na hora, pelo próprio sinal; nenhum cmake/ninja/clang fica
órfão.

`legacyrecomp run` devolve o código do jogo:

| Saída do jogo | Significado |
|---|---|
| 0 | fim normal |
| 2 | uso ou preparação falhou (imagem ausente, base diferente da compilada, mapeamento de memória) |
| 3 | importação do kernel não implementada (esperado no M1; o relatório vai para stderr) |
| 4 | falha de execução (SIGSEGV, SIGILL etc. tratada pelo handler) |

## Solução de problemas por código de erro

| `ErrorCode` | Causa comum | O que fazer |
|---|---|---|
| `ToolMissing` | clang, lld, cmake, ninja ou SDL2 ausentes (etapa `doctor` ou etapas que usam a ferramenta); no AppImage, arquivo do toolchain embutido ausente | `sudo pacman -S --needed clang lld llvm cmake ninja sdl2` (equivalente na sua distro); no AppImage, baixe-o de novo |
| `InsufficientResources` | CPU sem AVX, ou menos de 8 GiB livres na pasta de dados | O jogo é compilado com `-march=sandybridge`: precisa de AVX. Libere disco ou use `--root` em outro volume |
| `NotFound` | ISO ou arquivo esperado ausente; `build`/`run` antes de instalar | `legacyrecomp install --iso <arquivo>` |
| `InvalidFormat` | ISO não é XDVDFS, pacote STFS ou XEX inválido, `state.toml` corrompido | Confira a ISO; apague `work/state.toml` para recomeçar |
| `UnsupportedVersion` | Hash diferente do esperado (outra região, disco modificado, TU diferente); a mensagem nomeia o arquivo e o hash achado | Use a ISO XGD2 do Minecraft: Xbox 360 Edition com o TU1; `legacyrecomp inspect --iso` mostra o que há no disco |
| `ToolFailed` | XenonRecomp, XenonAnalyse ou compilador saíram com erro | Veja `work/logs/`; para falta de memória reduza `--jobs` |
| `IoError` | Falha ao ler ou escrever (permissão, disco cheio) | Confira permissões e espaço; não use tmpfs como pasta de dados |
| `Cancelled` | Ctrl+C ou botão Cancelar | Rode o comando de novo |
| `UnimplementedImport` | O jogo chamou uma importação do kernel sem implementação (saída 3) | Esperado no M1; veja `docs/kernel-coverage.md` e `docs/roadmap.md` |
| `Internal` | Bug | Abra uma issue com o log de `work/logs/` |

## Requisito de CPU

O código gerado é compilado com `-march=sandybridge`, portanto a CPU precisa do conjunto AVX. O `doctor`
verifica e recusa instalar sem ele (`InsufficientResources`). Se um `SIGILL` acontecer no host, o handler de
falhas imprime uma dica sobre isso.
