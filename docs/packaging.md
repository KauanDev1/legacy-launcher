# AppImage

O LegacyRecomp é distribuído como um AppImage autossuficiente (`LegacyRecomp-x86_64.AppImage`): ele leva o
launcher (CLI e GUI), os recursos de `share/legacyrecomp/` e o toolchain que compila o jogo na máquina do
usuário. Não é preciso instalar clang, cmake, ninja nem SDL2. Decisão registrada no §14 da spec do M1.

## Uso

```sh
chmod +x LegacyRecomp-x86_64.AppImage
./LegacyRecomp-x86_64.AppImage                                  # GUI
./LegacyRecomp-x86_64.AppImage gui --root ~/jogos/lr            # GUI com opções (ver docs/gui.md)
./LegacyRecomp-x86_64.AppImage install --iso ~/Games/minecraft.iso
./LegacyRecomp-x86_64.AppImage run -- --headless
./LegacyRecomp-x86_64.AppImage doctor
```

O `AppRun` abre a GUI quando não há argumentos ou quando o primeiro é `gui`; qualquer outra coisa vai para a
CLI `legacyrecomp`. Sem FUSE no sistema, use `--appimage-extract-and-run` como primeiro argumento (ou a
variável `APPIMAGE_EXTRACT_AND_RUN=1`).

Na primeira compilação o zig monta a libc++, a libunwind e os stubs da glibc no cache dele
(`$XDG_CACHE_HOME/zig`, normalmente `~/.cache/zig`, cerca de 30 s e algumas dezenas de MB). As compilações
seguintes reaproveitam esse cache.

## Gerar o AppImage

```sh
packaging/appimage/build.sh [--out DIR] [--jobs N]
```

Saída em `packaging/appimage/out/` (ou `--out`): `LegacyRecomp-x86_64.AppImage`,
`LegacyRecomp-<versão>-source.tar.gz` e o `AppDir/` usado para montar o AppImage. A versão vem de
`LR_VERSION` (padrão `0.1.0-m1`). Downloads, fontes extraídas, builds e o cache do zig ficam em
`packaging/appimage/cache/`; as duas pastas são ignoradas pelo git. O script pode ser rodado de novo: os
builds são incrementais e o `AppDir` é refeito do zero.

Requisitos da máquina que gera o AppImage: `bash`, `git`, `curl`, `tar`, `xz`, `gzip`, `python3` (bootstrap
do Ninja), `sha256sum`, `readelf`, `openssl` (opcional, confere a assinatura minisign do zig) e os headers de
desenvolvimento dos backends do SDL2 que se quer embutir (X11 e extensões, Wayland com `wayland-scanner`,
libdecor, PipeWire, PulseAudio, ALSA, D-Bus, IBus, udev, DRM/GBM). O SDL2 desliga sozinho o backend cujo
header não encontra; o resumo fica em `cache/build/sdl2.log`. FUSE não é necessário (o `appimagetool` roda
com `APPIMAGE_EXTRACT_AND_RUN=1`).

O que o `build.sh` faz:

1. Baixa as versões fixadas em `packaging/appimage/toolchain.lock`, cada uma numa pasta nova, e confere o
   SHA-256 antes de extrair. O tarball do zig também tem a assinatura minisign conferida (Ed25519 sobre o
   BLAKE2b-512, com `openssl`).
2. Monta o `AppDir/usr/share/legacyrecomp/toolchain/` (layout abaixo), com o zig reduzido ao que o alvo
   `x86_64-linux-gnu` usa (sem as outras libcs, sem headers de outros alvos, sem TSan e sem docs: de 406 MB
   para 246 MB) e o cmake sem GUI, `ctest`, `cpack` e documentação.
3. Compila com esse mesmo zig, para `x86_64-linux-gnu.2.31`: o Ninja (`configure.py --bootstrap`), o SDL2
   estático com PIC e o LegacyRecomp (preset `release` com `CMAKE_BUILD_TYPE=Release`, alvos
   `legacyrecomp`, `legacyrecomp-gui` e `lr_resources`), e instala com `cmake --install` em `AppDir/usr`.
4. Confere todos os ELF compilados: nenhum pode exigir glibc acima da 2.31 nem bibliotecas fora da glibc.
5. Escreve `THIRD-PARTY-LICENSES` (`packaging/appimage/licenses.sh`) em `usr/share/doc/legacyrecomp/` e na
   raiz do AppDir, mais `AppRun`, `.desktop` e ícone, e chama o `appimagetool` com o runtime fixado.
6. Gera o arquivo-fonte: `git archive` do `HEAD` mais `git submodule foreach --recursive git archive`,
   concatenados sob o prefixo `LegacyRecomp-<versão>/`. Os `.tar.gz` que o GitHub gera para as tags não
   trazem os submódulos. Mudanças não commitadas ficam de fora (o script avisa).

### Versões fixadas

