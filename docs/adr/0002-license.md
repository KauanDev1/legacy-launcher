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
- Os arquivos de licença dos terceiros ainda não são copiados para `share/legacyrecomp/` (só o
  `COPYING` do simde e as licenças das fontes). A Task 17 (AppImage) deve incluir todos.

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
| libmspack (só `lzxd.c`) | LGPL-2.1 (`thirdparty/XenonRecomp/thirdparty/libmspack/libmspack/COPYING.LIB` e cabeçalho do arquivo) | Compilado dentro de `libXenonUtils.a`; os símbolos `lzxd_*` estão ligados estaticamente em `legacyrecomp`, `legacyrecomp-gui`, nas duas ferramentas e no jogo (conferido no `lr_fake_game`, que liga as mesmas bibliotecas). Como a ligação é estática, a LGPL exige que o usuário possa religar com outra versão da biblioteca: o código-fonte completo do LegacyRecomp e dos submódulos é público, o que atende a isso |
| tiny-AES-c | Unlicense, domínio público (`thirdparty/XenonRecomp/thirdparty/tiny-AES-c/unlicense.txt`) | Dentro de `libXenonUtils.a` (decifra o XEX) |
| TinySHA1 | Licença tipo ISC no cabeçalho de `TinySHA1.hpp` | Só cabeçalho, usado por `XenonUtils` |
| disasm (desmontador PowerPC do GNU binutils) | GPL-2.0-or-later (cabeçalho de `thirdparty/XenonRecomp/thirdparty/disasm/ppc-dis.c`) | Ligado nas ferramentas `XenonAnalyse` e `XenonRecomp`. `libdisasm.a` também é instalada em `share/legacyrecomp/runtime/lib/` e passada ao link do jogo, mas nenhum símbolo dela é puxado (conferido com `nm` em `legacyrecomp`, `legacyrecomp-gui` e no `lr_fake_game`, que liga as mesmas bibliotecas do jogo). As ferramentas são programas separados, que o launcher só executa |
| SDL2 | zlib (pacote do sistema, `/usr/share/licenses/sdl2-compat/LICENSE`) | Ligada dinamicamente em `legacyrecomp-gui` e no jogo; dependência do sistema, não submódulo |
| Inter, JetBrains Mono | SIL OFL 1.1 (`assets/fonts/LICENSE-*.txt`) | Copiadas para `share/legacyrecomp/fonts/` com as licenças |
| Xenia | BSD (não está no repositório; licença não conferida localmente) | Nada. Usado só como referência para offsets de estruturas e formatos. O XenonRecomp, segundo o próprio README, reaproveita código do Xenia Canary para aplicar patches em XEX |

Resumo: o MIT do LegacyRecomp é compatível com todos os componentes acima. Quem redistribuir o pacote
completo precisa levar junto os avisos de licença dos terceiros e, para o desmontador do binutils
(GPL-2.0-or-later) e a libmspack (LGPL-2.1), garantir acesso ao código-fonte correspondente, que está
nos submódulos.
