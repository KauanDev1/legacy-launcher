# ADR 0002: Licença MIT

- Data: 2026-10-14
- Estado: aceita (substitui a decisão anterior de usar GPL-3.0)
- Decisão do dono do projeto, registrada na Task 16a.

## Contexto

O LegacyRecomp começou sob GPL-3.0. O motivo era prático: o UnleashedRecomp, o projeto da hedge-dev
que serve de referência para jogos recompilados com o XenonRecomp, é GPL-3.0, e a GPL deixaria portar
trechos dele sem problema de licença.

Até aqui nenhum código do UnleashedRecomp foi copiado. Ele foi usado só como referência de estrutura e
de flags de compilação (ver `src/runtime/template/CMakeLists.txt`). O dono do projeto preferiu uma
licença permissiva, que não imponha condições a quem reutilizar o código do launcher, do pipeline e do
runtime.

## Decisão

O código do LegacyRecomp passa a ser licenciado sob **MIT** (`LICENSE`, "Copyright (c) 2026
LegacyRecomp contributors"). Todo arquivo nosso começa com `SPDX-License-Identifier: MIT` (comentário
`//` em C/C++, `#` em CMake, Python e shell, `<!-- -->` em Markdown quando houver). O cabeçalho que o
gerador de stubs de imports (`src/pipeline/import_stubs.cpp`) escreve no código gerado também passou
a ser MIT.

Ficam de fora da troca: `thirdparty/` (cada submódulo mantém a própria licença) e as fontes em
`assets/fonts/` (SIL OFL 1.1, com os arquivos de licença ao lado). O `assets/fonts/fonts.lock` é um
arquivo nosso (procedência e hashes das fontes) e recebeu o cabeçalho MIT como os demais.

## Consequências

- **UnleashedRecomp é só referência.** Não se copia código dele (nem trechos pequenos). Ler o código
  para entender o que fazer e escrever a nossa versão continua permitido.
- O MIT cobre apenas o nosso código. O pacote distribuído (instalação via `cmake --install` e, a partir
  da Task 17, o AppImage) leva também código de terceiros com outras licenças, listado abaixo. As
  licenças GPL/LGPL de alguns desses componentes valem para eles, não para o nosso código.
- Os pacotes binários têm obrigações próprias (LGPL-2.1 e GPL-2.0-or-later de componentes de terceiros),
  descritas em "Obrigações dos pacotes binários" no fim deste documento.

### Dependências e onde cada uma entra

Licenças conferidas nos arquivos de licença dentro de `thirdparty/` (ou do sistema, no caso do SDL2)
em 2026-10-14. "Entra em" foi conferido com `nm` nos binários do build `dev` e com a lista de
arquivos que `cmake --install` produz.

