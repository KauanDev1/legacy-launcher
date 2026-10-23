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

### M8. Plano B: tradutor próprio de microcódigo para SPIR-V em tempo de execução (Tarefas 15.1 a 15.11)
- O quê: o M3 troca para o plano B do spec (seção 3.2), como manda o portão da Tarefa 15 (M7). Os shaders deixam de ser
  traduzidos na instalação. O runtime traduz, na primeira vez que vê, o microcódigo que o jogo carrega na GPU
  (`IM_LOAD`/`IM_LOAD_IMMEDIATE`). A chave é o hash do microcódigo carregado mais o estado de registradores que muda a
  tradução (`ShaderKey`), e nenhum metadado de contêiner entra nela. Os interpoladores ligam VS e PS pelo índice do
  registrador, não pela semântica do contêiner. O SPIR-V traduzido fica em `<raiz>/work/shaders/runtime/`, com a
  versão do tradutor no nome do arquivo, e nunca vai para o repositório. Consequências:
  - a etapa `shaders` sai de `default_stages`, e a instalação volta a ter sete etapas;
  - saem também o submódulo e o fork do XenosRecomp (`thirdparty/patches/xenosrecomp/`), o harness `tests/shaders`,
    o cache gerado (`lr_shader_cache*`, `lr_shader_blob*`) e a `lr_zstd`, que vinha do submódulo zstd do XenosRecomp
    (o cache em disco guarda SPIR-V sem compressão);
  - o DXC sai do projeto: do `doctor`, do `toolchain`, do contêiner e da Tarefa 24, que não precisa mais compilá-lo
    do fonte;
  - os shaders internos do renderizador (blit, limpeza, untile, resolve) passam de HLSL para GLSL, compilados para
    SPIR-V pelo `glslangValidator` do sistema **na compilação do launcher**. Eles viajam como bytes só dentro da
    `liblr_vulkan.a`, a única usuária deles. Nada compila shader na instalação ou no jogo. O `build.sh` do AppImage
    confere o `glslangValidator` e registra a versão;
  - a chave (`ShaderKey`) é: estágio, hash do microcódigo, tamanho e modificadores de 64 bits. Os modificadores do PS
    são o registrador `param_gen`, a flag de ponto e as máscaras flat e centroid de `SQ_INTERPOLATOR_CNTL`. O VS não
    tem modificadores no M3;
  - o contrato do runtime muda:
    - os endereços de dispositivo são lidos como `uvec2` com `OpBitcast`, sem exigir `shaderInt64`;
    - o bloco compartilhado passa a ter 736 bytes: `ndc_scale`/`ndc_offset`, flags do VTE, referência do alpha test,
      deslocamento e limites de índice;
    - `r0.x` do VS é o índice do vértice, com o deslocamento e o limite aplicados no shader;
    - temporários relativos a `aL` usam um banco de 64 registradores com índice limitado;
    - o `kill` liga uma flag e o `OpKill` fica só no fim, para preservar as derivadas;
  - partes do spec que deixam de valer:
    - §3.2 e §7: a etapa `shaders`, o cache gerado e o DXC na instalação;
    - §5.4: "formatos de vértice traduzidos para formatos Vulkan", porque o vertex fetch é feito no shader;
    - §5.4 e §6: o contrato do XenosRecomp (entradas de vértice pela tabela de elementos do contêiner, ligação por
      semântica);
    - §11: "casos novos do fork do XenosRecomp com contêineres sintéticos" vira testes de microcódigo sintético do
      tradutor;
    - §12, critério 4: "com o DXC embutido";
    - §12, critério 5: a etapa `shaders` em `pipeline.md`;
  - o SPIR-V do jogo é emitido por um construtor próprio e pequeno (`src/runtime/gpu/shader/spirv_builder.*`), que usa
    só os cabeçalhos do SPIRV-Headers (Khronos, licença no estilo MIT, submódulo fixado);
  - o `tests/support/xenos_shader_builder.h` continua: perde a montagem de contêineres e ganha os codificadores que
    faltam (saltos, chamadas, predicados, operações escalares, fetch com LOD);
  - o histórico do git guarda o fork (commits `0523eda`..`3b15c74`).