| Componente | Versão | Como o hash foi conferido |
|---|---|---|
| Zig | 0.17.0 (clang 22.1.8) | `shasum` do `index.json` oficial e assinatura minisign com a chave do Zig |
| CMake | 4.4.4 (binário oficial, glibc ≥ 2.17, libstdc++ estática) | `cmake-4.4.4-SHA-256.txt` da release |
| Ninja | 1.13.2 (fonte, compilado com zig) | sem checksum oficial; hash do arquivo da tag |
| SDL2 | 2.32.10 (fonte, compilado estático com zig) | assinatura GPG de Sam Lantinga (`1528 635D 8053 A57F 77D1 E086 30A5 9377 A776 3BE6`) |
| appimagetool | 1.9.1 | sem checksum oficial; hash do arquivo baixado |
| AppImage type2 runtime | 20251108 | sem checksum oficial; hash do arquivo baixado |

O binário oficial do Ninja não serve: ele depende da `libstdc++.so.6` do sistema. Compilado com o zig, ele só
depende da glibc.

## Layout do toolchain embutido

```
usr/share/legacyrecomp/toolchain/
  bin/cc, bin/c++          scripts sh: exec ../zig/zig cc|c++ -target x86_64-linux-gnu.2.31 "$@"
  bin/ar, bin/ranlib       scripts sh: zig ar / zig ranlib
  bin/ninja                Ninja 1.13.2
  cmake/bin/cmake          CMake 4.4.4, com cmake/share/cmake-4.4/{Modules,Templates,Licenses}
  zig/zig, zig/lib/        Zig 0.17.0 (clang, lld, libc++, headers e stubs da glibc)
  sdl2/                    SDL2 2.32.10 estático: include/, lib/libSDL2.a, lib/cmake/SDL2/
```

`lr::pipeline::resolve_toolchain` (`src/pipeline/toolchain.h`) entra no modo embutido quando
`<recursos>/toolchain/` existe. Aí o `doctor` só confere se esses arquivos estão lá (faltando um, o erro é
`ToolMissing` com a dica de AppImage corrompido) e a etapa `build` configura o template com
`-DCMAKE_CXX_COMPILER=toolchain/bin/c++`, `-DCMAKE_MAKE_PROGRAM=toolchain/bin/ninja` e
`-DCMAKE_PREFIX_PATH=toolchain/sdl2`. O jogo liga o SDL2 estaticamente e não precisa de nenhum `.so` do SDL;
o SDL continua abrindo com `dlopen` as bibliotecas X11/Wayland/áudio do sistema, como sempre.

O AppImage é montado num caminho diferente a cada execução (`/tmp/.mount_XXXX`). Como o caminho do template
muda, a etapa `build` apaga `work/build` e recompila do zero quando precisa rodar de novo (ver
`reset_stale_build_dir`); depois de uma instalação completa ela fica em cache e isso não acontece.

## Por que tudo é compilado com o zig

O jogo é compilado pelo zig na máquina do usuário, com a libc++ do zig. As bibliotecas do runtime que ele
liga (`liblr_runtime_core.a`, `libXenonUtils.a`) precisam usar a mesma biblioteca C++, então também são
compiladas com o zig. O mais simples foi compilar o projeto inteiro assim, o que traz uma vantagem: os
launchers ligam a libc++ estaticamente e miram a glibc 2.31, em vez de depender da `libstdc++` e da glibc da
máquina que gerou o AppImage.

Ajustes que isso exigiu:

- `std::to_string` no `file_time_type` (o `rep` é `__int128` na libc++) e `__builtin_cpu_supports` (precisa do
  `__cpu_model` do compiler-rt, que o runtime do zig não tem; o `doctor` passou a usar CPUID + XGETBV).
- O `{fmt}` embutido no XenonRecomp usa `malloc` sem incluir `<cstdlib>`; o `build.sh` passa
  `-include cstdlib` no build do launcher (não mexemos no submódulo).
- O backend SDL2 do Dear ImGui inclui `SDL_syswm.h`, que inclui os headers do X11: o `build.sh` passa
  `-idirafter /usr/include`, que só é consultado depois dos headers da glibc 2.31 do zig. A conferência de
  versões da glibc no fim do `build.sh` pega qualquer símbolo novo que escape.
- `-Wl,--strip-debug` nos executáveis: a libc++ que o zig compila tem informação de depuração (cerca de 20 MB
  por launcher).
- As ferramentas externas são executadas com o caminho completo em `argv[0]`: o CMake acha os próprios
  módulos a partir dele.

## Spike (Task 17, passo 1)

Com o zig 0.17.0 e o template do jogo, compilando o código gerado real (cópia de `work/generated` de uma
instalação completa, 219 arquivos) e o runtime compilado pelo zig:

- `-march=sandybridge`, `-ffp-model=strict` e `-fexceptions` funcionam como no clang do sistema (o zig repassa
  as flags; o binário tem cerca de 122 mil instruções VEX). O simde compila sem ajuste. Nenhum aviso no build.
