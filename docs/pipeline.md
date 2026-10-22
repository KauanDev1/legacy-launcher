# Pipeline de instalação

`legacyrecomp install --iso <arquivo>` (ou o botão **Instalar** da GUI, ver `docs/gui.md`) executa oito
etapas em ordem. Cada etapa tem um *fingerprint* (hash das suas entradas) e uma lista de saídas; o estado
fica em `work/state.toml`. Tudo roda na máquina do usuário e nada do jogo sai dela.

## Etapas

| # | Etapa | O que faz | Saídas (relativas à pasta de dados) |
|---|---|---|---|
| 1 | `doctor` | Confere ferramentas (clang++ ≥ 18, cmake ≥ 3.25, ninja, `pkg-config sdl2`; no AppImage, os arquivos do toolchain embutido), CPU com AVX, 8 GiB livres e RAM | nenhuma |
| 2 | `extract` | Lê a ISO (XDVDFS), acha o pacote STFS do jogo e o do TU1 e extrai `default.xex`, `default.xexp` e os dados do jogo | `work/input/package.xex`, `work/input/tu.xexp`, `game/.extracted` |
| 3 | `verify` | Compara o SHA-256 dos arquivos extraídos com `config/minecraft-tu1/game.toml` | nenhuma |
| 4 | `patch` | Aplica o `.xexp` ao XEX (XenonUtils `XexPatcher`), gerando um XEX2 sem criptografia e sem compressão | `work/image/default_tu1.xex` |
| 5 | `analyse` | Roda o XenonAnalyse e a varredura de `switch` própria (jump tables) | `work/analysis/switch_tables.toml`, `work/analysis/functions.toml` |
| 6 | `recompile` | Roda o XenonRecomp com `config/minecraft-tu1/recomp.toml`; falha se houver qualquer linha `ERROR:` (o XenonRecomp sai com 0 mesmo gerando código errado) | `work/generated/ppc_recomp_shared.h`, `ppc_func_mapping.cpp`, `ppc_config.h` (mais ~215 `ppc_recomp.*.cpp`) |
| 7 | `shaders` | Copia `work/image/default_tu1.xex` para `work/shaders/in/` e roda o XenosRecomp (patcheado, `tools/XenosRecomp`, com o DXC do sistema) sobre ele, passando o prelúdio HLSL `tools/xenos_shader_common.h`; o `translated N of M` impresso pela ferramenta precisa ter N == M ≥ 1, senão (ou se o arquivo gerado estiver vazio ou truncado) a etapa falha com `ToolFailed` e nenhum arquivo é deixado. Fingerprint: sha256 da imagem, commit do XenosRecomp (com o hash dos patches) e versão do DXC. É uma etapa *independente*: uma etapa anterior re-executada (por exemplo `recompile`, que `legacyrecomp build` força) não a suja; só uma mudança no próprio fingerprint, uma saída ausente ou `--force shaders` a re-executam O cache é lido em tempo de execução pelo hash do microcódigo (ver `docs/architecture.md`, "Contrato dos shaders traduzidos") | `work/shaders/shader_cache.cpp` (fora de `work/generated/`, que o `recompile` apaga; derivado do jogo, nunca vai para o git; exporta `lr_shader_cache` e `lr_shader_cache_count`, `lr_shader_vertex_inputs` e `lr_shader_vertex_input_count`, e o blob SPIR-V comprimido com zstd `lr_shader_blob` com `lr_shader_blob_size` e `lr_shader_blob_uncompressed_size`); `work/shaders/hlsl/` (HLSL de depuração) |
| 8 | `build` | Gera os thunks de importação (`import_stubs.cpp`, ver `docs/architecture.md`, "Kernel"), compila o C++ gerado e o `shader_cache.cpp` (passado ao template por `-DLR_SHADER_CACHE=<root>/work/shaders/shader_cache.cpp`; o fingerprint do `build` inclui o hash dele, então um cache novo recompila o jogo) com o `main.cpp` e as bibliotecas do runtime (template CMake/Ninja) e instala o binário. Usa o clang++/cmake/ninja do sistema ou, no AppImage, o toolchain embutido (ver "Toolchain") | `bin/legacyrecomp-game` |

O código gerado tem cerca de 160 MB, o `recompile` + `shaders` + `build` levam alguns minutos e usam bastante RAM por
job (use `--jobs <n>` para limitar).

Sem o XenosRecomp no pacote (AppImage sem DXC, ver `docs/adr/0005-m3-plan-deviations.md`), a etapa `shaders` falha
com `ToolMissing` dizendo qual ferramenta falta e apontando o aviso do `doctor`; o `doctor` só falha por falta de
DXC quando o XenosRecomp está presente. O log fica em `work/logs/shaders.log`.

