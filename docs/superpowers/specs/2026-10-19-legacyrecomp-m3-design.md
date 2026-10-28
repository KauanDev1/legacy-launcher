<!-- SPDX-License-Identifier: MIT -->
# LegacyRecomp — Marco 3: Gráficos (GPU Xenos em Vulkan)

- **Data:** 2026-10-19
- **Status:** Escrito sem rodada de perguntas (pedido do dono); aberto à revisão dele
- **Escopo:** Marco 3 (M3) do LegacyRecomp
- **Antecessor:** M2 (`docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`), concluído: o jogo inicializa
  sem tela e para em `VdInitializeEngines`

## 1. Objetivo e direção

A direção continua sendo o **Minecraft: Xbox 360 Edition jogável por completo no Linux** (M5). O M3 é o marco que
coloca imagem na tela: o jogo passa a desenhar numa janela SDL com Vulkan.

**Ponto de chegada do M3:** `legacyrecomp run` abre uma janela e o jogo desenha a sequência de abertura e a **tela
título** (panorama e menu) com texturas e shaders de verdade, a 60 Hz, sem erros da camada de validação do Vulkan,
e continua rodando até o usuário fechar a janela. Sem controle (M4) o menu não responde; isso é esperado.
Verificação automática: `legacyrecomp-game --screenshot-after <N> <arquivo.png>` grava o quadro N e sai com 0; o
teste de integração compara esse quadro com critérios objetivos (não vazio, não uma cor só, proporção de pixels
"de interface" acima de um limiar) — nunca com uma imagem do jogo guardada no repositório.

Marco intermediário dentro do M3 (**M3a**): a GPU recebe comandos de verdade, o jogo não mostra mais a caixa de erro
de inicialização e a janela apresenta os quadros do `VdSwap` (mesmo que só com a cor de limpeza).

## 2. O que já sabemos (experimento de 2026-10-18 com o jogo real)

Com stubs temporários para as chamadas de GPU, o jogo:

- passa de `VdInitializeEngines` e faz ~73 400 chamadas de importação em 10 threads, **sem nunca chamar `VdSwap`**;
- configura o ring buffer (`VdInitializeRingBuffer(0x1FFF4000, log2 12)`, write-back do ponteiro de leitura em
  `0x1FFFD03C`), registra o callback de interrupção gráfica e cria 9 threads;
- trava a inicialização esperando algo da GPU (uma thread estoura o tempo de espera 5 vezes num objeto) e acaba
  chamando `XamShowMessageBoxUI` e `XamLoaderLaunchTitle` (caixa de erro e "reinicie o jogo"). Hipótese: o ponteiro
  de leitura do ring buffer nunca avança e nenhuma interrupção de vblank chega;
- precisa de `KeCertMonitorData` (variável que falta: falha logo após a GPU) e de áudio que **finja funcionar**
  (`XAudioRegisterRenderDriverClient` com falha leva a `KeBugCheck`);
- antes disso tudo, **três execuções caíram por falhas do recompilador**, não do kernel: jump tables do tipo
  "deslocamento de meia palavra (`lhzx`) + `add`" que o `analyse` não reconhece (há ~61 sítios `lhzx` e ~69 `lbzx`
  com `bctr` indexado, mais 21 sem guarda) e um grupo de funções minúsculas sem `.pdata` chamadas por ponteiro;
- tem **~70 shaders pré-compilados dentro do XEX** (45 contêineres de vertex shader, 25 de pixel shader) e não abre
  arquivo de dados até ali; os arquivos do jogo são PNG soltos, bancos de áudio XACT, dois `.xzp` (XUI) e pacotes PIRS.

Os stubs do experimento não foram guardados (o experimento era descartável); os dados que o plano precisa ficam aqui:

- jump tables não reconhecidas: `0x82AB1528` (35 casos, dentro de `sub_82AB0C68`) e `0x8294E434` (17 casos, já listada
  como "sem guarda" no `analyse.log`); funções minúsculas sem `.pdata` chamadas por ponteiro em
  `0x82A12650`–`0x82A127F0` (uma é comparador de `qsort`);