- Por quê:
  - O portão mostrou que o jogo carrega microcódigo que nenhum contêiner tem. São 2 variantes alteradas pelo D3D nos
    campos de `vfetch` e 3 vertex shaders sem contêiner. Nenhum cache feito a partir do XEX cobre isso, seja qual for o
    tradutor. Traduzir o que foi carregado cobre por construção.
  - O vertex fetch é feito dentro do shader, lendo de buffers por endereço de dispositivo. Formato, stride e offset vêm
    da própria instrução, que entra no hash, e o endereço e a troca de bytes vêm da fetch constant em tempo de
    execução. Assim as variantes alteradas viram chaves diferentes sem estado extra, e formatos sem `VkFormat`
    equivalente funcionam.
  - Para produzir SPIR-V, foram pesadas três opções:
    - (a) construtor próprio sobre o SPIRV-Headers: cerca de 1 000 linhas, sem biblioteca nova no binário do jogo e
      nada a compilar com o zig para glibc 2.31;
    - (b) o `spv::Builder` do glslang: BSD-3-Clause, e o `SpvBuilder.h` não depende do front-end. Usá-lo traz o
      submódulo inteiro do glslang para compilar 4 arquivos, num repositório com partes em outras licenças (a
      gramática gerada pelo bison, GPL-3.0 com exceção) que o ADR 0002 teria de separar;
    - (c) gerar GLSL e compilar com glslang/shaderc no jogo: o front-end inteiro (alguns MB) no binário, alguns
      milissegundos por shader e o mesmo problema de licenças.
  - A legibilidade, que é a vantagem de (c), vem por outro caminho: o desmontador do microcódigo e o dump de IR
    (`--dump-shaders`), mais `spirv-val` e `spirv-dis` do SPIRV-Tools, que só os testes usam no contêiner e que nunca
    são distribuídos.
  - O limite de 72 componentes do fork desaparece. Ele vinha de declarar `TEXCOORD0..15` + `COLOR0..1`. Com ligação
    por registrador, o VS declara as 16 saídas de interpolador (64 componentes, o mínimo garantido do Vulkan) e o PS
    lê só as que usa.
- Custo se errado:
  - Escrever o tradutor é o maior item do M3: cerca de 11 tarefas, semanas.
  - Uma semântica do Xenos errada aparece como desenho errado, não como falha de tradução. Os testes executam cada
    operação no lavapipe para pegar isso, mas só cobrem o que foi escrito neles.
  - Se o construtor próprio ficar caro de manter (controle de fluxo estruturado, depuração), a saída é trocar para o
    `spv::Builder` do glslang (opção b) atrás da mesma interface `SpirvBuilder`. O tradutor não muda.
  - Se o jogo usar exportação de memória (`eA`/`eM`) num shader carregado, o portão da Tarefa 15.11 para e uma
    tarefa nova a implementa.
  - Se o número de chaves distintas crescer sem limite, porque o D3D altera microcódigo a cada desenho, o custo vira
    tempo de tradução no primeiro quadro. O cache em disco só ajuda da segunda execução em diante. O portão mede
    isso pelas chaves novas nos últimos 100 swaps (`new_last_100_swaps`, na linha de resumo da saída).
  - O limite de 2^20 desvios para trás por invocação corta um laço legítimo de 4 níveis com 255 iterações cada.
    Isso é documentado, e o limite sobe se aparecer um caso real.

### M9. Portão do tradutor (Tarefa 15.11): aprovado
- O quê: medição no jogo real (2026-10-23), com o tradutor da versão 9 (`kTranslatorVersion`), `NullBackend` e
  `legacyrecomp run -- --headless --trace-gpu --exit-after-swaps 600`:
  - execução 1, com `work/shaders/runtime/` vazio: saída 0 depois de 600 swaps, cerca de 110 s com a sincronização
    do contêiner; `[gpu] shaders: keys=8 vs=3 ps=5 translated=8 disk=0 failed=0 new_last_100_swaps=0`;
  - execução 2, com o cache da primeira: saída 0 depois de 600 swaps;
    `keys=8 vs=3 ps=5 translated=0 disk=8 failed=0 new_last_100_swaps=0`, ou seja, 100% das chaves vieram do disco;
  - `tools/shader_cache_stats.sh` (novo; `spirv-val --target-env vulkan1.2` sobre o SPIR-V de cada arquivo do cache,
    depois do cabeçalho `LRSV` e do microcódigo): 8 arquivos, 8 válidos, 0 inválidos (3 VS, 5 PS);
  - 7 microcódigos distintos carregados (4 VS, 3 PS), exatamente os 7 hashes do M7. As 8 chaves são 3 VS e 5 PS:
    um mesmo PS de 9 dwords aparece com três modificadores (`0xFF`, `0x1000000FF` e `0x1000100FF`: nenhuma máscara,
    centroide em `r0`, e centroide mais flat em `r0`), e outro PS tem flat em `r0`;
  - um dos 7, o VS `0xAB1E86137A0240E8` (15 dwords), é carregado mas nunca usado num desenho: o jogo carrega outro
    VS antes do próximo desenho, então a regra "o último carregado é o ativo" não gera chave para ele. Uma sonda só
    do portão (não commitada) traduziu cada microcódigo no `IM_LOAD`: os 7 traduziram e passaram no `spirv-val`
    (10 arquivos em outra pasta de cache, todos válidos);
  - todas as chaves aparecem até o swap 40; nenhuma chave nova nos últimos 100 swaps, então o critério (c) da regra
    de parada não se aplica e a execução com 900 swaps não foi necessária;
  - nenhuma rodada de correção: 0 falhas na primeira execução. Nenhum shader carregado usa exportação de memória.