O `analyse` também procura funções sem entrada no `.pdata` que só são alcançadas por ponteiro (vtables, tabelas
de callbacks, o comparador de um `qsort`), que o XenonRecomp nunca recompilaria
(`src/pipeline/function_discovery.cpp`). Candidatas: palavras de 32 bits (big-endian, alinhadas) fora das seções
de código cujo valor aponta para código e endereços de código montados com `lis` + `addi`. Alvos de `bl` nunca
são emitidos (o XenonRecomp já cria uma função em cada um, ver ADR 0005), só servem de fronteira. Uma
candidata só vale se estiver num vão entre funções conhecidas (`.pdata`, funções derivadas dos `switch` e as
escritas à mão no `recomp.toml`; os 8 bytes de dados do handler antes de uma função com exception handler contam
como dela), se a palavra anterior não puder cair nela (`blr`, `bctr`, `b`, padding zero) e se terminar num
desvio incondicional sem desvio adiante pendente. É rejeitada se correr para dentro de outra função (ou de um
alvo de `bl`), do padding ou do fim do vão, se tiver um `bctr` alimentado por load indexado (`switch` não
resolvido), se uma chamada ou um desvio de fora cair no seu interior ou se contiver um rótulo de `switch`. Na dúvida ela é
descartada: uma fronteira errada gera código errado, uma função perdida só falha com relatório claro. As
aceitas vão para `work/analysis/functions.toml` e o `work/logs/analyse.log` ganha a linha
`pointer-only functions: N`.

O `recompile` nunca falha por avisos do XenonRecomp. Ao terminar, ele conta as instruções que o XenonRecomp não soube traduzir
(`Unrecognized instruction at ...`) e acrescenta ao fim de `work/logs/recompile.log`, e emite como última mensagem de progresso,
a linha `unrecognized instructions: N (top: bdzf 754, stfsu 507, ...)` (até 10 mnemônicos, mais `, float16_4 packs: K` se houver).
`python3 -I tools/recomp_warnings.py work/logs/recompile.log` imprime a tabela completa por mnemônico e o endereço de cada
`float16_4` (o aviso só traz o endereço, sem operandos); lê apenas o log, nunca a imagem.

## Toolchain

`lr::pipeline::resolve_toolchain` (`src/pipeline/toolchain.h`) escolhe o compilador da etapa `build`:

- **Embutido** (AppImage): se existe `share/legacyrecomp/toolchain/`, usa `toolchain/bin/c++` (`zig c++
  -target x86_64-linux-gnu.2.31`), `toolchain/cmake/bin/cmake`, `toolchain/bin/ninja` e o SDL2 estático de
  `toolchain/sdl2/`. O `doctor` só confere se esses arquivos estão lá; se faltar algum, o erro é `ToolMissing`
  com a dica de que o AppImage está corrompido. Detalhes em `docs/packaging.md`.
- **Sistema** (instalação a partir do código-fonte): `clang++`, `cmake` e `ninja` do `PATH` e o SDL2 do
  sistema. O template do jogo é só C++ (`LANGUAGES CXX`), então nenhum compilador C é exigido. O `doctor`
  confere exatamente o que a etapa `build` usa: as versões (clang++ ≥ 18, cmake ≥ 3.25), o `ninja` e o
  `pkg-config sdl2`. O `lld` não é usado (o link é o padrão do clang).

## Layout da pasta de dados

Precedência: `--root` > variável `LEGACYRECOMP_ROOT` > `$XDG_DATA_HOME/legacyrecomp` (ou
`~/.local/share/legacyrecomp`).

