<!-- Rascunho da entrada M8 de docs/adr/0005-m3-plan-deviations.md. A Tarefa 15.1 do plano a copia para o ADR e
     faz o commit junto com a remoção da etapa shaders. -->

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
