<!-- SPDX-License-Identifier: MIT -->
# Desenvolvimento em container (Windows, macOS e Linux)

O LegacyRecomp só compila em Linux. Quem desenvolve no Windows ou no macOS usa o container de
desenvolvimento: `tools/dev/container.sh`. Em Linux o script também funciona, mas você pode seguir
os comandos do `AGENTS.md` direto na máquina.

## Requisitos

- Docker Desktop (backend WSL2 no Windows) ou um daemon Docker em execução.
- No Windows, rode o script pelo Git Bash (ele já trata `MSYS_NO_PATHCONV` e caminhos com espaço).
- Memória: o build usa até `memória do Docker / 2 GiB` jobs do Ninja. Com 4 GB o build roda com 2 jobs
  e é lento; recomenda-se **8 GB ou mais**. Para aumentar: Docker Desktop, Settings, Resources,
  Memory (no WSL2, também pode ser definido em `%UserProfile%\.wslconfig`).
- ~8 GiB livres em disco para os volumes.

## Comandos

```
tools/dev/container.sh shell                     # shell interativo em /work
tools/dev/container.sh build                     # sincroniza os fontes e compila o preset dev
tools/dev/container.sh test                      # compila e roda ctest --preset dev -L unit
tools/dev/container.sh test -R recomp            # argumentos extras vão para o ctest
tools/dev/container.sh install --iso /caminho/jogo.iso
tools/dev/container.sh run [-- argumentos do jogo]
```

`LR_PRESET=release tools/dev/container.sh build` usa o preset `release` no lugar de `dev`. `install` e
`run` compilam antes e chamam `build/<preset>/bin/legacyrecomp ... --root /data`.

## Onde ficam as coisas

- Imagem `legacyrecomp-dev` (Arch Linux com clang, cmake, ninja, SDL2, pkgconf, git, python, binutils, rsync e Vulkan: `mesa`, `vulkan-swrast` (lavapipe, o driver
  por software usado nos testes `[vulkan]`), `vulkan-icd-loader`, `vulkan-validation-layers` e `vulkan-tools`; mais `glslang` (o `glslangValidator`, que compila os shaders GLSL do próprio renderizador na compilação; ferramenta de build, nunca distribuída) e `spirv-tools`, que traz o `spirv-val` e o
  `spirv-dis` usados só nos testes do tradutor de shaders, por `tests/support/spirv_tools.h`, e nunca distribuídos). É reconstruída pelo Docker apenas quando o `Dockerfile` muda.
- O repositório é montado somente leitura em `/src`. Os arquivos **rastreados pelo git** (incluindo
  submódulos) são sincronizados com `rsync --delete` para o volume `lr-build`, montado em `/work`;
  também entram os arquivos novos ainda não rastreados (que não estejam no `.gitignore`); arquivos rastreados mas apagados na árvore de trabalho não são copiados e saem do volume. O build fica em `/work/build/<preset>` e sobrevive
  entre execuções (a sincronização não apaga `build/`).
- O container roda com `--init` (reaper de processos órfãos); sem isso dois testes de cancelamento falham.
- Os dados (cache de instalação, jogo extraído, executável gerado) ficam no volume `lr-data`, em `/data`.
- Para limpar tudo: `docker volume rm lr-build lr-data`.

## Submódulos

Para clonar, use `tools/dev/init-submodules.sh` em vez de `git submodule update --init --recursive`: ele inicializa
os submódulos e, só dentro do `thirdparty/XenonRecomp`, os aninhados que usamos. O `container.sh` sincroniza o que
está inicializado na árvore de trabalho.