| Componente | Licença (arquivo conferido) | Entra em |
|---|---|---|
| XenonRecomp, XenonAnalyse, XenonUtils (hedge-dev) | MIT (`thirdparty/XenonRecomp/LICENSE.md`) | Ferramentas `XenonAnalyse`/`XenonRecomp` (executáveis separados, chamados via `posix_spawn`); `libXenonUtils.a` é ligada estaticamente em `legacyrecomp`, `legacyrecomp-gui` e `legacyrecomp-game` |
| Dear ImGui | MIT (`thirdparty/imgui/LICENSE.txt`) | `legacyrecomp-gui` |
| Catch2 | BSL-1.0 (`thirdparty/Catch2/LICENSE.txt`) | Só nos testes (`lr_tests`); não é distribuído |
| toml++ | MIT (`thirdparty/XenonRecomp/thirdparty/tomlplusplus/LICENSE`) | `legacyrecomp`, `legacyrecomp-gui`, `XenonRecomp` |
| {fmt} | MIT (`thirdparty/XenonRecomp/thirdparty/fmt/LICENSE`) | `legacyrecomp`, `legacyrecomp-gui`, ferramentas do XenonRecomp |
| xxHash (biblioteca) | BSD-2-Clause (`thirdparty/XenonRecomp/thirdparty/xxHash/LICENSE`) | `XenonRecomp`. O utilitário `xxhsum` (GPL-2.0, `cli/COPYING`) é compilado como subproduto, mas não é instalado nem distribuído |
| SIMD Everywhere (simde) | MIT (`thirdparty/XenonRecomp/thirdparty/simde/COPYING`) | Cabeçalhos incluídos pelo `ppc_context.h` no código gerado; copiados para `share/legacyrecomp/runtime/include/simde/` com o `COPYING` |
| libmspack (só `lzxd.c`) | LGPL-2.1 (`thirdparty/XenonRecomp/thirdparty/libmspack/libmspack/COPYING.LIB` e cabeçalho do arquivo) | Compilado dentro de `libXenonUtils.a`; os símbolos `lzxd_*` estão ligados estaticamente em `legacyrecomp`, `legacyrecomp-gui`, nas duas ferramentas e no jogo (conferido no `lr_fake_game`, que liga as mesmas bibliotecas). Obrigações ao distribuir binários: ver "Obrigações dos pacotes binários" abaixo |
| tiny-AES-c | Unlicense, domínio público (`thirdparty/XenonRecomp/thirdparty/tiny-AES-c/unlicense.txt`) | Dentro de `libXenonUtils.a` (decifra o XEX) |
| TinySHA1 | Licença tipo ISC no cabeçalho de `TinySHA1.hpp` | Só cabeçalho, usado por `XenonUtils` |
| disasm (desmontador PowerPC do GNU binutils) | GPL-2.0-or-later (cabeçalho de `thirdparty/XenonRecomp/thirdparty/disasm/ppc-dis.c`; a pasta não tem arquivo `COPYING`) | Só nas ferramentas `XenonAnalyse` e `XenonRecomp`, que por isso são, como obra combinada, GPL-2.0-or-later. Não entra em `legacyrecomp`, `legacyrecomp-gui` nem no jogo: desde a decisão R25 (2026-10-14) a `libdisasm.a` não é mais copiada para `share/legacyrecomp/runtime/lib/` nem passada ao link do jogo, e o teste `no_gpl_disassembler_in_binaries` (rótulo `unit`) falha se o símbolo `decode_insn_ppc` aparecer nesses binários ou se a `libdisasm.a` voltar a ser copiada |
| SDL2 | zlib (pacote do sistema, `/usr/share/licenses/sdl2-compat/LICENSE`; no AppImage, `LICENSE.txt` do SDL2 2.32.10) | Ligada dinamicamente em `legacyrecomp-gui` e no jogo; dependência do sistema, não submódulo. No AppImage, compilada estática a partir do fonte fixado e ligada estaticamente na GUI e no jogo |
| Inter, JetBrains Mono | SIL OFL 1.1 (`assets/fonts/LICENSE-*.txt`) | Copiadas para `share/legacyrecomp/fonts/` com as licenças |
| Xenia | BSD (não está no repositório; licença não conferida localmente) | Nada. Usado só como referência para offsets de estruturas e formatos. O XenonRecomp, segundo o próprio README, reaproveita código do Xenia Canary para aplicar patches em XEX |

### Obrigações dos pacotes binários

O código-fonte do LegacyRecomp é MIT. Os pacotes binários que distribuímos (instalação via
`cmake --install`, AppImage, qualquer arquivo de release) juntam esse código com componentes de outras
licenças e, por isso, carregam as obrigações combinadas de todos eles:

- **LGPL-2.1 (libmspack, `lzxd.c`)**, ligada estaticamente em `legacyrecomp` e `legacyrecomp-gui` (e no
  jogo, que o usuário compila na própria máquina). Cada release binária leva o texto da LGPL-2.1, um aviso
  de que a libmspack está incluída e sob qual licença, e o código-fonte correspondente àquela release
  exata, oferecido junto com o binário. Como entregamos o código-fonte completo, submódulos incluídos, o
  usuário consegue religar os programas com outra versão da libmspack, que é o que a LGPL exige para
  ligação estática.
- **GPL-2.0-or-later (desmontador do binutils)**: as ferramentas `XenonAnalyse` e `XenonRecomp` são, como
  obras combinadas, GPL-2.0-or-later. Cada release binária leva o texto da GPL-2.0 e o código-fonte
  correspondente dessas ferramentas. A pasta `thirdparty/XenonRecomp/thirdparty/disasm/` não traz um
  arquivo `COPYING`, então o texto da GPL-2.0 é fornecido por nós.
- **Avisos dos demais componentes** (MIT, BSD-2-Clause, zlib, OFL, Unlicense, ISC): os textos de licença
  vão juntos no pacote. A instalação via `cmake --install` copia só o `COPYING` do simde e as licenças das
  fontes para `share/legacyrecomp/`; o AppImage leva o `THIRD-PARTY-LICENSES` completo
  (`usr/share/doc/legacyrecomp/`, gerado por `packaging/appimage/licenses.sh`, ver `docs/packaging.md`).
- **Código-fonte com submódulos**: os arquivos `.tar.gz` que o GitHub gera para cada tag **não incluem os
  submódulos**. Por isso cada release precisa anexar um arquivo de código-fonte completo, com os
  submódulos, ao lado do AppImage (`LegacyRecomp-<versão>-source.tar.gz`, escrito pelo
  `packaging/appimage/build.sh`).

Isto é uma leitura de engenharia das licenças, não um parecer jurídico.