- Tempo: 255 s com 9 jobs (12 threads, 19 GB de RAM), contra cerca de 4,5 min com o clang 22 do sistema.
- O jogo roda com `--headless` e sai com código 3 em `NtAllocateVirtualMemory`, igual ao build do sistema.
- O binário exige glibc 2.29 (os stubs miram a 2.31) e só depende de `libc`, `libm`, `libpthread`, `libdl`
  e `ld-linux`.
- O executável fica com 144 MB, contra 48 MB com o clang do sistema: a diferença é a informação de depuração
  da libc++ do zig. O `.text` tem o mesmo tamanho.

O plano B (LLVM oficial + sysroot de uma distro antiga) não foi necessário.

## Aceite (Task 17, passo 5)

Executado em 2026-10-14 na máquina de desenvolvimento (Manjaro, glibc 2.44, 12 threads, 19 GB de RAM).

- `build.sh` com os downloads já em cache e sem nenhum build anterior (cache do zig vazio): 221 s. De novo,
  sem mudanças: 29 s.
- `LegacyRecomp-x86_64.AppImage`: 81,6 MB (o `AppDir` tem 319 MB; zstd). Arquivo-fonte: 8,7 MB.
- Ambiente do teste: `env -i HOME=$HOME PATH=<pasta> XDG_CACHE_HOME=<pasta nova>`, onde a pasta do `PATH` só
  tem links para `sh`, `dirname`, `readlink`, `fusermount` e `fusermount3` (nenhum `clang`, `cc`, `c++`,
  `ld`, `ar`, `cmake`, `ninja`, `make` ou `pkg-config`), e pasta de dados nova em disco
  (`work/appimage-root`). O FUSE estava disponível, então não foi preciso `--appimage-extract-and-run`.
- `./LegacyRecomp-x86_64.AppImage install --iso <ISO> --root work/appimage-root`: código 0 em 286 s, as sete
  etapas. O configure do template levou 27 s, quase tudo o zig montando a libc++ no cache vazio (99 MB ao
  final). O `CMakeCache.txt` do jogo aponta para `/tmp/.mount_*/usr/share/legacyrecomp/toolchain/bin/c++`.
- `./LegacyRecomp-x86_64.AppImage run --root work/appimage-root -- --headless`: código 3 em 0,2 s, com
  `Unimplemented import: NtAllocateVirtualMemory`, igual ao build do sistema.
- `./LegacyRecomp-x86_64.AppImage gui --root work/appimage-root --screenshot <arquivo>`: a GUI abre com as
  fontes Inter/JetBrains Mono e mostra as sete etapas concluídas.
- O arquivo-fonte extraído compila com `cmake --preset release && cmake --build --preset release` (clang do
  sistema, 94 s). Fora de um checkout git, o `resources.cmake` não consegue ler o commit do XenonRecomp e
  avisa; isso só afeta o cache das etapas `analyse`/`recompile`. Extraído dentro de outro repositório git (como
  neste teste, em `work/`), o `git rev-parse` acha o repositório de fora e registra o commit dele.
- `THIRD-PARTY-LICENSES` está na raiz do AppImage e em `usr/share/doc/legacyrecomp/` (156 KB).

## Limitações

- **glibc mínima**: os launchers, as ferramentas, o Ninja e o jogo são compilados pelo zig para a glibc 2.31
  e, na prática, exigem 2.29 (launchers e jogo), 2.17 (Ninja) e 2.16 (XenonAnalyse e XenonRecomp); o CMake
  oficial exige 2.17 e o zig é estático. Isso cobre Debian 11, Ubuntu 20.04, Fedora 32 e mais novas. O
  AppImage foi gerado numa máquina com glibc 2.44, e o alvo `-target x86_64-linux-gnu.2.31` evita depender
  dela, mas até agora só foi testado nessa máquina; testar numa distro antiga (container) fica para depois.
- **Backends do SDL2**: dependem dos headers presentes na máquina que gerou o AppImage (ver requisitos).
- **x86-64 com AVX** apenas, como no build a partir do código-fonte.
- **libfuse no runtime do AppImage**: o runtime type2 liga estaticamente a libfuse 3 (LGPL-2.1). O
  `THIRD-PARTY-LICENSES` aponta o repositório e os scripts que dizem quais fontes ele usa; não anexamos essas
  fontes à release.

## Licenças no AppImage

`usr/share/doc/legacyrecomp/THIRD-PARTY-LICENSES` (cópia em `THIRD-PARTY-LICENSES` na raiz do AppImage) tem a
tabela de componentes, as notas de obra combinada (XenonAnalyse/XenonRecomp GPL-2.0-or-later; libmspack
LGPL-2.1 estática nos launchers e nas ferramentas, com o arquivo-fonte completo da release ao lado) e o texto
integral de cada licença: as da tabela do ADR 0002, mais zig, LLVM/libc++, glibc (`LICENSES`), headers do
Linux, CMake e as bibliotecas que ele embute, Ninja, SDL2, o runtime do AppImage e o que ele liga. Os textos
que não estão no código nem nos downloads ficam em `packaging/appimage/licenses/` (origem em `SOURCES`),
incluindo o texto da GPL-2.0 que o `disasm/` não traz.