- valores que bastaram para passar da GPU: `VdInitializeEngines` → 1, `VdGetSystemCommandBuffer` escreve
  `0xBEEF0000`/`0xBEEF0001`, `VdPersistDisplay` → 1, demais `Vd*` → 0; `VdGlobalDevice`/`VdGlobalXamDevice` = 0,
  `VdGpuClockInMHz` = 500, `VdHSIOCalibrationLock` como seção crítica; áudio: `XAudioRegisterRenderDriverClient`
  escreve o handle `0x41550000`;
- argumentos observados: `VdSetGraphicsInterruptCallback(0x824371B0, 0x40055980)`,
  `VdInitializeEngines(0x52B50000, 0x82445B00, …)`; a thread que estoura o tempo espera o objeto `0x400587CC`.

## 3. Decisões

Todas tomadas pelo agente a partir da documentação, das pesquisas e do experimento; o dono pediu que o spec fosse
escrito sem rodada de perguntas. Cada uma fica aberta à revisão dele.

| Tema | Decisão |
|------|---------|
| Como desenhar | **Emular a GPU** (estilo Xenia, implementação própria): ring buffer, processador de comandos PM4, registradores do Xenos, EDRAM como render targets do host, cache de texturas. O código D3D do jogo roda recompilado, sem ganchos por endereço |
| Shaders | **XenosRecomp na instalação**, a partir dos contêineres que estão no XEX; em tempo de execução o microcódigo que o jogo envia à GPU é identificado por hash e ligado ao SPIR-V já traduzido. Sem tradutor de microcódigo em tempo de execução no M3 |
| API gráfica | **Vulkan 1.2** próprio (sem camada RHI), carregado com **volk** a partir do `libvulkan.so.1` do sistema; memória com **VMA**; janela e superfície pelo SDL2 já usado |
| Recompilador primeiro | Antes de qualquer gráfico: `analyse` reconhece jump tables `lhzx`/`lbzx` e funções sem `.pdata` chamadas por ponteiro |
| Áudio no M3 | Stubs que "funcionam em silêncio" (registro de cliente de áudio com sucesso, volumes fixos), não "recurso ausente" |
| Ponto de chegada | Tela título desenhada a 60 Hz (seção 1), com captura de quadro para o teste |

### 3.1 Por que emular a GPU e não substituir as funções D3D

A alternativa (modelo do UnleashedRecomp) troca as funções da biblioteca D3D que o jogo carrega estaticamente por
ganchos que chamam o renderizador do host. Para este jogo ela exigiria achar, sem símbolos e sem Ghidra (que o
projeto ainda não usou), 50 a 80 funções de uma biblioteca D3D de 2011-2012 e o layout interno do dispositivo, que
muda com a versão do XDK. Muito estado é escrito por código inline, e qualquer caminho de desenho que contorne os
ganchos (a interface do jogo pode usar um renderizador próprio) falha em silêncio. Emular a GPU começa exatamente onde
o M2 parou (importações com nome), não depende de endereços do jogo e cobre qualquer caminho de desenho. O custo é
mais engenharia (processador de comandos, EDRAM, texturas), em troca de quase nenhuma engenharia reversa específica.

### 3.2 Por que XenosRecomp e não um tradutor próprio

O roadmap prevê o XenosRecomp, e o experimento mostrou que os shaders estão no XEX como contêineres que ele sabe ler.
Traduzir na instalação mantém o executável simples e reaproveita um tradutor existente (MIT). O ponto delicado é que o
XenosRecomp foi feito para o modelo de ganchos D3D; por isso:

- usamos um **fork** com um define próprio (`LR_*`) no lugar dos caminhos específicos do Unleashed, mantido do mesmo
  jeito que o XenonRecomp (patches no repositório, aplicados numa cópia na configuração; ADR 0004 M7);
- o **contrato de ligação** (constantes, texturas, entradas de vértice, flags de especialização) é adaptado ao que a
  GPU emulada conhece: constantes vêm do arquivo de registradores, fetch de vértices e de texturas vem dos fetch
  constants (seção 6);