Não há compilador para os shaders do jogo: o XenosRecomp, o DXC e a `lr_zstd` saíram no plano B (ADR 0005, M8), e o
runtime traduz o microcódigo para SPIR-V ele mesmo (Tarefas 15.2 em diante). O único compilador de shaders é o
`glslangValidator`, e só para o nosso GLSL, na compilação do launcher. Os shaders do renderizador (blit,
limpeza, resolve) são GLSL em `src/runtime/gpu/vk/shaders/`, compilado pelo `glslangValidator` do sistema
(`-V --target-env vulkan1.2`) na compilação do launcher (`cmake/LrGpuShaders.cmake`, função
`lr_compile_gpu_shaders`) e embutido na `liblr_vulkan.a` como vetores `uint32_t` (`lr_gpu_shaders.h`/`.cpp`, gerados
na árvore de build e nunca versionados; `kBuiltinShaders` lista todos). O `cmake` falha com a dica de instalação
(`sudo pacman -S --needed glslang`) se a ferramenta faltar. O destile das texturas é feito na
CPU (Tarefa 19, `runtime/gpu/texture_format.cpp`), então não há shader de destile. Os `resolve_*.comp` da EDRAM (Tarefa 18)
compartilham `resolve_common.glsl` por `#include` (`GL_GOOGLE_include_directive`); todo `*.glsl` da pasta é
dependência deles no build. O histórico do git guarda o fork do XenosRecomp (commits `0523eda`..`3b15c74`).

Vulkan nos testes: os testes `[vulkan]` rodam no lavapipe do contêiner e fazem `SKIP` sem driver. O jogo liga a camada
de validação com `LR_VULKAN_VALIDATION=1`; os testes do jogo falso com Vulkan usam isso e falham com qualquer
mensagem `[vulkan]` na saída. Os outros testes do jogo falso rodam com `--gpu null`, sem Vulkan.

O jogo guarda os shaders traduzidos em `--shader-cache-dir <pasta>` (o `legacyrecomp run` passa
`<raiz>/work/shaders/runtime`; sem a flag, ficam só na memória). Os `.spv` do cache **não** são módulos SPIR-V puros:
começam com um cabeçalho `LRSV` de 48 bytes (versão do tradutor, chave, checksum) seguido do microcódigo, então o
`spirv-dis` não os lê. Para depurar o tradutor: `tools/dev/container.sh run -- --dump-shaders` grava, ao lado de cada
`.spv` no volume `lr-data`, `<nome>.txt` com a desmontagem do microcódigo (e, numa falha, o motivo) e
`<nome>.raw.spv`, o módulo puro para o `spirv-dis`/`spirv-val`. Ao sair, o jogo escreve no stderr
`[gpu] shaders: keys=N vs=N ps=N translated=N disk=N failed=N new_last_100_swaps=N`. Mudou a saída do tradutor?
Suba `kTranslatorVersion` (`src/runtime/gpu/shader/translator.h`) e atualize a impressão digital do teste
`translator output fingerprint matches kTranslatorVersion` (`tests/test_gpu_shader_cache.cpp`; o valor novo aparece
na falha do teste).
Para validar o cache inteiro, rode dentro do contêiner (`tools/dev/container.sh shell`)
`tools/shader_cache_stats.sh [pasta]` (padrão `/data/work/shaders/runtime`): ele passa o SPIR-V de cada arquivo pelo
`spirv-val --target-env vulkan1.2` e imprime só contagens (arquivos, válidos, inválidos, por estágio e por versão).
Para julgar um screenshot do jogo (`run -- --screenshot-after <n> /data/shot.png`) sem abrir a imagem, rode no
contêiner `python3 -I tools/frame_stats.py /data/shot.png`: ele imprime só uma linha,
`colors=N mean_luminance=F edge_fraction=F` (cores RGB distintas, luminância média em [0, 1] e fração de pixels
cuja luminância difere em mais de 0,1 do vizinho da direita ou de baixo). O screenshot é dado do jogo: nunca o
versione.

