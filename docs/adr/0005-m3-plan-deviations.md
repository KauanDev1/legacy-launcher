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

### M3. O fork do XenosRecomp escreve o cache no formato da Tarefa 14 e uma tabela de entradas de vértice (Tarefa 13)
- O quê: no modo `LR_LEGACYRECOMP` a própria ferramenta escreve o `shader_cache.cpp` com a struct
  `lr_shader_cache_entry` do plano (Tarefa 14), o blob `lr_shader_blob` (zstd de SPIR-V cru, sem o smol-v do upstream)
  e, além do plano, `lr_shader_vertex_inputs` (uma linha por entrada de vértice: fetch constant, offset, stride,
  formato, flags) e os tamanhos do blob. A entrada de vértice `location = i` é o elemento de vértice i do contêiner.
  O hash do contêiner aparece no log e em comentário, não na struct. O prelúdio HLSL é copiado para
  `share/legacyrecomp/tools/xenos_shader_common.h`. Contrato completo em `docs/architecture.md`.
- Por quê: o plano pede locais de entrada "por slot de vertex fetch" mas a struct da Tarefa 14 não tem onde dizer
  qual fetch constant, offset e formato alimentam cada local; o tradutor já decodifica isso e o runtime não deveria
  decodificar o microcódigo de novo. Gerar o `.cpp` na ferramenta deixa a etapa `shaders` só com
  executar, conferir `translated N of M` e compilar.
- Custo se errado: se a Tarefa 20 preferir derivar o layout do microcódigo, a tabela fica sem uso (custo zero); se o
  formato precisar mudar, muda num patch do fork e no teste `tests/shaders`.

### M4. Booleanos: dois bancos de 128 no bloco compartilhado (Tarefa 13)
- O quê: o plano fala em "128 booleanos" no bloco compartilhado. Como o bloco é um só por desenho (VS e PS), ele guarda
  os 256 bits dos 8 registradores de booleanos do Xenos, em ordem: o VS lê o banco 0..127 e o PS o banco 128..255, com
  índice `banco + (endereço do microcódigo % 128)`. As 32 constantes de laço são as mesmas para os dois estágios (como
  no Xenia, que também usa um único buffer de booleanos e laços).
- Por quê: não confirmamos se o microcódigo de PS endereça os booleanos de 0..127 ou de 128..255 (o upstream supõe
  0..15 por estágio; o Xenia indexa os 256 direto). O módulo 128 dentro do banco do estágio dá o resultado certo nos
  dois casos.
- Custo se errado: se o D3D do 360 mapear booleanos de outro jeito (por exemplo, um único banco compartilhado), um
  desvio condicional de PS lê o bit errado; a correção é o índice em `lrBoolIndex` (patch 0002) e a cópia no runtime.

### M5. Tradução mais estrita que o upstream (Tarefa 13)
- O quê: no modo LegacyRecomp, `cond_exec*` e `cond_exec_pred*` respeitam a condição (o upstream executava a cláusula
  sempre), os laços usam início e passo da constante (o upstream contava de 0 com passo 1 e exigia um literal `i<N>`),
  `aL` é restaurado ao fim de laços aninhados, `r0.x` começa com o índice do vértice e `oPos` começa zerado. Todo
  acesso a tabela do contêiner é conferido contra o tamanho dele; contêiner malformado, exportação sem destino,
  laço aninhado num shader com saltos e o mesmo microcódigo com traduções diferentes falham a tradução em vez de
  `assert`. Ficam de fora (falham alto): texturas 1D e exportação de memória; o tamanho de ponto é descartado.
- Por quê: o jogo não é o Unleashed; o que o upstream supunha só valia para os shaders dele, e a etapa `shaders` precisa
  falhar alto (spec, seção 7).
- Custo se errado: shaders reais que usem as construções recusadas param a etapa com a mensagem; o portão da Tarefa 15
  mede quantos.