- a ligação em tempo de execução é por **hash do microcódigo**: a etapa de instalação registra, para cada contêiner,
  o hash do trecho de microcódigo; quando o jogo carrega um shader na GPU (pacotes `IM_LOAD`/`IM_LOAD_IMMEDIATE`), o
  runtime calcula o hash do que foi carregado e procura no cache. Shader sem entrada no cache é um erro fatal claro
  (saída 5) com o hash, nunca um desenho errado em silêncio;
- **portão de decisão no início do M3** (decisão do agente): antes do renderizador, uma tarefa roda o XenosRecomp do
  fork sobre o XEX real e, com a GPU emulada mínima (M3a), confere que todo microcódigo carregado pelo jogo até a
  tela título tem hash no cache. Se mais de ~10% dos shaders não traduzirem, ou se aparecer microcódigo que não está
  em nenhum contêiner (shaders internos do D3D ou montados em tempo de execução), o M3 troca para o **plano B**: um
  tradutor próprio de microcódigo para SPIR-V em tempo de execução, usando o Xenia como referência de semântica. A
  troca é registrada em ADR e não exige um spec novo; o resto do M3 (GPU emulada, EDRAM, texturas) não muda;
- enquanto o plano A vale, microcódigo sem entrada no cache é erro fatal com o hash.

**Compilador de shaders (DXC):** o XenosRecomp gera HLSL e usa o DXC para chegar a SPIR-V. O DXC (licença NCSA, com
SPIRV-Tools Apache-2.0) entra como ferramenta da instalação (decisão do agente): **release oficial fixado** do
repositório microsoft/DirectXShaderCompiler para Linux, baixado e conferido por SHA-256 como os outros itens do
`toolchain.lock`, copiado para `share/legacyrecomp/tools/`. Ele é uma exceção documentada à regra "todo ELF do AppImage
compilado pelo zig" (como o runtime type2); o piso de glibc dele é medido com `readelf` e registrado. Se o piso
passar de 2.31 ou o release não servir, o plano B é compilar do fonte. Na instalação a partir do fonte, o `doctor`
aceita o DXC do sistema (`directx-shader-compiler`). As licenças entram no `THIRD-PARTY-LICENSES`. O jogo final não
depende do DXC.

## 4. Recompilador antes dos gráficos

- O scanner de jump tables (`src/pipeline/switch_scan.cpp`) passa a reconhecer as formas "tabela de meia palavra/byte
  com deslocamento relativo + `add` + `mtctr` + `bctr`" (`lhzx`/`lbzx`), com os mesmos critérios de segurança das
  formas atuais (limite por `cmplwi`/máscara, alvos dentro da função).
- Funções sem `.pdata` alcançadas só por ponteiro: o `analyse` passa a propor funções a partir de (a) endereços de
  código escritos como constantes em tabelas de dados e (b) alvos de `bl` ainda não cobertos; cada proposta é validada
  pelas mesmas regras do `fit_switch_functions` (sem sobreposição, sem desvio de fora para o meio).
- Defesa em profundidade: chamada indireta para endereço sem função recompilada passa a gerar um relatório com o
  endereço do convidado (saída 4 com "indirect call to 0x… without a recompiled function") em vez de salto para o
  endereço 0 do host.
- Critério: as três falhas do experimento somem sem entradas manuais no `recomp.toml`; o `analyse.log` lista quantos
  sítios novos foram resolvidos.

## 5. GPU emulada

### 5.1 Importações e variáveis

- As 18 importações `Vd*` restantes e `MmMapIoSpace` (catálogo em `docs/reverse-engineering/kernel-imports.md`):
  `VdInitializeEngines`, `VdShutdownEngines`, `VdSetGraphicsInterruptCallback`, `VdInitializeRingBuffer`,
  `VdEnableRingBufferRPtrWriteBack`, `VdGetSystemCommandBuffer`, `VdSetSystemCommandBuffer`,
  `VdSetSystemCommandBufferGpuIdentifierAddress`, `VdInitializeScalerCommandBuffer`, `VdSwap`, `VdPersistDisplay`,
  `VdRetrainEDRAM`, `VdRetrainEDRAMWorker`, `VdCallGraphicsNotificationRoutines`, `VdSetDisplayMode`,
  `VdSetDisplayModeOverride`, `VdEnableDisableClockGating`, `VdQueryVideoMode` (já stub).
