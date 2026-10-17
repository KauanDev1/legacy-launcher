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
  arquivos novos precisam de `git add` para entrar. O build fica em `/work/build/<preset>` e sobrevive
  entre execuções (a sincronização não apaga `build/`).
- O container roda com `--init` (reaper de processos órfãos); sem isso dois testes de cancelamento falham.
- Os dados (cache de instalação, jogo extraído, executável gerado) ficam no volume `lr-data`, em `/data`.
- Para limpar tudo: `docker volume rm lr-build lr-data`.

## ISO do jogo

`install --iso <arquivo>` monta a pasta do arquivo, somente leitura, em `/iso` e passa `/iso/<arquivo>`
ao instalador. Use o disco de um jogo que você possui; nada do jogo é copiado para o repositório.
Os testes de integração (`-L integration`) ainda não são expostos pelo script.