```
<root>/
  game/                  dados extraídos do jogo (pertence ao LegacyRecomp: é apagado a cada extract)
  user/                  dados do jogador (saves, cache:): o pipeline nunca apaga, nem no extract (ao contrário de game/)
  bin/legacyrecomp-game  executável compilado
  work/
    state.toml           estado das etapas
    logs/                log de cada ferramenta externa
    input/               package.xex, tu.xexp
    image/               default_tu1.xex (imagem final)
    analysis/            switch_tables.toml, functions.toml
    generated/           C++ gerado pelo XenonRecomp
    shaders/             entrada (in/), HLSL (hlsl/) e shader_cache.cpp do XenosRecomp
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
- Uma etapa suja (re-executada) suja todas as seguintes: `--force recompile` roda `recompile`, `shaders` e `build`;
  `--force extract` roda tudo depois do `doctor`.
- `--force <etapa[,etapa]>` pode ser repetido. Nomes: `doctor`, `extract`, `verify`, `patch`, `analyse`,
  `recompile`, `shaders`, `build`.
- O carregamento é tolerante: chave ausente ou com tipo errado significa "não completo" e só causa
  re-execução. TOML ilegível é `InvalidFormat`.
- O fingerprint do `doctor` é constante (`doctor-1`): ele é cacheado como as outras etapas, então mudanças
  no ambiente (instalar/remover clang) só são notadas se uma etapa posterior rodar e falhar, ou com
  `--force doctor`. `legacyrecomp doctor` sempre confere o sistema.
- `legacyrecomp build` roda `recompile` + `shaders` + `build` sem ISO. Sem `--iso`, o `extract` só vale se já estiver
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

`legacyrecomp run` devolve o código do jogo, com duas exceções do próprio launcher:

| Saída de `run` | Significado |
|---|---|
| 0 | fim normal |
| 1 | o launcher não conseguiu iniciar o jogo (jogo não compilado, `work/image/default_tu1.xex` ausente, falha no `posix_spawn`); a mensagem vai para stderr como `error:` |
| 2 | uso ou preparação falhou dentro do jogo (imagem ausente, base diferente da compilada, mapeamento de memória) |
| 3 | importação do kernel não implementada (o relatório vai para stderr e nomeia a importação; veja `docs/kernel-coverage.md`) |
| 4 | falha de execução (SIGSEGV, SIGILL etc. tratada pelo handler) |
| 5 | erro fatal do convidado (`lr::kernel::guest_fatal`: o jogo chegou a um estado de onde o kernel não segue; motivo em stderr) |
| 6 | deadlock: todas as threads do jogo ficaram bloqueadas além do limite do detector; stderr lista o que cada uma espera |
| 128 + sinal | o jogo morreu por um sinal não tratado (por exemplo 130 com Ctrl+C, 137 com SIGKILL) |

Para cada código diferente de 0, a CLI (em inglês) e a GUI (em português) mostram uma explicação vinda de
`lr::pipeline::describe_game_exit` (`src/pipeline/game_exit.h`). Nos códigos 3 a 6 ela sugere rodar
`legacyrecomp run -- --trace-imports` e ler `<raiz>/work/logs/game-trace.log`, o rastro das chamadas ao kernel
que antecederam a parada.

## Solução de problemas por código de erro

| `ErrorCode` | Causa comum | O que fazer |
|---|---|---|
| `ToolMissing` | clang++, cmake, ninja ou SDL2/pkg-config ausentes (etapa `doctor` ou etapas que usam a ferramenta); no AppImage, arquivo do toolchain embutido ausente | `sudo pacman -S --needed clang cmake ninja sdl2 pkgconf` (equivalente na sua distro, como mostra a dica do erro); no AppImage, baixe-o de novo |
| `InsufficientResources` | CPU sem AVX, ou menos de 8 GiB livres na pasta de dados | O jogo é compilado com `-march=sandybridge`: precisa de AVX. Libere disco ou use `--root` em outro volume |
| `NotFound` | ISO ou arquivo esperado ausente; `build`/`run` antes de instalar | `legacyrecomp install --iso <arquivo>` |
| `InvalidFormat` | ISO não é XDVDFS, pacote STFS ou XEX inválido, `state.toml` corrompido | Confira a ISO; apague `work/state.toml` para recomeçar |
| `UnsupportedVersion` | Hash diferente do esperado (outra região, disco modificado, TU diferente); a mensagem nomeia o arquivo e o hash achado | Use a ISO XGD2 do Minecraft: Xbox 360 Edition com o TU1; `legacyrecomp inspect --iso` mostra o que há no disco |
| `ToolFailed` | XenonRecomp, XenonAnalyse ou compilador saíram com erro | Veja `work/logs/`; para falta de memória reduza `--jobs` |
| `IoError` | Falha ao ler ou escrever (permissão, disco cheio) | Confira permissões e espaço; não use tmpfs como pasta de dados |
| `Cancelled` | Ctrl+C ou botão Cancelar | Rode o comando de novo |
| `UnimplementedImport` | O jogo chamou uma importação do kernel sem implementação (saída 3) | Veja o relatório em stderr, `docs/kernel-coverage.md` e `docs/roadmap.md`; `legacyrecomp run -- --trace-imports` grava as chamadas anteriores em `work/logs/game-trace.log` |
| `Internal` | Bug | Abra uma issue com o log de `work/logs/` |

## Requisito de CPU

O código gerado é compilado com `-march=sandybridge`, portanto a CPU precisa do conjunto AVX. O `doctor`
verifica e recusa instalar sem ele (`InsufficientResources`). Se um `SIGILL` acontecer no host, o handler de
falhas imprime uma dica sobre isso.