- Variáveis: `VdGlobalDevice`, `VdGlobalXamDevice`, `VdGpuClockInMHz` (500), `VdHSIOCalibrationLock`,
  `KeCertMonitorData`.
- Semântica de referência: Xenia (`xboxkrnl_video.cc`), só como referência.

### 5.2 Ring buffer, write-back e interrupções

- Uma thread do host ("GPU") consome o ring buffer: lê o ponteiro de escrita que o jogo atualiza (o registrador
  `CP_RB_WPTR`, que o código recompilado escreve como memória comum na faixa reservada `0x7F000000`–`0x7FFFFFFF`,
  porque o XenonRecomp não emula MMIO), processa os pacotes até ele e escreve o ponteiro de leitura no endereço de
  write-back (em big-endian, no endereço físico convertido para o endereço do convidado).
- Leituras de registradores de GPU pelo jogo: a faixa MMIO tem valores preenchidos de forma consistente (status
  "ocioso", contadores); se o jogo depender de uma leitura que muda, o plano B é emular a leitura pelo handler de
  falhas com a página protegida.
- Interrupções: um temporizador de 60 Hz chama o callback de interrupção gráfica do jogo como uma interrupção de
  vblank (pela thread de trabalho do kernel, como um DPC: `call_guest`), e pacotes de interrupção do PM4 geram as
  interrupções de comando. Fences (`EVENT_WRITE`, `MEM_WRITE`) escrevem os valores na memória do convidado na ordem
  em que os pacotes são processados.

### 5.3 Processador de comandos e registradores

- Pacotes PM4 tipos 0, 2 e 3; os opcodes do tipo 3 que o jogo usa (no mínimo: `ME_INIT`, `NOP`, `INDIRECT_BUFFER`,
  `WAIT_REG_MEM`, `REG_RMW`, `COND_WRITE`, `EVENT_WRITE*`, `MEM_WRITE`, `SET_CONSTANT`, `SET_CONSTANT2`,
  `LOAD_ALU_CONSTANT`, `SET_SHADER_CONSTANTS`, `IM_LOAD`, `IM_LOAD_IMMEDIATE`, `DRAW_INDX`, `DRAW_INDX_2`,
  `INVALIDATE_STATE`, `VIZ_QUERY`, `SET_BIN_*` ignorados). Opcode desconhecido: erro fatal com nome e contagem
  (`--trace-gpu` registra cada pacote).
- Arquivo de registradores do Xenos (tabela própria com nomes e índices, conferida contra a documentação pública e o
  Xenia como referência).
- O jogo escreve endereços físicos nos comandos: conversão física → endereço do convidado pela tabela do
  `PhysicalHeap` (cada página física pertence a uma vista; decisão do M2). Se aparecer acesso pela mesma página em
  duas vistas, entra o espelhamento com memfd (contingência já prevista no M2).

### 5.4 Renderizador Vulkan

- Janela SDL (a do `main.cpp` já existe) com superfície Vulkan; swapchain FIFO; o `VdSwap` apresenta o front buffer
  indicado.
- **EDRAM**: render targets do host por (base EDRAM, pitch, formato, MSAA); "resolve" (cópia EDRAM → memória) feito
  por shaders de cópia que escrevem na memória do convidado no layout em tiles do Xenos, com troca de bytes. Sem o
  caminho preciso de interlock por pixel no M3 (fica para quando um efeito precisar).
- **Texturas**: cache por (endereço, formato, dimensões, tiling); destile e troca de bytes em compute shader; formatos
  DXT1/3/5 → BC1/2/3, DXN → BC5, CTX1 decodificado; invalidação quando a memória do convidado muda (proteção de
  página ou hash por quadro — decisão do construtor, documentada).
- **Vértices e índices**: lidos da memória do convidado pelos fetch constants, com troca de bytes; formatos de vértice
  traduzidos para formatos Vulkan.
