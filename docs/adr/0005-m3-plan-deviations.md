# ADR 0005: Desvios do plano do M3

- Status: aceita
- Data: 2026-10-19

Registro de cada decisão que se afastou do plano (`docs/superpowers/plans/2026-10-19-legacyrecomp-m3.md`) ou
do spec (`docs/superpowers/specs/2026-10-19-legacyrecomp-m3-design.md`), ou que preencheu uma lacuna dele.
Formato: o quê, por quê, custo se estiver errada.

## Decisões

### M1. Alvos de `bl` são só fronteiras na descoberta de funções por ponteiro (Tarefa 2)
- O quê: o plano manda `discover_pointer_functions` emitir, além dos endereços vindos de ponteiros, todo alvo de
  `bl` não coberto por uma função conhecida. Ela emite só candidatas vindas de ponteiros (palavras em seções de
  dados e constantes `lis` + `addi`). Os alvos de `bl` continuam servindo de fronteira: uma candidata que cai
  num deles sem terminar é rejeitada, um `b` para um deles é chamada de cauda, e um `bl` para o interior de uma
  candidata a rejeita. Uma candidata que também é alvo de `bl` não é emitida. Além disso, qualquer `bctr`
  alimentado por load indexado dentro da candidata (não só o último) a rejeita.
- Por quê: o XenonRecomp já cria uma função em cada alvo de `bl` não coberto e varre os vãos entre funções
  (`thirdparty/XenonRecomp/XenonRecomp/recompiler.cpp`), e as entradas `functions` da configuração vencem a
  análise dele por contenção. Emitir alvos de `bl` só trocaria as fronteiras dele pelas nossas em milhares de
  funções, sem ganho e sem validação na ISO real.
- Custo se errado: se o XenonRecomp errar a fronteira de alguma função chamada por `bl`, ela continua errada
  (como antes do M3) e precisa de uma entrada manual em `recomp.toml`; uma função alcançada por ponteiro que
  também é alvo de `bl` fica com a fronteira do XenonRecomp.

### M2. DXC do sistema no desenvolvimento; o release oficial foi rejeitado e a compilação do fonte fica para a Tarefa 24 (Tarefa 6)
- O quê: o plano manda fixar em `toolchain.lock` o tarball oficial `linux_dxc_*.x86_64.tar.gz` do DXC e baixá-lo no
  `Dockerfile` e no `build.sh`. Não fizemos isso. O DXC usado no desenvolvimento e nas instalações a partir do
  fonte é o do sistema (pacote `directx-shader-compiler`, no `Dockerfile` do contêiner: `dxc`, `libdxcompiler.so`
  e `dxcapi.h` em `/usr/include/dxc/`). O CMake define o alvo importado `Microsoft::DirectXShaderCompiler` apontando
  para essa biblioteca e esses cabeçalhos antes de adicionar o XenosRecomp, então o submódulo aninhado `dxc-bin` do
  upstream (binários prontos) nunca é inicializado; `fmt::fmt` e `xxHash::xxhash` são os do XenonRecomp (mesmos
  commits), e só `smol-v` e `zstd` são inicializados (`tools/dev/init-submodules.sh`). O `doctor` exige `dxc` no PATH
  (modo sistema) ou `<recursos>/toolchain/dxc/` (modo embutido, que o AppImage ainda não preenche). O AppImage
  levará um DXC compilado de uma tag fixada do fonte, com o zig mirando glibc 2.31, na Tarefa 24.
- Por quê: o piso de glibc do release oficial passa do 2.31 do AppImage. Medido com `readelf -V` em `dxc` e
  `libdxcompiler.so` (a medida vale para os dois arquivos): v1.9.2609, v1.9.2602.24 e v1.10.2605.37 exigem
  glibc 2.38; v1.8.2505.1 e v1.8.2502 exigem 2.34. Os dois arquivos também dependem de `libstdc++.so.6`,
  `libgcc_s.so.1` e `libz.so.1`, que o `build.sh` do AppImage proíbe (só libc, libm, libdl, libpthread e librt).
  Ver a tabela em `docs/packaging.md`.
- Custo se errado: enquanto a Tarefa 24 não entrar, o AppImage não traz o DXC e a futura etapa de shaders não roda
  nele (o `doctor` avisa); instalações a partir do fonte dependem de um pacote do sistema que nem toda
  distribuição tem (no Arch ele existe; nas outras é preciso instalar o release oficial no PATH). Compilar o DXC
  (LLVM) com o zig custa tempo de build e manutenção de um patchset e um tempo de CI de dezenas de minutos; se
  ficar caro demais, a alternativa é subir o piso de glibc do AppImage para 2.38, o que corta distribuições LTS
  anteriores a 2023. Dependemos também de a versão do sistema continuar compatível com o `dxcapi.h` que o
  XenosRecomp espera: ele foi testado aqui com 1.10.2605.37.