- Por quê: os critérios do plano (0 `failed`, 0 erros do `spirv-val`, os 7 hashes do M7 traduzidos, segunda execução
  toda do disco) foram cumpridos. O teste de integração `the game's loaded shaders all translate` (ISO, sem
  interface, `--exit-after-swaps 300`) guarda o resultado: 0 linhas `failed` no trace e `failed=0` no resumo.
- Custo se errado: 600 swaps cobrem só as telas iniciais. Os 15 contêineres do XEX com exportação de memória (M7)
  e os shaders do mundo ainda não foram carregados; se algum deles chegar com `eA`/`eM` ou outra função fora das
  Tarefas 15.4 a 15.9, a tradução falha (com o backend Vulkan, saída 5 nomeando o shader) e uma tarefa nova a
  implementa. Os modificadores do PS já dividem um microcódigo em três chaves; se isso crescer dentro do jogo, o custo
  é tempo de tradução no primeiro uso, visível em `new_last_100_swaps`.

### M10. Apresentação Vulkan (Tarefa 17): cópia linear do front buffer, `--gpu`, sonda à parte e saída ordenada
- O quê:
  - O `swap()` do `VkBackend` lê o front buffer como superfície **linear** de A8R8G8B8 big-endian, não como a
    superfície em tiles que o console usa (o plano diz "cópia destilada"). O destile e o formato do fetch constant
    dependem do caminho de texturas da Tarefa 19, que substitui essa cópia. O jogo falso pinta o front buffer de uma
    cor só, então o teste `clear-color frame is captured` independe do layout.
  - `--gpu vulkan|null` no jogo, com `vulkan` como padrão, também com `--headless` (o spec manda a GPU funcionar sem
    interface). `null` mantém o `NullBackend` para os testes do jogo falso que não testam o Vulkan, e para quem quer
    rodar sem GPU. `--screenshot-after` exige `vulkan`.
  - O `doctor` roda `tools/legacyrecomp-vkprobe`, um executável pequeno, em vez do `legacyrecomp-game --probe-vulkan`:
    o jogo ainda não existe quando o `doctor` roda pela primeira vez. A sonda roda num processo à parte (um driver
    que trava não derruba o launcher, e as alocações do driver não aparecem para o LeakSanitizer do build `dev`). A
    falta de Vulkan é **aviso**, não erro: a instalação funciona numa máquina sem GPU (o contêiner sem lavapipe, por
    exemplo), e o jogo é que se recusa a iniciar com saída 2.
  - SIGTERM, SIGINT e SIGHUP passam a sair com 0 de forma ordenada (thread vigia com `sigwait`; para a GPU, destrói o
    Vulkan, sai). Antes, o sinal matava o processo com o código do sinal. Ctrl+C no terminal também sai com 0.
  - Os cabeçalhos copiados para `runtime/include` incluem `volk.h` e os cabeçalhos C do Vulkan-Headers que ele usa,
    para que os `runtime/gpu/vk/*.h` copiados compilem sozinhos. O `main.cpp` só inclui `vk_backend.h`, que não tem
    tipos do Vulkan. O teste `template_compiles_with_staged_headers` também compila cada um desses cabeçalhos e liga
    o jogo falso com as três bibliotecas copiadas (`liblr_runtime_core.a`, `liblr_vulkan.a`, `libXenonUtils.a`).