Para descobrir quem escreve um buffer do jogo (ou provar que ninguém escreve), use a vigia de escrita:
`tools/dev/container.sh run -- --watch-guest <end>:<tam>` (hex com `0x` ou decimal; até 16 faixas, repetindo a
flag; `src/runtime/write_watch.h`). As páginas de 4 KiB da faixa ficam somente leitura; cada escrita nelas gera uma
falha que o tratador de SIGSEGV deixa passar (libera a página, executa a instrução em passo único e protege de novo
no SIGTRAP). O jogo escreve no stderr `[watch] armed range N <end>+<tam>`, uma linha
`[watch] write <end> range N n=<contagem> tid=<thread> fn <função> <- <chamadora> ... pc=<host>` por sítio novo
(as três funções convidadas mais internas da pilha do host, mapeadas pela tabela de funções), uma linha `(count)`
em cada potência de dois a partir de 1024, e `[watch] host zero ...` para escritas do próprio runtime que não
falham (`GuestMemory::zero`; as leituras de arquivo passam por um buffer e um `memcpy`, que a vigia vê). Alocações
e liberações de memória física que tocam a faixa aparecem como `[watch] host alloc|free ...` (não contam como
escrita). Depois do passo único, cada escrita é conferida: se deixou algum byte diferente de zero (até 8 bytes a
partir do endereço da falha) conta como `nonzero_stores`, então um escritor que só grava zeros aparece sem olhar os
dados. Na saída, o relatório imprime `[watch] summary range N ... writes=N nonzero_stores=N nonzero_bytes=N` e uma
linha por sítio. Só endereços e contagens, nunca dados. Qualquer outra falha continua sendo a saída 4. É diagnóstico: enquanto uma
thread executa o passo único, outra thread pode escrever na página sem ser contada. O trace da GPU
(`--trace-gpu`) dá o endereço convidado de cada microcódigo de shader (`shader load ... at=0x...`), que serve de
faixa para a vigia.

## ISO do jogo

`install --iso <arquivo>` monta a pasta do arquivo, somente leitura, em `/iso` e passa `/iso/<arquivo>`
ao instalador. Use o disco de um jogo que você possui; nada do jogo é copiado para o repositório.
Para os testes de integração, defina `LEGACYRECOMP_ISO` no host; o script monta a pasta da ISO somente leitura em `/iso` e repassa `LEGACYRECOMP_ISO=/iso/<arquivo>` ao container (vale também para `shell`):

```
LEGACYRECOMP_ISO=<caminho no host>/jogo.iso tools/dev/container.sh test -L integration
```

## Alterar o XenonRecomp (patches do LegacyRecomp)

O submódulo `thirdparty/XenonRecomp` fica fixado no `main` do upstream (hedge-dev, `ddd128b`). As nossas
mudanças no recompilador (instruções novas, correções) ficam como arquivos `git format-patch` em
`thirdparty/patches/xenonrecomp/NNNN-*.patch` (ADR 0004, M7). No configure, `cmake/LrSubmodulePatches.cmake`
(`lr_apply_submodule_patches`) copia o submódulo para `build/<preset>/_deps/XenonRecomp-patched`, aplica os patches nessa cópia (com
`git apply`) e compila a cópia; o checkout do submódulo nunca é alterado. Os patches são reaplicados quando
um deles muda ou quando muda o original de um arquivo que eles tocam. O hash dos patches entra na impressão
digital das etapas `analyse` e `recompile` (`LR_XENONRECOMP_COMMIT`), então um patch novo invalida o cache
da instalação. A cópia é só acrescentada e atualizada (`file(COPY)`), nunca podada: um arquivo que o upstream
apagar continua nela até alguém remover `build/<preset>/_deps/XenonRecomp-patched` (ou a pasta de build inteira),
o que vale fazer depois de atualizar o submódulo.

Para editar o recompilador:

```
cd thirdparty/XenonRecomp
git switch legacyrecomp            # branch local = ddd128b + os patches (crie com `git am` se não existir)
#   ... edite, e configure o build com os patches desligados (já estão no checkout):
cmake --preset dev -DLR_XENONRECOMP_APPLY_PATCHES=OFF      # no container: dentro de `container.sh shell`
#   ... teste-falha-implementa-passa com tests/recomp (`ctest --preset dev -L recomp`), um commit por família
rm -f ../patches/xenonrecomp/*.patch
git format-patch --no-signature -o ../patches/xenonrecomp ddd128b..legacyrecomp
git switch --detach ddd128b        # volta ao commit fixado
cd ../..
cmake --preset dev -DLR_XENONRECOMP_APPLY_PATCHES=ON
git add thirdparty/patches/xenonrecomp    # o gitlink do submódulo continua em ddd128b
```

Para recriar a branch local num clone novo: `git -C thirdparty/XenonRecomp switch -c legacyrecomp ddd128b` e
`git -C thirdparty/XenonRecomp am ../patches/xenonrecomp/*.patch`. Os patches mantêm autoria e créditos (o
PR #60 do upstream, de Mystixor, MIT). Um build com o submódulo fora de `ddd128b` e os patches ligados falha
no configure com uma mensagem que explica as duas saídas.
