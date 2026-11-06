<!-- SPDX-License-Identifier: MIT -->
# Passagem de contexto

Ponto de entrada único para retomar o LegacyRecomp. Escrito em 2026-11-06, na mudança do desenvolvimento de um
Windows (só pelo contêiner Docker) para uma máquina Linux nativa. Tudo o que existia só no Windows (o registro de
execução em `.superpowers/sdd/`, ignorado pelo git, e a memória do Claude em `~/.claude/projects/.../memory/`)
está resumido aqui ou copiado para o repositório.

Leia nesta ordem: este arquivo, `AGENTS.md`, `CLAUDE.md`, `docs/m3-status.md`, `docs/roadmap.md`. Os detalhes
técnicos estão em `docs/architecture.md`, `docs/adr/0005-m3-plan-deviations.md` (M1 a M16) e no registro do M3,
copiado em `docs/superpowers/ledgers/2026-10-19-legacyrecomp-m3/`.

Convenção deste arquivo: **(fato)** foi medido ou está no código; **(palpite)** é inferência ainda não conferida.

## Sumário

1. [Estado atual](#estado-atual)
2. [Como retomar no Linux](#como-retomar-no-linux)
3. [O que falta para fechar o M3](#o-que-falta-para-fechar-o-m3)
4. [Itens estacionados e riscos conhecidos](#itens-estacionados-e-riscos-conhecidos)
5. [M4 e M5](#m4-e-m5)
6. [Processo de trabalho e regras do dono](#processo-de-trabalho-e-regras-do-dono)
7. [Decisões e rulings importantes](#decisões-e-rulings-importantes)
8. [Ferramentas de diagnóstico](#ferramentas-de-diagnóstico)

## Estado atual

Branch `feat/m3`. Antes do commit desta passagem, o HEAD era `6245bd6`, com a árvore limpa.

### Git e remoto

- `origin` = `https://github.com/KauanDev1/legacy-launcher.git`.
- `origin/feat/m3` = `e004840`. O PR #1 juntou essa branch (que já continha o M2) em `main`: o merge é `e94d5e6`
  (2026-10-28). Também existe `origin/feat/m1`, antiga.
- **10 commits locais ainda não publicados** (`20eef10`..`6245bd6`), mais este commit de documentação. Na hora da
  mudança, o controlador ia perguntar ao dono se publica antes. Se não publicou, ver "Levar os commits" abaixo.
- Branches locais: `main`, `feat/m2` (já contida em `feat/m3`) e `feat/m3`.

### Marco a marco

| Marco | Estado | Números e commits |
|---|---|---|
| M1 | feito | Pipeline completo (doctor..build), formatos (XDVDFS, STFS, XEX2), CLI e GUI, AppImage autossuficiente com zig (Task 17 do M1). O jogo compilava e parava na primeira importação sem implementação (saída 3) |
| M2 | feito | Kernel HLE (memória, threads, sincronização, timers/DPC, arquivos, XAM mínima, LZX, printf). Boot sem tela até `VdInitializeEngines` (saída 3); 0 avisos do XenonRecomp com os patches de `thirdparty/patches/xenonrecomp/` |
| M3 | Tarefas 1 a 22 feitas; 23 cancelada (redefinida); 24 pendente | Ver abaixo |
| M4 | não começou | Nem spec nem plano. Ver [M4 e M5](#m4-e-m5) |
| M5 | não começou | Idem |

### M3 em números (fato)

- **Tela de título alcançada sem janela** (lavapipe no contêiner), com os limites objetivos da Tarefa 22: ≥ 64
  cores, luminância média entre 0,05 e 0,95, ≥ 2% de bordas, 0 mensagens da camada de validação e 600 swaps sem
  sair. Números de `tools/frame_stats.py`:
  - N=300: `colors=73762 mean_luminance=0.3889 edge_fraction=0.0488`;
  - N=600: `colors=60108 mean_luminance=0.3691 edge_fraction=0.0490`;
  - N=600 com `LR_VULKAN_VALIDATION=1`: `colors=70111 mean_luminance=0.3896 edge_fraction=0.0477`, 0 linhas
    `[vulkan]`.
  - Os números variam um pouco entre rodadas (a tela é animada). Antes das correções da Tarefa 22: 1 cor, depois 4.
- **Teste de integração** `real ISO: the game reaches its title screen` (`tests/test_integration_m3.cpp`): passou na
  ISO em 390 s (a rodada anterior levou 182 s), com uma raiz já instalada (`LEGACYRECOMP_TEST_ROOT=/data`). Ele
  exige que a camada de validação tenha rodado e faz SKIP sem ela.
- **Shaders**: `[gpu] shaders: keys=10 vs=5 ps=5 failed=0` (10 chaves, todas traduzidas pelo tradutor próprio).
  O portão do tradutor (Tarefa 15.11) mediu 8 chaves, 0 falhas, e 100% vindas do cache em disco na segunda execução.
- **Desempenho no lavapipe**: 600 swaps em cerca de 133 s de execução; cerca de 213 desenhos, 1 resolve e 1 swap
  por quadro na tela de título.
- **Recompilador** (varredura `switch-scan-18`): 229 jump tables encontradas, 228 mantidas, 46 fronteiras de folha,
  14 sítios `bctr` sem guarda ainda sem solução, 3 248 funções só por ponteiro. A recompilação sai limpa.
- **Testes de unidade**: 763 no contêiner, com 1 SKIP que já existia.
- **M3a** (Tarefa 12): o jogo alimentou a GPU por 757 quadros com o `NullBackend`; 362 409 chamadas de importação
  (117 distintas), 15 threads do jogo.

### M3 tarefa a tarefa

| Tarefa | Conteúdo | Commits |
|---|---|---|
| 1–4 | Recompilador: jump tables relativas, funções só por ponteiro, relatório de chamada indireta sem função, falhas do experimento resolvidas | `24d4e82`..`87ec3a5` |
| 5 | Vulkan no contêiner e no build (volk, VMA, Vulkan-Headers) | `ec5a3ac`, `fa421eb` |
| 6 | Submódulo XenosRecomp, patches genéricos de submódulo, DXC do sistema (aposentados na 15.1) | `10e73aa`..`8e067e1` |
| 7–9 | Registradores do Xenos, processador PM4, tradução física→convidado, janela MMIO | `8d556b6`..`5b483cc` |
| 10 | `GpuSystem`: ring buffer, write-back, interrupções, importações `Vd*` | `2ffff53`, `ac0e9bc` |
| 11 | Áudio mudo, caixa de mensagem no terminal, `XamLoaderLaunchTitle` fatal | `c6b111a` |
| 12 | [ISO] M3a: comandos de GPU fluem, sem caixa de erro | `e5e0851`..`af19927` |
| 13–15 | Fork do XenosRecomp, etapa `shaders`, portão do plano A: **reprovado** (ADR 0005 M7) | `0523eda`..`a17bae3` |
| 15.1 | Retirada da tradução na instalação, do XenosRecomp e do DXC | `34647d9` |
| 15.2–15.10 | Tradutor próprio microcódigo → SPIR-V (plano B) e cache em disco | `4f8a91d`..`4030e2e` |
| 15.11 | [ISO] Portão do tradutor: aprovado (ADR 0005 M9) | `b81d6ca`, `26476f4` |
| 16–20 | Shaders GLSL do renderizador, apresentador, EDRAM, texturas e amostradores, caminho de desenho | `b8ff0c3`..`498725c` |
| 21 | Jogo falso desenha um quadrado com textura de ponta a ponta | `7fb89df` |
| 22 | [ISO] Até a tela de título, revisão limpa | `75d8ee9`..`6245bd6` |
| 23 | Conferência visual pelo WSLg: **cancelada** pelo dono em 2026-11-06; redefinida abaixo | nenhum |
| 24 | AppImage, licenças, documentação, fechamento do M3 | pendente |

### O que a Tarefa 22 achou (fato)

Ela começou com o quadro de uma cor só e três problemas. Os dois primeiros tinham a mesma raiz.

- **a. Texturas de fonte zeradas** e **b. vertex shader `CB7E755E3A72E353` quase todo zerado**. Causa: o runtime
  não espelhava as três vistas da memória física (`0xA0`, `0xC0`, `0xE0`). O jogo preenche a textura pela vista
  `0xE0` e o conversor (`0x826529A0`) lê pela `0xC0`; a cópia do microcódigo para o buffer de comandos também lia
  por outra vista. Correção `563485e`: um `memfd` de 512 MiB mapeado nas três vistas (ADR 0005 M14). Com ela o jogo
  carrega `BF9066893FA79ADD` no lugar de `CB7E…`. A vigia de memória (`--watch-guest`, com `:rw`) foi o que achou o
  leitor.
- **c. Coordenadas de textura erradas no logo e nos botões** (VS `E91536F60A69ABF3` + PS `C3B8C62B63E1BB9E`). O
  vertex shader estava certo: um reprodutor sintético com a mesma forma de instruções passa no lavapipe. A causa:
  o runtime lia `SQ_INTERPOLATOR_CNTL.param_shade` como máscara de sombreamento flat (decisão do planejador na
  Tarefa 15.4), e o jogo liga o bit 0 nesses desenhos. Correção `d2f0584`: nada mais é `Flat`, como no Xenia
  (ADR 0005 M15). Não houve aumento de `kTranslatorVersion`: a saída do tradutor por chave não mudou, só a chave.
- Hipóteses descartadas no caminho: `dcbz`/`dcbz128` zerando a linha errada (semântica fixada por teste,
  `67f20e8`), "ninguém escreve nas texturas", retângulo de resolve, chaves de render target, visibilidade dos
  descritores, tamanho e deslocamento do vertex fetch, arquivos faltando, importações falhando.
- Outras correções da tarefa:
  - tabela inline logo após o `bctr` (`0x8235B7AC` e mais 4 sítios), limite de função que salta para uma faixa de
    `switch` de folha;
  - a descoberta de funções não emite mais funções em thunks de importação (`8871b56`), e duplicatas na tabela de
    funções viram aviso (`8c6b883`, ADR 0005 M16);
  - `XMACreateContext`/`XMAReleaseContext` silenciosos (`0079357`, e `4568791` para liberar só contextos entregues);
  - `XamBackgroundDownloadSetMode`/`GetMode` em modo offline (`90ff270`).
- Rodada de correção 1 da revisão (`4568791`, `0573e15`, `4e6b9f6`, `6245bd6`):
  - o filtro de desvios "perdidos" decodificados dos bytes de uma tabela inline (`switch-scan-18`): a saída na TU1
    ficou byte a byte igual à `switch-scan-17`;
  - o teste exige que a validação tenha rodado de verdade;
  - os intervalos dos thunks vêm do tamanho do símbolo (`FunctionRange`).

A exceção do dono para ler a desmontagem do VS `E91536F60A69ABF3` foi **usada e está encerrada**: o que foi
lido nunca foi commitado, e qualquer nova exceção precisa de outra autorização do dono.

## Como retomar no Linux

### 1. Levar os commits

Os 10 commits locais (mais este) só estão no Windows enquanto ninguém publicar. Opções:
- o dono autoriza o push de `feat/m3` (ver [Publicação](#5-publicar-e-abrir-o-pr)) e a máquina nova clona do
  GitHub;
- ou, sem push, um bundle: no Windows, `git bundle create legacyrecomp.bundle --all`; no Linux,
  `git clone legacyrecomp.bundle legacy-launcher`, depois `git remote set-url origin
  https://github.com/KauanDev1/legacy-launcher.git`.

Não copie a pasta de trabalho do Windows: ela usa `core.autocrlf=true`, e os arquivos viriam com CRLF.

### 2. Clonar e configurar a identidade

```
git clone https://github.com/KauanDev1/legacy-launcher.git && cd legacy-launcher
git switch feat/m3
tools/dev/init-submodules.sh          # não use git submodule update --init --recursive
git config user.name  "Kauan Turcato Storto"
git config user.email "159664484+KauanDev1@users.noreply.github.com"
git config credential.https://github.com.username KauanDev1
```

A configuração local do repositório não viaja com o clone: refaça as três linhas `git config`. Só a conta
**KauanDev1** pode aparecer como autora, committer ou conta de push.

### 3. Ferramentas e compilação nativa (padrão daqui em diante)

No Arch (os nomes mudam em outras distros):

```
sudo pacman -S --needed clang cmake ninja sdl2 pkgconf glslang python \
     vulkan-icd-loader vulkan-validation-layers vulkan-tools spirv-tools vulkan-swrast
# mais o driver Vulkan da GPU: vulkan-radeon, vulkan-intel ou nvidia-utils
```

- A linha do `AGENTS.md` só lista o que compila. Para rodar e testar também são precisos o carregador Vulkan,
  um driver, a camada de validação (testes e conferência visual), `python` (`frame_stats`, `func_profile`; sem ele
  esses testes ficam `DISABLED`) e `spirv-tools` (testes do tradutor). `vulkan-swrast` é o lavapipe, o driver por
  software que os testes usaram até agora.
- A lista é a mesma da imagem do contêiner (`tools/dev/Dockerfile`).
- Requisitos: CPU x86-64 com AVX, Clang ≥ 18, CMake ≥ 3.25, 8 GiB livres.
- Regra para agentes: **instalar pacotes do sistema só com autorização do dono** (ver regras).

```
cmake --preset dev && cmake --build --preset dev            # Debug com ASan+UBSan
ctest --preset dev -L unit                                  # esperado: 763 testes, 1 SKIP antigo
cmake --preset release && cmake --build --preset release    # para rodar o jogo (o dev é lento com sanitizers)
```

**(palpite)** Os testes `[vulkan]` sempre rodaram no lavapipe. Numa GPU real eles podem dar pixels um pouco
diferentes. Se algum falhar só na máquina nova, rode-os forçando o lavapipe (`VK_DRIVER_FILES` ou
`VK_ICD_FILENAMES` apontando para o `lvp_icd.x86_64.json` em `/usr/share/vulkan/icd.d/`) antes de investigar.

### 4. Raiz de dados e ISO

- Raiz padrão: `~/.local/share/legacyrecomp`. Outra raiz: `--root <pasta>` ou `LEGACYRECOMP_ROOT`. Use um disco
  de verdade, nunca tmpfs. A instalação ocupa alguns GiB (`work/` com o código gerado e a compilação).
- A ISO é a XGD2 do Minecraft: Xbox 360 Edition com o TU1, dump do disco do próprio dono, que autorizou o uso em
  2026-10-18. No Windows ela ficava na raiz do repositório (`Minecraft - Xbox 360 Edition (World).iso`, coberta
  pelo `*.iso` do `.gitignore`). No Linux, guarde-a **fora do repositório** (por exemplo `~/jogos/`) ou em
  `private/`.
- **Nunca** commitar `*.iso`, `*.xex`, `*.xexp`, `private/`, `work/`, `ppc_*` gerados, caches de shaders,
  screenshots do jogo ou qualquer byte derivado do jogo.
- A primeira instalação recompila tudo: levou cerca de 22 min no contêiner (o caminho de raiz nova do teste de
  integração foi estimado em 30 min e nunca rodou).

```
build/release/bin/legacyrecomp doctor
build/release/bin/legacyrecomp install --iso ~/jogos/minecraft.iso
build/release/bin/legacyrecomp run                      # abre a janela (ver Tarefa 23 redefinida)
```

### 5. Testes de integração (ISO)

```
LEGACYRECOMP_ISO=~/jogos/minecraft.iso ctest --preset dev -L integration
# reaproveitando uma raiz já instalada (evita a instalação inteira em cada teste):
LEGACYRECOMP_ISO=~/jogos/minecraft.iso LEGACYRECOMP_TEST_ROOT=~/.local/share/legacyrecomp \
  ctest --preset dev -L integration
```

São 6 testes: `pipeline doctor..recompile`, `install, cached rerun`, `package and title update match game.toml`,
`the game drives the GPU for 60 swaps`, `the game's loaded shaders all translate` e `the game reaches its title
screen`. Este último exige `python3` e a camada de validação.

### 6. Contêiner Docker (alternativa)

`tools/dev/container.sh {shell,build,test,install --iso,run}` continua funcionando no Linux (`docs/development.md`):
volumes `lr-build` (fontes em `/work`) e `lr-data` (raiz em `/data`). Ele serve para reproduzir o ambiente dos
testes (lavapipe, Arch) e para o teste "máquina limpa" do AppImage na Tarefa 24.

A memória "build/test só pelo contêiner" do Windows **deixou de valer**. Os problemas do Docker Desktop
(travamentos; a saída era parar o Docker Desktop, rodar `wsl --shutdown` e reiniciar) também ficaram no Windows.
Os diagnósticos derivados do jogo que estavam no volume `lr-data` do Windows (`/data/scratch/sc-diag-*`,
`/data/scratch/probe-cache-15.11`, `/data/shot*.png`, os `.txt` do `--dump-shaders`) não precisam ser levados.

### 7. Registro de execução (ledger)

O registro do M3 está copiado em `docs/superpowers/ledgers/2026-10-19-legacyrecomp-m3/` (ver o `README.md` dali).
Para continuar o desenvolvimento orientado por subagentes, recrie a pasta de trabalho a partir dele:

```
mkdir -p .superpowers/sdd/2026-10-19-legacyrecomp-m3
cp docs/superpowers/ledgers/2026-10-19-legacyrecomp-m3/*.md .superpowers/sdd/2026-10-19-legacyrecomp-m3/
rm .superpowers/sdd/2026-10-19-legacyrecomp-m3/README.md
printf '%s\n' docs/superpowers/plans/2026-10-19-legacyrecomp-m3.md > .superpowers/sdd/2026-10-19-legacyrecomp-m3/plan-path
```

A pasta `.superpowers/sdd/` tem um `.gitignore` próprio (`*`). Ao fechar o M3, copie o `progress.md` final de
volta para `docs/superpowers/ledgers/` no commit de fechamento.

## O que falta para fechar o M3

Em ordem. Cada item diz os arquivos e quando está pronto.

### 1. Conferência visual no Linux nativo (Tarefa 23 redefinida)

A Tarefa 23 do plano (instalar uma distro no WSL e rodar pelo WSLg) foi **cancelada pelo dono em 2026-11-06**. O
agente tinha instalado um Ubuntu 24.04 no WSL; o dono recusou ("por isso o disco ta enchendo"), e tudo foi
removido (a distro, `D:\Jogos Teste\WSL`, as edições não commitadas e o `task-23-brief.md`). Com a máquina Linux,
a conferência vira simplesmente rodar o jogo numa janela.

**Por que importa:** janela, swapchain, redimensionamento e apresentação **nunca rodaram**. O contêiner não tem
tela, então esse código só foi compilado. É a maior área sem teste do M3. A Tarefa 17 apontou também que
`vkQueuePresentKHR` em FIFO pode bloquear em alguns compositores com a janela escondida (o acquire tem limite de
1 s, o present não).

Comandos (build `release`, raiz instalada):

```
build/release/bin/legacyrecomp run                                 # janela, GPU real; fechar a janela sai com 0
LR_VULKAN_VALIDATION=1 build/release/bin/legacyrecomp run -- --trace-gpu
#   stderr: nenhuma linha [vulkan]; <raiz>/work/logs/gpu-trace.log: uma linha "stats draws=… swaps=…" por segundo
build/release/bin/legacyrecomp run -- --gpu vulkan --screenshot-after 600 /tmp/lr-shot.png
python3 -I tools/frame_stats.py /tmp/lr-shot.png                   # números da GPU real, comparar com o lavapipe
build/release/bin/legacyrecomp run -- --headless --exit-after-swaps 600   # caminho sem janela na GPU real
```

O que conferir (o dono olha a janela; um agente nunca abre a imagem):
- a janela abre e mostra a abertura e a tela de título: logo, botões, textos com a fonte do jogo, panorama;
- redimensionar, minimizar e restaurar a janela não derruba o jogo (recriação do swapchain);
- fechar a janela e Ctrl+C saem com código 0 em até 5 s;
- quadros por segundo: contar `swaps=` por segundo nas linhas `stats` do `gpu-trace.log`. A meta é 60 Hz, mas
  isso é medido e registrado, não é critério de aprovação. Com janela, o vblank segue o relógio real;
- `frame_stats` na GPU real passa dos mesmos limites (≥ 64 cores, luminância 0,05..0,95, bordas ≥ 2%);
- `param_shade` (ADR 0005 M15): procurar geometria que deveria ser de sombreamento flat e sai suave;
- ordem S/T do cubo e ordem das metades da cor de limpeza de 64 bpp, se aparecerem.

Pronto quando: o dono confirmou o que viu, os itens acima estão registrados em `docs/m3-status.md` (sem imagem
nenhuma no repositório) e o que quebrar vira correção com teste. Commit sugerido: `docs: record the M3 visual check
on native Linux`. O plano (`docs/superpowers/plans/2026-10-19-legacyrecomp-m3.md`, Tarefa 23) ainda descreve o
WSLg: ao fazer a conferência, registre a redefinição no ADR 0005 (entrada nova, M17) e ajuste o texto da tarefa.

### 2. Os quatro detalhes de documentação da revisão final da Tarefa 22

A última re-revisão aprovou tudo com 4 detalhes. Eles foram aplicados junto da Tarefa 23 e **revertidos** com ela;
três continuam pendentes:
- `docs/reverse-engineering/addresses.md`, seção "Tabela logo após o `bctr`": uma frase sobre o resíduo dos desvios
  perdidos *para trás*. Um byte da tabela que se decodifica como `b`/`bc` para trás, caindo dentro da tabela, ainda
  pode encurtá-la. Isso é raro: as entradas teriam de parecer `0x4A`/`0x4B` seguido de `0xFF 0xFF`, ou um `bc` com
  `0xFF 0xF?` na segunda metade. O custo é uma tabela truncada, cujos índices finais caem em
  `__builtin_unreachable`;
- `tests/test_integration_m3.cpp`, linha 182: o comentário do watchdog cita as medidas "130-300 s"; deve citar
  também os 390 s da última rodada (o watchdog é de 1200 s);
- `src/pipeline/function_discovery.h`: quebrar as linhas de comentário com mais de 120 colunas (linha 36, 141
  colunas).

O quarto, o "(esta)" na linha da Tarefa 22 do `docs/m3-status.md`, foi resolvido neste commit (agora diz
`75d8ee9`..`6245bd6`). Commit sugerido para os três: `docs: tidy Task 22 notes`.

### 3. Tarefa 24: AppImage, licenças, documentação, fechamento

Texto da tarefa: `docs/superpowers/plans/2026-10-19-legacyrecomp-m3.md`, Tarefa 24. Arquivos e critérios:

- `packaging/appimage/build.sh`:
  - versão `0.3.0-m3` (hoje o padrão do script é `0.1.0-m1`; `LR_VERSION` sobrescreve);
  - o runtime preparado agora leva o tradutor, os cabeçalhos do SPIRV-Headers e a `liblr_vulkan.a`, tudo compilado
    pelo zig para glibc 2.31;
  - registrar em `docs/packaging.md` o piso de glibc do binário do jogo (`readelf -V`).
- `packaging/appimage/licenses.sh`: as linhas de volk, VMA, Vulkan-Headers e SPIRV-Headers já existem; conferir.
  As de DXC, XenosRecomp, zstd e smol-v saíram na 15.1.
- **Herança a resolver:**
  - a `liblr_vulkan.a` preparada (com o `volk.c` compilado pelo zig cc) e o `legacyrecomp-vkprobe` ligado ao SDL2
    estático **nunca foram compilados com o zig/glibc 2.31** (Tarefa 17);
  - `--gpu test-stall` é só para testes e deve ficar fora da ajuda do usuário. A ajuda do jogo já não o lista e o
    erro diz "vulkan or null"; conferir READMEs e docs no passe de documentação;
  - o `build.sh` já confere o `glslangValidator` e registra a versão (Tarefa 16);
  - ficaram sem efeito com o plano B (ADR 0005 M8) e devem ser fechadas no ADR: o DXC embutido (layout, rpath,
    obrigatório no `doctor`), o XenosRecomp no AppImage (`-DLR_BUILD_XENOSRECOMP=OFF`), o XenosRecomp com
    zig/libc++ (`std::execution`) e o rpath da `libdxcompiler.so`. O critério 4 do spec ("com o DXC embutido") não
    vale mais.
- **Passo 1 (pronto quando):** o AppImage é gerado. Num contêiner `archlinux` limpo, sem compiladores no PATH,
  `install --iso` mais `run -- --headless --screenshot-after 600 …` passam nos limites da Tarefa 22 com o lavapipe,
  e `work/shaders/runtime/` fica preenchida. Isso precisa de Docker (ou uma VM) mesmo no Linux.
- **Passo 2, documentação:**
  - `docs/architecture.md`: GPU, EDRAM, cache de texturas, tradutor e contrato;
  - `docs/pipeline.md`: as sete etapas;
  - `docs/packaging.md`: nenhum compilador de shader; o carregador Vulkan vem do sistema;
  - `docs/roadmap.md`: M3 feito, M4 em seguida, as lacunas que ficam;
  - os sete READMEs: o estado honesto (tela de título desenhada, sem entrada nem som);
  - ADR 0005 completo, com todos os rulings;
  - `docs/kernel-coverage.md` regenerado do build real (comando no cabeçalho dele);
  - status do spec do M3.
  - Aproveitar para completar a linha de pacotes do `AGENTS.md` com o Vulkan e o `python` (ver acima).
- **Passo 3:** a suíte de unidade inteira e os testes de integração verdes.
- **Passo 4:** commit `docs: close M3`, com o `progress.md` final copiado para `docs/superpowers/ledgers/`.

### 4. Revisão final da branch inteira

Pela skill `superpowers:subagent-driven-development`, depois da última tarefa: um revisor (esforço alto) sobre o M3
inteiro, base `24d4e82` (início do M3) até o HEAD, com atenção maior ao que ainda não está em `main` (depois de
`e004840`). As correções entram como rodadas, como nas tarefas.

### 5. Publicar e abrir o PR

**Sempre perguntar ao dono antes de qualquer push.** Antes de publicar:
- `git status --short` sem nenhum arquivo do jogo;
- `git log --format='%an <%ae> | %cn <%ce>' origin/feat/m3..HEAD | sort -u` mostra só
  `Kauan Turcato Storto <159664484+KauanDev1@users.noreply.github.com>`;
- `git log --name-only origin/feat/m3..HEAD` sem `*.iso`, `*.xex`, `*.xexp`, `*.png` do jogo, `work/` ou `ppc_*`.

Depois: `git push origin feat/m3` e um PR de `feat/m3` para `main` (`gh pr create`). O corpo termina com
`🤖 Generated with [Claude Code](https://claude.com/claude-code)`. O dono faz o merge. É uma branch só: nada de
`feat/m2` ou de branches separadas.

## Itens estacionados e riscos conhecidos

Nenhum destes bloqueia o M3 hoje. Cada um diz o que fazer quando aparecer.

**Estacionados com conhecimento de causa (fato):**
- **Corridas da vigia de memória** (`src/runtime/write_watch.*`): (1) outra thread pode escrever na página enquanto
  uma thread faz o passo único, e essa escrita não é contada (a página é protegida de novo no meio do passo); (2)
  o relatório com `:rw` é montado sob o spin lock. É uma ferramenta de diagnóstico; até agora não atrapalhou.
- **`MmQueryAllocationSize`/`MmQueryAddressProtect` por uma vista que não é a da alocação** respondem 0 (ADR 0005
  M14). Risco baixo.
- **`tools/func_profile.py` conta `dcbz128` como `dcbz`.**
- **Teste instável**: `a pending APC does not leak into a thread that reuses the KTHREAD address` falhou uma vez
  com a máquina carregada. A pré-condição `REQUIRE(kthread == a_kthread)` depende de quando a thread do host libera
  o KTHREAD de A. Passou ao repetir.
- **Significado de `param_shade` não confirmado** (ADR 0005 M15): hoje nada é `Flat`. Se for de fato a máscara
  flat junto com outro bit de estado, um desenho que pede sombreamento flat sai suave. A conferência visual é o
  primeiro lugar para notar.
- **Caminho de raiz nova do teste da tela de título nunca rodou** (sem `LEGACYRECOMP_TEST_ROOT`: instalação inteira,
  cerca de 30 min, antes das mesmas conferências).
- **14 sítios `bctr` sem guarda** continuam sem tabela (`docs/reverse-engineering/addresses.md`). Se o jogo chegar a
  um deles, sai com 4 e o relatório "indirect call to 0x… without a recompiled function".
- **Thunks de ordinal sem nome** não viram símbolo no XenonUtils e continuam fora da lista de thunks que a
  descoberta de funções evita (ADR 0005 M16). Uma duplicata ali vira só aviso.
- **7 ordinais da XAM pedidos por `XexGetProcedureAddress` falham** (`0x305`, `0x30B`, `0x48C`, `0xAFF`, `0xB00`,
  `0xB0B`, `0xB10` → `0xC0000263`). Também falham `XamUserGetSigninInfo` (`0x80070525`) e `XMsgInProcessCall`
  (`0x80070490`) em algumas chamadas. Sem efeito visível até agora.

**Do plano, ainda não apareceram até a tela de título (fato: não dispararam):**
- shader com memexport (`eA`/`eM`, registradores de exportação 32..37). 15 contêineres de pixel shader do XEX usam;
  se o jogo carregar um, é decisão de plano (regra de parada), não correção;
- ordem S/T do cubo (alimentação `.yxw`), formatos de render target não suportados (`k_16_16`, `k_16_16_16_16`,
  7e3; sugestão: 7e3 → `R16G16B16A16_SFLOAT`, `k_16_16` → `R16G16_SNORM` ×32), texturas com sinal (hoje fatal com
  nome);
- ordem das metades da cor de limpeza de 64 bpp; `RB_COLOR_CLEAR` não foi fixado por teste;
- consultas de oclusão (`EVENT_WRITE_ZPD`, marcadores `0xFFFFFEED`): se travarem, saída 6;
- qual registrador de status MMIO o jogo consulta (`RBBM_STATUS` `0x5D0` ou `0x174`);
- ritmo do vblank sem janela (`PacedBySwap` pode passar de 60 Hz com o `NullBackend`);
- escape de caracteres de controle na caixa de mensagem e testes com argumentos nulos;
- guarda RAII para restaurar a CPU atual do KPCR;
- a cópia da imagem inteira no loader supõe que `image.data` tem `image.size` (compressão BASIC);
- reinício de primitiva numa lista é fatal (o Xenia ignora): se disparar, trocar para "ignorar";
- denormais tratados como zero em `RECIP`/`RSQ`/`LOG` (suposição de FTZ do Xenos, não conferida);
- desenhos com pitch 0 são pulados com aviso; scissor ou viewport vazios avisam.

**Limites registrados do M3 (ADR 0005 M10 a M13):** envio esperado a cada resolve e swap, sem quadros em voo (lento,
mas seguro); destile e hash de texturas na CPU; listas de retângulos exigem `geometryShader`; `PA_SC_AA_MASK`,
`alpha_to_mask` e os filtros por instrução do `tfetch` ignorados; sem `depthClamp` com `clip_disable`; sem expansão
de point sprites; sem o caminho preciso de EDRAM por interlock. Cobertura de pixels sem teste: MSAA e MRT, estêncil,
depth bias, provoking vertex, D24FS8.

**Decisões do M2 nunca conferidas no jogo (ADR 0004):** argumentos de 64 bits e floats na pilha (M10), layout do
`CONTEXT` em `RtlCaptureContext` (M16), layout do LDI do `XMemDecompress`, `XGetAVPack` = 6 e `XamUserGetXUID` com
máscara 4 (M14), `NtOpenFile` com 6 argumentos (M12), semântica MSVC do printf (M13). Endereços dos helpers de
prólogo/epílogo e 23 jump tables ainda sem confirmação no Ghidra.

## M4 e M5

Nenhum dos dois tem spec ou plano. O próximo passo depois do M3 é escrever o spec do M4 a partir deste arquivo e do
roadmap, sem rodada de perguntas, com `superpowers:brainstorming` e `superpowers:writing-plans`.

### Direção (fato, dos specs)

- Roteiro desde o spec do M1 (seção 13): M2 kernel → M3 GPU → **M4 áudio, entrada e saves** → **M5 jogável (menu e
  mundo)**. A meta final é o jogo completo jogável no Linux.
- O spec do M3 (seção 13) deixa para o M4 "som de verdade, controle, saves" e fora de escopo "rede e LIVE; interface
  do sistema; escala de resolução, filtros e melhorias visuais; caminho preciso de EDRAM por interlock".
- A classificação das importações (`docs/reverse-engineering/kernel-imports.md`, spec do M2 §5.1) põe cerca de 45
  importações no M4 (XAudio/XMA, entrada, conteúdo e saves) e cerca de 194 em "M5 ou nunca" (UI do sistema,
  avatares, voz, Kinect, rede).

### Importações do kernel hoje (fato, `docs/kernel-coverage.md`)

Funções: 176 `implemented`, 58 `stub`, 225 `missing` de 459. Variáveis: 15 `implemented` de 15. A tabela foi
regenerada na Tarefa 12 e atualizada à mão na Tarefa 22 (`XMA*`, download em segundo plano); a Tarefa 24 a
regenera do build. Por área:

| Área | implemented | stub | missing | Observação |
|---|---|---|---|---|
| `Ke*` (threads, sincronização, DPC, FPU) | 30 | 6 | 0 | |
| `Ex*` | 7 | 0 | 0 | |
| `Mm*` | 8 | 0 | 0 | |
| `Nt*`/`Io*`/`Ob*` (arquivos, objetos) | 38 | 3 | 2 | faltam `NtWriteFileGather`, `NtFlushBuffersFile` (M4) |
| `Rtl*` | 21 | 0 | 0 | |
| `Xex*` | 6 | 0 | 1 | falta `XexUnloadImageAndExitThread` (M5) |
| `XeCrypt*`/`XeKeys*` | 8 | 0 | 4 | faltam `XeKeysGetKey`, `XeKeysGetConsoleID`, `XeCryptBnQwBeSigVerify` (M4) e `XeCryptRotSumSha` |
| `Vd*` (vídeo) | 5 | 18 | 0 | o M3 deixou as que não importam como stub |
| `XAudio*`/`XMA*` | 0 | 7 | 6 | stubs silenciosos; faltam as 6 do ducker |
| `Xam*` | 31 | 10 | 95 | ver abaixo |
| `NetDll_*` (Winsock e XNet) | 0 | 5 | 114 | só a inicialização, offline |
| outras (`Dbg*`, `Etx*`, `Fsc*`, `Hal*`, `LDI*`, `XGet*`, `XMsg*`, `XNotify*`, printf da CRT, `PsCam*`) | 22 | 9 | 3 | |

As 95 `Xam*` que faltam: 23 `XamShow*UI`, 13 de avatar, 13 de Kinect/NUI/biometria, 12 `XamContent*`, 11 de
usuário/perfil/gamer tile, 11 de LIVE (tokens, XAuth, Xlfs, Live hive, XStudio), 3 de voz, 3 `XamCache*` e 6
outras (`XamGetActiveDashAppInfo`, `XamGetAppliesTo`, `XamLoaderSetLaunchData`, `XamReadString`,
`XamSessionCreateHandle`, `XamSessionRefObjByHandle`). Uma importação `missing` chamada pelo jogo sai com 3 e o
nome: o laço "rodar, ver onde para, implementar" do M2 continua valendo.

### Respostas provisórias de hardware a revisitar no M4 (fato, Tarefa 12)

Escolhidas para o jogo passar da inicialização sem nada do hardware:
- **Armazenamento:** nenhum dispositivo. `XamContentGetDeviceState` e `XamContentGetDeviceData` →
  `ERROR_DEVICE_NOT_CONNECTED`; enumeração de dispositivos vazia; `XamContentCreateEnumerator` devolve um
  enumerador vazio e **ignora `device_id`**. Sem saves.
- **Controle:** nenhum. `XamInputGetCapabilities`/`GetState`/`SetState`/`GetKeystrokeEx` →
  `ERROR_DEVICE_NOT_CONNECTED`. Sem controle o jogo fica no laço do "press start", e é isso que deixa os swaps
  correrem nos testes.
- **Kinect:** `XamNuiGetDeviceStatus` responde "sem Kinect".
- **Voz:** `XamVoiceIsActiveProcess` 0, `XamVoiceHeadsetPresent` 0.
- **Rede:** offline. `NetDll_WSAStartup` 0, `NetDll_XNetStartup` 0, `XNetGetEthernetLinkStatus` 0 (sem link),
  `XNetGetTitleXnAddr`/`GetDebugXnAddr` com o endereço zerado (`XNET_GET_XNADDR_NONE`),
  `XamBackgroundDownloadSetMode` aceita qualquer modo e `GetMode` responde 0.
- **Privilégios:** `XamUserCheckPrivilege` concede tudo ao usuário 0 (e a `0xFF`).
- **Usuário:** um perfil local "Player", XUID offline `0xE000000000000001`, conectado localmente, só no índice 0;
  `XamUserCreateAchievementEnumerator` vazio; `XamUserReadProfileSettings` sem perfil gravado (padrões do Xenia).
- **Vídeo:** 1280x720, widescreen, 60 Hz, `XGetAVPack` 6.

**(palpite)** Quando um controle "aparecer", o jogo sai do "press start" e entra em caminhos que nunca rodaram:
seletor de dispositivo, perfil, saves, e provavelmente o mundo. Espere uma nova leva de importações `missing` e de
falhas do recompilador (os 14 `bctr` sem guarda).

### Áudio (fato do código e do catálogo; o desenho é palpite)

- Hoje (`src/runtime/kernel/exports/audio.cpp`): `XAudioRegisterRenderDriverClient` escreve o handle `0x41550000` e
  **nunca chama o callback**; `XAudioSubmitRenderDriverFrame` descarta o quadro; `XAudioGetVoiceCategoryVolume`
  escreve `1.0f`; `XAudioGetSpeakerConfig` responde estéreo (`0x00010001`).
- **Contextos XMA:** `XMACreateContext` entrega um bloco zerado de 64 bytes numa página física de 4 KiB só dele,
  que nenhum decodificador lê; o kernel guarda os contextos num `std::set` protegido por mutex
  (`add_xma_context`/`take_xma_context` em `kernel.h`), e `XMAReleaseContext` libera só um contexto que entregou e
  ainda não liberou (ignora segunda liberação e base de outra alocação). O jogo cria esses contextos antes da tela
  de título.
- O que o catálogo prevê para o M4: um thread de áudio do host chama o callback a cada quadro de 256 amostras;
  `XAudioSubmitRenderDriverFrame` entrega 6 canais × 256 floats big-endian; um decodificador XMA2 de verdade (o
  Xenia usa o ffmpeg). Faltam ainda as 6 importações do ducker.
- **(palpite)** A licença do decodificador XMA é decisão de spec (o ffmpeg é LGPL; o ADR 0002 teria de tratar). Os
  dados de áudio do jogo são bancos XACT soltos no disco.
- **(palpite)** O detector de impasse precisa contar a futura thread de áudio do host como trabalho pendente, senão
  uma espera por ela vira falso impasse (saída 6), como `docs/architecture.md` avisa para todo sinalizador novo do
  host.

### Entrada (palpite sobre o desenho; fato sobre o catálogo)

Catálogo: gamepad do SDL2 → `XINPUT_STATE`/`XINPUT_CAPABILITIES` em big-endian, vibração por `XamInputSetState`,
`XamInputGetKeystrokeEx` para chat e teclado virtual. `XamShowKeyboardUI` (nomes de mundos) é M4. A janela SDL já
existe no `main.cpp`. Teclado e mouse mapeados para o controle são uma decisão de spec ainda não tomada.

### Saves (fato sobre o catálogo)

- Pacotes de conteúdo como pastas do host sob a raiz de dados: create/open/close por root name (link simbólico),
  enumerar, apagar, miniaturas, criador = XUID local. Faltam 10 `XamContent*` do M4 (`Close`, `CreateEx`,
  `Delete`, `Flush`, `GetCreator`, `GetDeviceName`, `GetThumbnail`, `OpenFile`, `Resolve`, `SetThumbnail`), os 3
  `XamCache*` (stub de falha, sem partição de cache) e `NtWriteFileGather`/`NtFlushBuffersFile`.
- `XamShowDeviceSelectorUI` escolhe o único dispositivo de armazenamento do host (necessário para saves);
  `XamShowSigninUI` e `XamShowMessageBoxUIEx` também são M4. `XamShowMessageBoxUI` já existe (imprime no
  terminal e responde "primeiro botão").
- Perfil gravável: `XamUserWriteProfileSettings`, `XamProfileCreateEnumerator`, `XamProfileEnumerate`.
- Assinaturas: `XeCryptBnQwBeSigVerify` = TRUE (pula a verificação RSA), `XeKeysGetKey`/`XeKeysGetConsoleID` com
  valores falsos fixos. **(palpite)** `XeCryptRotSumSha`, ainda `missing` (ADR 0004 M15), deve aparecer na
  assinatura de saves e perfis.

### SEH (fato)

Não há despacho de exceções do convidado. `RtlUnwind` e `__C_specific_handler` terminam com `guest_fatal` (saída 5),
como qualquer exceção sem tratamento; `RtlRaiseException` só deixa passar `0x406D1388` (`SetThreadName`). Fica para
quando um trace mostrar o jogo dependendo disso.

### M5 (palpites, salvo indicação)

- **Shaders do mundo:** os 600 primeiros swaps só carregam os shaders das telas iniciais. Os 15 contêineres de
  pixel shader com memexport (fato: ADR 0005 M7) devem aparecer no mundo, e o tradutor não faz memexport (falha
  com saída 5 nomeando o shader). Será uma tarefa nova do tradutor.
- **Desempenho:** 60 Hz numa GPU real ainda não foi medido. Os pontos caros já conhecidos (fato): envio esperado a
  cada resolve e swap (ADR 0005 M12), destile e hash de texturas na CPU (M11), cópia dos intervalos de vértices em
  cada desenho, tradução de shaders no primeiro uso (o cache em disco só ajuda da segunda execução em diante).
- **Rede:** o catálogo prevê System Link (LAN) mapeado para sockets do host como "M5 real"; XHttp, XNet seguro e LIVE
  provavelmente nunca. 191 dos 459 nomes vêm de bibliotecas do XDK ligadas estaticamente e talvez nunca rodem numa
  sessão offline.
- **UI do sistema, avatares, voz, Kinect:** stubs de falha conforme forem chamados. `XamLoaderLaunchTitle` hoje
  encerra com saída 5 ("the game asked to relaunch itself"); carregar outro XEX do disco (`XexLoadImage`) é M5.
- **Fora de escopo até agora:** escala de resolução, filtros e melhorias visuais; EDRAM por interlock; Windows e
  macOS nativos.

## Processo de trabalho e regras do dono

- **Desenvolvimento orientado por subagentes** (`superpowers:subagent-driven-development`). O controlador não
  implementa: para cada tarefa do plano, despacha um implementador com um brief, depois um revisor (spec e
  qualidade), e até 5 rodadas de correção. Cada passo vira uma linha no registro (`progress.md`), com rulings
  ("Ruling: … — cost if wrong: …"), itens carregados para tarefas seguintes ("Carry to Task N") e itens
  estacionados ("Parked"). Briefs e relatórios ficam na pasta do registro, que o git ignora.
- **Todo subagente com `effort: "high"`** (implementadores, revisores, planejadores), pedido do dono em 2026-10-24.
  Um agente em execução não muda de esforço: só é parado e despachado de novo se o dono pedir.
- **Sem rodada de perguntas.** Specs e planos são escritos direto da documentação, com as decisões marcadas como do
  agente. Pergunte só o que só o dono pode responder (questões legais, a ISO, ações na conta do GitHub).
- **Mensagens curtas, em português.** Código, identificadores e commits em inglês; prosa de `docs/` em português.
- **Identidade única:** commits só como `Kauan Turcato Storto <159664484+KauanDev1@users.noreply.github.com>`. A
  conta **KauanCentral** (id 274010019) nunca pode ser autora nem conta de push. A barra "Contributors" do GitHub
  mostrou KauanCentral por cache; nenhum commit é dela.
- **Perguntar antes de cada push.** Uma branch só, consolidada.
- **Nada de instalar software do sistema, distros no WSL ou downloads grandes sem perguntar**, mesmo que um spec diga
  que estava pré-aprovado; mencionar o custo em disco. (O disco do Windows estava quase cheio; vale como regra na
  máquina nova também.)
- **Dados do jogo:** nunca no contexto de um agente nem no repositório. Um agente lê só relatórios de saída,
  contagens e trechos curtos de log; screenshots são julgados por `frame_stats`, nunca abertos. Hashes de shader,
  endereços e contagens podem ser escritos. Exceções só com o dono, uma de cada vez (a única dada, para o VS
  `E91536F60A69ABF3`, já foi usada).
- **Licença:** MIT, cabeçalho SPDX em todo arquivo novo. UnleashedRecomp (GPL-3.0) nunca tem código copiado e não é
  lido para o tradutor; Xenia e MarathonRecomp são só referência (ler, entender e escrever do zero).
- **Documentação no mesmo commit do código.** Pedido do dono em 2026-10-28: guardar todo o contexto nos `.md`;
  `docs/m3-status.md` (ou o arquivo de estado do marco atual) muda no mesmo commit de cada achado ou correção, e
  este `docs/handoff.md` é o ponto de entrada.
- **Commits:** Conventional Commits, um por passo marcado "Commit" no plano, cada um passando
  `ctest --preset dev -L unit`, terminando com `Co-Authored-By: Claude <modelo> <noreply@anthropic.com>` (o modelo
  que escreveu). Antes de cada commit, `git status --short` sem arquivos do jogo.
- **Commitar cada correção assim que os testes passam**, antes de qualquer rodada longa com a ISO: sessões e o
  Docker caíram várias vezes, e o limite mensal de gastos da organização já parou o trabalho por dois dias
  (2026-10-25 a 2026-10-27).
- Ferramentas externas sempre por `posix_spawn` com vetor de argumentos, nunca por shell.

## Decisões e rulings importantes

Para não reabrir. Cada linha: a decisão e o porquê. Os detalhes estão no ADR 0005 e no registro.

**Arquitetura do M3**
- **Emular a GPU, não interceptar o D3D** (spec §3.1): começa onde o M2 parou (importações com nome), não depende
  de endereços do jogo e cobre qualquer caminho de desenho.
- **Plano B: tradutor próprio em tempo de execução** (M7, M8): o portão do plano A mediu 64 de 85 contêineres
  traduzidos e 5 de 7 microcódigos carregados fora de qualquer contêiner (variantes alteradas pelo D3D e shaders
  sem contêiner). Nenhum cache feito na instalação cobre isso; traduzir o que foi carregado cobre por construção.
- **SPIR-V por um construtor próprio sobre o SPIRV-Headers**: sem biblioteca nova no jogo, sem front-end de GLSL,
  sem licenças mistas. Saída de emergência: trocar o backend do `SpirvBuilder` pelo `spv::Builder` do glslang.
- **Vertex fetch dentro do shader**, por endereço de dispositivo: variantes do D3D viram chaves diferentes, e
  formatos sem `VkFormat` funcionam.
- **Interpoladores ligados por índice de registrador**: o VS declara 16 saídas `vec4` (64 componentes, o mínimo do
  Vulkan).
- **Endereços de dispositivo lidos como `uvec2` com `OpBitcast`**, sem exigir `shaderInt64`.
- **`kill` liga uma flag; `OpKill` só no fim**, para preservar derivadas; fetches com predicado são avaliados sempre
  e escolhidos com `OpSelect`.
- **DXC fora do projeto**: os releases oficiais pedem glibc 2.38 (o AppImage mira 2.31). Os shaders do próprio
  renderizador são GLSL compilado pelo `glslangValidator` na compilação do launcher.
- **Toda mudança na saída do tradutor sobe `kTranslatorVersion`** (o teste de impressão digital obriga).

**Semântica do tradutor**
- Limite de 2^24 desvios para trás por invocação: 3 laços aninhados de 255 cabem; um caso real maior não rodaria em
  tempo de quadro.
- `return` com profundidade de chamada 0 não faz nada (nota de hardware do `ucode.h` do Xenia).
- O `aL` é restaurado ao sair de um laço por salto (mais seguro com código malformado).
- Casos extremos do ALU escalar decididos pela entrada; denormais contam como zero (suposição de FTZ).
- Componentes de vértice ausentes saem (0,0,0,1) (o Xenia usa 0).
- Máscara de centróide corrigida pelo Xenia na revisão da 15.10 (estava invertida).
- Regra de multiplicação do Xenos: produto com fator 0 dá +0 (mesmo 0 × inf), exceto com os dois operandos iguais.

**Renderizador**
- `--gpu vulkan` é o padrão, também com `--headless`; `--gpu null` volta à GPU sem desenho.
- Vulkan ausente é **aviso** no `doctor` (a instalação funciona sem GPU); o jogo é que sai com 2.
- SIGTERM, SIGINT e SIGHUP saem com 0 de forma ordenada, com limite de 5 s; um segundo sinal encerra na hora.
- `--gpu test-stall` fica no binário, só para testes e fora da ajuda.
- Um command buffer por quadro, enviado e esperado nos pontos de sincronização (correção antes de desempenho, M12).
- Destile e hash de texturas na CPU (M11); retângulos por geometry shader (M13).
- Referência de tiling na CPU em `tests/support/xenos_tiling.h` (Tarefa 18), reaproveitada pela 19.

**Recompilador e runtime**
- Alvos de `bl` são só fronteiras na descoberta por ponteiro (M1): o XenonRecomp já cria funções neles.
- `0x8294E434` tem 15 casos, não 17 (o 17 do experimento era estimativa).
- Tabelas sem limite ficam na lista "unbounded", não "unresolved".
- Tabela inline após o `bctr`: só desvios perdidos *para frente* são filtrados (os para trás não se distinguem de
  um laço de verdade); o limite "antes" é recusado quando a faixa tem duas funções.
- Duplicata na tabela de funções: a primeira vale (os thunks vêm antes), agora com aviso (M16).
- Espelhamento das vistas físicas por `memfd` (M14): era a contingência prevista no M2.
- `param_shade` não liga `Flat` (M15); sem aumento de `kTranslatorVersion` (só a chave mudou, não a saída).
- O teste da tela de título faz SKIP sem a camada de validação, em vez de falhar.

**Processo e dono**
- Ruling R-stage: todo `.cpp` novo do runtime entra nas duas variantes do runtime, e todo cabeçalho que o template
  inclui é preparado (o teste de cabeçalhos preparados guarda isso).
- Ruling R-gate: portão reprovado → parar e voltar ao dono antes de seguir.
- A ISO na raiz era dump do dono, uso autorizado (2026-10-18).
- Esforço alto em todo subagente (2026-10-24); documentação viva (2026-10-28); uma branch só, só KauanDev1
  (2026-10-28); Tarefa 23 cancelada e nada de instalações sem perguntar (2026-11-06).

## Ferramentas de diagnóstico

Flags do jogo vão depois de `--` em `legacyrecomp run -- …`. O launcher sempre passa `--trace-file`,
`--gpu-trace-file` (`<raiz>/work/logs/game-trace.log` e `gpu-trace.log`) e `--shader-cache-dir`
(`<raiz>/work/shaders/runtime`).

| Ferramenta | Uso em uma linha |
|---|---|
| `tools/frame_stats.py` | `python3 -I tools/frame_stats.py <png>` → `colors=N mean_luminance=F edge_fraction=F`, sem abrir a imagem |
| `--screenshot-after <n> <png>` | grava o quadro do swap n e sai com 0 (só `--gpu vulkan`) |
| `--exit-after-swaps <n>` | sai com 0 depois de n swaps (testes) |
| `--headless` | sem janela; a GPU processa e resolve normalmente |
| `--gpu vulkan\|null` | backend Vulkan (padrão) ou nenhum desenho |
| `LR_VULKAN_VALIDATION=1` | liga a camada de validação; toda mensagem sai como `[vulkan] …` |
| `--trace-gpu` | `gpu-trace.log`: cada pacote PM4 e desenho, `shader load … at=0x…`, uma linha `stats draws= resolves= textures= swaps=` por segundo (limite de 64 MiB) |
| `--trace-imports` | `game-trace.log`: uma linha por chamada de importação (`r3..r6 -> r3`) |
| `--watch-guest <end>:<tam>[:rw]` | vigia escritas (e leituras com `:rw`) numa faixa do convidado; até 16 faixas; arme uma por vista física (`0xA0000000+p`, `0xC0000000+p`, `0xE0000000+p-0x1000`) |
| `--dump-shaders` | ao lado de cada `.spv` do cache: `<nome>.txt` (desmontagem do microcódigo; é dado do jogo, nunca ler nem commitar) e `<nome>.raw.spv` |
| resumo `[gpu] shaders:` | na saída: `keys vs ps translated disk failed new_last_100_swaps` |
| `tools/shader_cache_stats.sh [pasta]` | `spirv-val` em cada arquivo do cache; imprime só contagens |
| `tools/func_profile.py` | `python3 -I tools/func_profile.py <image.bin> <início> [fim] --mapping work/generated/ppc_func_mapping.cpp`: perfil por classe de instrução e alvos de `bl`, sem código |
| `tools/site_window.py` | decodifica algumas instruções em volta de um endereço (jump tables, fronteiras), pela skill `xbox360-re` |
| `tools/find_helpers.py`, `tools/vpkd3d_sites.py`, `tools/recomp_warnings.py` | helpers de prólogo/epílogo, sítios `vpkd3d128`, resumo dos avisos do XenonRecomp |
| `legacyrecomp inspect --dump-image` | gera o `image.bin` carregado que as ferramentas acima leem (skill `xbox360-re`) |
| `--coverage --image <xex>` | tabela de cobertura das importações (`docs/kernel-coverage.md`) |
| `--strict-memory` | páginas não comprometidas do heap ficam `PROT_NONE`: um acesso a elas vira falha (saída 4) |
| `LR_DEADLOCK_SECONDS` | segundos do detector de impasse (padrão 10; 0 desliga) |
| `addr2line -f -e legacyrecomp-game <pc - base>` | nomeia um `pc=` do host fora do código gerado (a vigia imprime `[watch] exe base`) |

Os códigos de saída do jogo: 0 ok, 2 uso ou carga, 3 importação faltando, 4 falha, 5 fatal do convidado, 6 impasse.
O fluxo de engenharia reversa está na skill `.claude/skills/xbox360-re/SKILL.md`.