- Por quê: cada ponto fecha um item da tarefa sem esperar as Tarefas 18 a 21 e sem mudar códigos de saída.
- Custo se errado: a cópia linear mostra lixo listrado no jogo real até a Tarefa 19; isso é esperado e não deve ser
  lido como erro de apresentação. Quem depender do código do sinal na saída (nada no projeto depende) passa a ver 0.
  O modo com janela (swapchain) não roda no contêiner, que não tem servidor gráfico. Ele só foi compilado, e a
  conferência fica para o ponto de controle com WSLg.

### M11. Texturas (Tarefa 19): destile na CPU, hash por textura, `Result` no `get` e front buffer pelo fetch constant
- O quê:
  - O destile, a troca de endian e a conversão das texturas rodam na CPU (`runtime/gpu/texture_format.cpp`), não num
    compute shader como dizia o spec (5.4). O placeholder `untile.comp` saiu.
  - Invalidação por hash XXH3-64 dos bytes que cada textura cobre, comparado no primeiro uso de cada quadro, mais
    `invalidate_range` para o que o resolve da EDRAM escreve; não por proteção de página.
  - `TextureCache::get` devolve `Result<VkImageView>` (o plano escreve `VkImageView`): formato não suportado, sinal,
    textura fora da memória ou grande demais são erros que nomeiam a textura, e as APIs públicas devolvem `Result`.
  - `host_format(XenosFormat)` fica em `runtime/gpu/vk/textures.h`, não em `texture_format.h`: o núcleo da GPU não tem
    tipos do Vulkan. O núcleo descreve o layout do host com `HostLayout`.
  - `Backend::swap` recebe o arquivo de registradores (`swap(regs, phys, w, h)`): o front buffer usa o formato, o
    tiling, o endian, o pitch e o swizzle do fetch constant 0 que o `VdSwap` escreve. O jogo falso agora descreve um
    front buffer em tiles com um padrão por pixel; o teste `clear-color frame is captured` virou
    `tiled front buffer frame is captured`.
  - Mips empacotados implementados (não ficaram como limite). Formatos com sinal são erro até o jogo precisar.
    `samplerAnisotropy` e `samplerMirrorClampToEdge` são ligados quando o dispositivo os tem, sem entrar no piso.
- Por quê: o hash exige ler os bytes na CPU de qualquer jeito, e assim toda leitura guiada pelo convidado é conferida
  num lugar só e testável sem dispositivo; proteção de página teria de conviver com o código recompilado escrevendo na
  memória e com o handler de falhas (saída 4).
- Custo se errado: texturas grandes custam tempo de CPU no envio e no hash de cada quadro em que são usadas (8192×8192
  RGBA8 são 256 MiB); uma escrita da CPU entre dois draws do mesmo quadro só aparece no quadro seguinte. Se isso pesar,
  o destile pode ir para um compute shader e o hash para menos vezes por quadro sem mudar a interface do cache.

### M12. Fluxo de comandos (Tarefa 20): um command buffer enviado em pontos de sincronização e esperado na hora
- O quê: draws, limpezas e resolves da EDRAM e a inicialização de render targets vão para um único command buffer
  (`CommandStream`, `runtime/gpu/vk/stream.h`), enviado no resolve, no swap, no `wait_idle`, antes do trabalho
  próprio do cache de texturas e quando o anel de envio enche. Cada envio espera a fence na hora; não há quadros em
  voo. Destruições que comandos gravados podem usar passam por `CommandStream::defer`, e o cache de texturas envia o
  fluxo (gancho `set_before_gpu_work`) antes de enviar, destruir ou regravar descritores, em vez de
  `UPDATE_UNUSED_WHILE_PENDING` com reciclagem adiada de índices.
- Por quê: o resolve precisa dos bytes na CPU quando volta, e o jogo resolve pelo menos uma vez por quadro; com o
  envio esperado, nada fica pendente quando o host muda descritores ou destrói objetos, e a validação não tem o que
  apontar. É o lado "correção primeiro" que a Tarefa 20 pede.
- Custo se errado: a CPU espera a GPU a cada resolve e a cada swap (sem sobreposição entre quadros). Se o desempenho
  pedir, o fluxo ganha vários command buffers com fences próprias, um anel por quadro e reciclagem de índices pela fence,
  sem mudar a interface de `defer`/`flush`.