- **Estado**: blend, profundidade/estêncil, rasterização, viewport/scissor e meio pixel vindos dos registradores;
  pipelines em cache por hash do estado.
- Requisitos mínimos de GPU do host: Vulkan 1.2, `VK_KHR_swapchain`, `textureCompressionBC`, descriptor indexing e
  buffer device address (exigidos pelo contrato do XenosRecomp). O `doctor` passa a verificar isso (com uma mensagem
  clara para quem não tem Vulkan).
- `--headless` continua existindo: a GPU processa comandos e resolve normalmente, só não apresenta. Os testes de
  integração rodam assim no contêiner (Mesa lavapipe, renderização por software, para não depender de GPU do host).

## 6. Contrato entre a GPU emulada e os shaders traduzidos

Partindo do contrato do XenosRecomp, adaptado (o fork registra cada mudança):

- **Constantes**: buffers com as constantes float de VS e PS montados a partir do arquivo de registradores (já em
  little-endian), endereçados por buffer device address como o XenosRecomp espera; booleanos (128, não só 16) e
  inteiros (loops) num buffer próprio.
- **Texturas e samplers**: bindless; o índice de cada slot de fetch vem dos fetch constants de textura.
- **Vértices**: entradas de vértice derivadas dos fetch constants de vértice e da tabela de elementos do contêiner;
  sem a tabela fixa de locais do Unleashed.
- **Especialização**: alpha test com todas as funções de comparação do Xenos, meio pixel por registrador.
- O que o XenosRecomp não implementa e o jogo usar (integer constants, indexação dinâmica, vfetch explícito, LOD de
  textura) é implementado no fork, com teste.

## 7. Pipeline de instalação

- Nova etapa **`shaders`** entre `recompile` e `build`: entrada `work/image/default_tu1.xex`; executa o XenosRecomp
  do fork (via `posix_spawn`) num diretório temporário com o XEX; saída `work/generated/shader_cache.cpp` (SPIR-V
  comprimido + tabela hash do microcódigo → entrada) e `work/logs/shaders.log` com: contêineres encontrados,
  traduzidos, falhas do DXC. **Falha de tradução de qualquer shader falha a etapa** (o XenosRecomp hoje engole erros;
  a etapa confere "contêineres encontrados = entradas geradas").
- Fingerprint: hash da imagem + commit do XenosRecomp + hash dos patches + versão do DXC.
- O cache gerado é derivado do jogo: vive só em `work/`, nunca no repositório.

## 8. Áudio, mensagens e outros stubs para chegar à tela título

- `XAudioRegisterRenderDriverClient` (sucesso, handle fixo), `XAudioGetVoiceCategoryVolume`, `XAudioUnregister…` e o
  que o rastreio mostrar, como stubs silenciosos: o motor de áudio do jogo roda, mas nada toca (som é o M4).
- `XamShowMessageBoxUI`: implementado com saída no terminal (texto da mensagem e botões no log) e resposta "primeiro
  botão", para que um erro do jogo apareça como texto legível no relatório, não como uma janela que não existe.
- `XamLoaderLaunchTitle`: termina com relatório (saída 5) dizendo que o jogo pediu para reiniciar.
- O resto aparece pelo laço "rodar, ver onde para, implementar" do M2.

## 9. Diagnóstico

- `--trace-gpu`: um arquivo com cada pacote PM4 (tipo, opcode, tamanho) e cada desenho (contagem, formatos), com
  limite de tamanho.
- `--screenshot-after <N> <png>` (seção 1).
- Camada de validação do Vulkan ligada nos testes de integração; qualquer erro de validação falha o teste.
- Contadores por quadro (desenhos, resolves, texturas criadas) no log a cada segundo com `--trace-gpu`.

## 10. Ambiente de desenvolvimento

