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

### M4. Booleanos: o arquivo de 256 bits inteiro no bloco compartilhado, endereçado direto (Tarefa 13)
- O quê: o plano fala em "128 booleanos" no bloco compartilhado. O bloco guarda os 256 bits dos 8 registradores de
  booleanos do Xenos, copiados literalmente e em ordem, e o shader lê `g_Bool(<endereço>)` com o endereço de 8 bits
  da instrução de controle de fluxo, sem banco por estágio nem módulo. As 32 constantes de laço são as mesmas para os
  dois estágios. A primeira versão usava `banco do estágio + endereço % 128`; a rodada de revisão 1 a trocou pelo
  endereçamento direto.
- Por quê: o `bool_address` do Xenos é um índice de 8 bits no arquivo de 256 booleanos, igual nos dois estágios, como
  no Xenia (referência), que também usa um único buffer de booleanos e laços.
- Custo se errado: se o microcódigo de PS endereçar só 0..127 esperando o banco do PS, um desvio condicional de PS lê
  o bit errado; a correção é o índice emitido em `shader_recompiler.cpp` (patch 0004) e o teste
  `pixel shader bool constant 130 indexes the shared bool file directly`.

### M5. Tradução mais estrita que o upstream (Tarefa 13)
- O quê: no modo LegacyRecomp, `cond_exec*` e `cond_exec_pred*` respeitam a condição (o upstream executava a cláusula
  sempre), os laços usam início e passo da constante (o upstream contava de 0 com passo 1 e exigia um literal `i<N>`),
  `aL` é salvo e restaurado em todo laço (também no `switch` de pc), um laço com `is_repeat` mantém o `aL` atual, o
  segundo operando constante de uma instrução usa `const_1_rel_abs` (o upstream usava sempre `const_0_rel_abs`),
  `r0.x` começa com o índice do vértice e `oPos` começa zerado. Todo
  acesso a tabela do contêiner é conferido contra o tamanho dele; contêiner malformado, exportação sem destino,
  laço aninhado num shader com saltos e o mesmo microcódigo com traduções diferentes falham a tradução em vez de
  `assert`. Ficam de fora (falham alto): texturas 1D e exportação de memória; o tamanho de ponto é descartado.
- Por quê: o jogo não é o Unleashed; o que o upstream supunha só valia para os shaders dele, e a etapa `shaders` precisa
  falhar alto (spec, seção 7).
- Custo se errado: shaders reais que usem as construções recusadas param a etapa com a mensagem; o portão da Tarefa 15
  mede quantos.

### M6. Cache de shaders no runtime: registro com `Result`, hash no `GpuSystem`, ausência fatal só com backend que liga shaders (Tarefa 15)
- O quê: `ShaderCache::register_entries` devolve `Result<void>` e recebe também `lr_shader_blob_uncompressed_size` (o
  plano tinha `void register_entries(entries, blob)`); descomprime o blob uma vez, no registro, e confere as entradas;
  `spirv()` devolve `Result<std::vector<uint32_t>>`. O hash do microcódigo é calculado pelo `GpuSystem` (no proxy do
  backend), que guarda a primeira ocorrência de cada (hash, estágio) em `shader_stats()` e a escreve no trace da GPU.
  Hash ausente do cache é `guest_fatal` só quando o backend liga shaders (`Backend::binds_shaders`, falso por padrão);
  com o `NullBackend` é só informado (linha `missing` no trace). zstd: biblioteca nossa, `lr_zstd`, com os fontes de
  descompressão do submódulo zstd do XenosRecomp, preparada como `runtime/lib/liblr_zstd.a` e ligada pelo template;
  xxHash só em cabeçalho (`XXH_INLINE_ALL`), a cópia do XenonRecomp que o fork usa.
- Por quê: APIs públicas devolvem `Result` (AGENTS.md); o tamanho descomprimido já vem no cache gerado. Com o
  `NullBackend` nada é desenhado, então um shader ausente não muda o que roda, e o portão precisa contar todos os
  ausentes em vez de parar no primeiro. O alvo `libzstd_static` só existe quando o XenosRecomp é compilado (exige o
  DXC), e a `libzstd.so` do sistema seria mais uma dependência dinâmica do jogo (AppImage).
- Custo se errado: se o backend Vulkan da Tarefa 20 não sobrescrever `binds_shaders`, um shader ausente passa sem
  erro e o desenho sai sem shader; o teste `unknown hash is reported` mostra o comportamento esperado para os dois
  tipos de backend.

### M7. Portão dos shaders (Tarefa 15): reprovado, plano B
- O quê: medição no jogo real (2026-10-22). Tradução: `translated 64 of 85` (75,3%, abaixo dos 90%); 21 contêineres
  falharam: 15 pixel shaders com exportação de memória (`unsupported pixel shader export register 32`), 2 vertex
  shaders com `vfetch` sem elemento de vértice, 2 com texturas 1D (`g_Texture1DIndex`, erro do DXC) e 2 vertex shaders
  com amostragem de LOD implícito (erro do DXC: só permitida em fragment e compute). Sem falhas por microcódigo
  repetido com traduções diferentes; `literal-loops` somou 0. Como a ferramenta não escreve o cache quando algum
  contêiner falha, a cobertura foi medida contra os 64 hashes traduzidos do `shaders.log`, com o jogo compilado com um
  cache vazio e `NullBackend` (`--headless --trace-gpu --exit-after-swaps 300`, saída 0, 300 swaps): 1112 pacotes
  `IM_LOAD` e 902 `IM_LOAD_IMMEDIATE`, 7 microcódigos distintos (4 vertex, 3 pixel), 2 cobertos (os 2 pixel shaders
  carregados sem alteração), 5 ausentes (cobertura de 28,6%). Nenhum dos 5 ausentes é o microcódigo de um contêiner do
  XEX: 1 pixel shader e 1 vertex shader diferem do microcódigo de um contêiner em 1 e 2 dwords (no mesmo tamanho), e
  3 vertex shaders (15, 24 e 27 dwords) não correspondem a contêiner nenhum. Os hashes estão na seção do relatório da
  Tarefa 15. Decisão do plano: cobertura < 100% e tradução < 90%: parar e voltar ao controlador (plano B, tradutor
  próprio); a Parte E não começa.
- Por quê: o critério é o do plano. A medição mostra dois problemas diferentes: lacunas do tradutor (exportação de
  memória, texturas 1D, amostragem em VS, `vfetch` sem elemento) e, mais fundo, o próprio jogo (o D3D) altera o
  microcódigo antes de carregá-lo e carrega shaders que não estão em contêiner, então nenhum cache feito só a partir
  dos contêineres do XEX, com chave no hash do microcódigo carregado, cobre o que o jogo usa.
- Custo se errado: se os dwords alterados forem só campos de fetch que dá para normalizar antes do hash, e os 3 vertex
  shaders sem contêiner vierem de outro arquivo do disco, o plano A ainda serviria com mais trabalho no fork; trocar
  para o plano B sem conferir isso custa a escrita de um tradutor. A escolha entre os dois é do controlador.
