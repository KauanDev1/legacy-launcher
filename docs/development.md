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

- Imagem `legacyrecomp-dev` (Arch Linux com clang, cmake, ninja, SDL2, pkgconf, git, python, binutils e
  rsync). É reconstruída pelo Docker apenas quando o `Dockerfile` muda.
- O repositório é montado somente leitura em `/src`. Os arquivos **rastreados pelo git** (incluindo
  submódulos) são sincronizados com `rsync --delete` para o volume `lr-build`, montado em `/work`;
  também entram os arquivos novos ainda não rastreados (que não estejam no `.gitignore`); arquivos rastreados mas apagados na árvore de trabalho não são copiados e saem do volume. O build fica em `/work/build/<preset>` e sobrevive
  entre execuções (a sincronização não apaga `build/`).
- O container roda com `--init` (reaper de processos órfãos); sem isso dois testes de cancelamento falham.
- Os dados (cache de instalação, jogo extraído, executável gerado) ficam no volume `lr-data`, em `/data`.
- Para limpar tudo: `docker volume rm lr-build lr-data`.

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
`thirdparty/patches/xenonrecomp/NNNN-*.patch` (ADR 0004, M7). No configure, `cmake/LrXenonRecompPatches.cmake`
copia o submódulo para `build/<preset>/_deps/XenonRecomp-patched`, aplica os patches nessa cópia (com
`git apply`) e compila a cópia; o checkout do submódulo nunca é alterado. Os patches são reaplicados quando
um deles muda ou quando muda o original de um arquivo que eles tocam. O hash dos patches entra na impressão
digital das etapas `analyse` e `recompile` (`LR_XENONRECOMP_COMMIT`), então um patch novo invalida o cache
da instalação.

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