- O contêiner ganha Mesa (lavapipe), `vulkan-icd-loader`, `vulkan-validation-layers` e as dependências de build do DXC.
- Janela de verdade (decisão do dono, 2026-10-19): **WSLg** neste Windows. Uma tarefa do plano instala uma distro
  Linux no WSL (Arch ou Ubuntu), confere Vulkan pela GPU do Windows (Mesa `dzn`, sobre D3D12) e documenta como rodar o
  AppImage ali com janela. Nos pontos de controle, o agente roda o jogo com janela no WSLg e registra o que viu
  (captura de tela local, nunca no repositório) e os quadros por segundo. Se o WSLg não tiver Vulkan utilizável nesta
  GPU, o fallback é renderização por software (lavapipe) na mesma janela, só para conferência visual, e os 60 Hz
  ficam para uma máquina Linux com GPU. Os testes automáticos não dependem do WSLg.

## 11. Testes

- **Unitários**: parser PM4 com fluxos sintéticos; arquivo de registradores; conversão física → convidado; destile e
  troca de bytes de texturas (comparação com implementação de referência simples em CPU); formatos de vértice; scanner
  de jump tables `lhzx`/`lbzx` com o codificador sintético existente; casos novos do fork do XenosRecomp com
  contêineres sintéticos montados pelos testes.
- **Jogo falso**: um "jogo" escrito à mão que inicializa a GPU, envia um ring buffer com limpeza de tela e `VdSwap`, e
  verifica a imagem capturada (headless, lavapipe).
- **Integração (ISO real)**, em dois níveis (decisão do agente): o **curto** (rotina, a cada tarefa de GPU) para no
  primeiro `VdSwap` com imagem não vazia; o **completo** (nos pontos de controle do plano e no aceite) chega à tela
  título, passa nos critérios objetivos da seção 1, sem erros de validação, e roda 600 quadros sem travar — pode levar
  dezenas de minutos no lavapipe.
- **60 Hz** é meta medida e registrada, não critério de aprovação automática. Em execução headless, o vblank é
  cadenciado pelos quadros apresentados (um vblank por `VdSwap` processado, com um mínimo de tempo real), para que um
  renderizador por software lento não provoque esperas estouradas; com janela, o vblank segue o relógio real (60 Hz).

## 12. Critérios de aceite do M3

1. `legacyrecomp install` + `legacyrecomp run` abrem a janela e mostram abertura e tela título; o teste de integração
   com `--screenshot-after` passa no contêiner (lavapipe).
2. As falhas de recompilação do experimento somem sem entradas manuais.
3. Nenhum erro de validação do Vulkan nos testes.
4. O AppImage faz o mesmo (com o DXC embutido), numa máquina sem compiladores no PATH.
5. Docs: `architecture.md` (GPU, EDRAM, contrato dos shaders), `pipeline.md` (etapa `shaders`), ADR 0005 com os
   desvios do M3, `roadmap.md`, READMEs (status), cobertura regenerada.

## 13. Fora do escopo do M3

Som de verdade, controle, saves (M4); rede e LIVE; interface do sistema; escala de resolução, filtros e melhorias
visuais; caminho preciso de EDRAM por interlock; tradutor de microcódigo em tempo de execução (só como plano B).

## 14. Riscos

| Risco | Mitigação |
|-------|-----------|
| Mais falhas do recompilador além das três vistas | Seção 4 antes dos gráficos; relatório claro de chamada indireta sem função |
| O fork do XenosRecomp não cobre os shaders do jogo | Etapa `shaders` falha alto; plano B: tradutor próprio (ADR) |
| Hash do microcódigo carregado não bate com o do contêiner | Teste com o jogo falso; o hash é calculado sobre o mesmo trecho; se divergir, chave alternativa por (tamanho, primeiras palavras) |
| Leituras de registradores MMIO que precisam mudar | Plano B por proteção de página e handler |
| Vistas físicas sem espelhamento | Detectável; memfd como contingência |
| DXC pesado no AppImage | Compilado do fonte fixado; só a biblioteca necessária; medir o tamanho |
| Hipótese da caixa de erro errada | O texto da caixa (seção 8) diz o motivo na primeira execução |
| Cópia acidental de código GPL (UnleashedRecomp) | Só leitura; ADR registra a disciplina; nada de trechos |
| Áudio e threads exercitados pela primeira vez | Laço do M2 com testes; ADR 0004 lista as decisões ainda não confirmadas |
