# Estado do M3 (GPU)

Documento vivo: o que o M3 já entregou, onde está agora, as decisões tomadas pelo caminho e o que ficou em aberto.
O plano é `docs/superpowers/plans/2026-10-19-legacyrecomp-m3.md` (com a emenda do plano B depois da Tarefa 15), o
spec é `docs/superpowers/specs/2026-10-19-legacyrecomp-m3-design.md` e os desvios ficam em
`docs/adr/0005-m3-plan-deviations.md` (M1 a M13). Este arquivo junta o contexto que estava espalhado nos relatórios
de cada tarefa.

Branch: `feat/m3` (não publicada). Todas as tarefas passaram por revisão independente antes de serem dadas como
concluídas.

## Onde estamos (2026-10-28)

O jogo real roda sem janela até 600 quadros. Os 11 shaders que ele carrega traduzem todos, e a camada de validação
do Vulkan não acusa erro. **A imagem final ainda sai com uma cor só.** A Tarefa 22 (chegar à tela de título) investiga
três causas independentes:

| | Problema | Situação |
|---|---|---|
| a | As duas texturas de fonte (`k_8`, 190×170 e 128×128) ficam zeradas na memória do convidado; nenhuma cópia de EDRAM nem shader com memexport escreve nelas | em investigação |
| b | O vertex shader `CB7E755E3A72E353` (as duas faixas de triângulos) chega quase todo zerado | em investigação |
| c | O vertex shader `E91536F60A69ABF3` (logo e botões) gera coordenadas de textura erradas com entradas corretas (dados de vértice e constantes c10/c11 conferidos) | próximo passo |

O que já foi descartado:
- a hipótese de `dcbz`/`dcbz128` zerando a linha errada (a semântica do Xenon está fixada por teste, `67f20e8`);
- retângulo de resolve, chaves de render target, visibilidade dos descritores, tamanho e deslocamento do fetch de
  vértice, aliasing entre as vistas físicas, arquivos faltando e importações falhando.

Ferramentas novas para a investigação (código nosso, commitado):
- `tools/frame_stats.py`: estatísticas do quadro (cores distintas, luminância média, fração de bordas) sem abrir a
  imagem;
- vigia de escrita no convidado (`src/runtime/write_watch.*`): mostra quem escreve num intervalo de memória,
  separando escritas de zero das de dados e registrando alocações e liberações físicas.

## Tarefas

| Tarefa | Conteúdo | Commits |
|---|---|---|
| 1–4 | Recompilador: jump tables relativas, funções só por ponteiro, relatório de chamada indireta sem função, falhas do experimento resolvidas | `8c59895`..`87ec3a5` |
| 5 | Vulkan no contêiner e no build (volk, VMA, Vulkan-Headers, `vk::Device`) | `ec5a3ac`, `fa421eb` |
| 6 | Submódulo XenosRecomp, mecanismo genérico de patches, DXC do sistema (aposentado depois, ver 15.1) | `10e73aa`..`8e067e1` |
| 7–9 | Registradores do Xenos, processador PM4, tradução física→convidado e janela MMIO | `8d556b6`..`5b483cc` |
| 10 | `GpuSystem`: ring buffer, write-back, interrupções e as importações `Vd*` | `2ffff53`, `ac0e9bc` |
| 11 | Áudio mudo, caixa de mensagem no terminal, `XamLoaderLaunchTitle` fatal | `c6b111a` |
| 12 | [ISO] M3a: o jogo alimenta a GPU por 757 quadros | `e5e0851`..`af19927` |
| 13–15 | Fork do XenosRecomp, etapa `shaders`, portão do plano A, **que reprovou** | `0523eda`..`a17bae3` |
| 15.1 | Aposentadoria da tradução na instalação, do XenosRecomp e do DXC | `34647d9` |
| 15.2–15.10 | Tradutor próprio microcódigo → SPIR-V (plano B), com cache em disco | `4f8a91d`..`4030e2e` |
| 15.11 | [ISO] Portão do tradutor: 8 chaves, 0 falhas, 100% de acertos no cache na segunda rodada | `b81d6ca`, `26476f4` |
| 16–20 | Shaders internos (GLSL), apresentador, EDRAM, texturas e amostradores, caminho de desenho | `b8ff0c3`..`498725c` |
| 21 | Jogo falso desenha um quadrado com textura de ponta a ponta | `7fb89df` |
| 22 | [ISO] Até a tela de título | em andamento (`75d8ee9`..) |
| 23 | Conferência visual pelo WSLg (escolha do dono) | pendente |
| 24 | AppImage, licenças, documentação, fechamento do M3 | pendente |

## Decisões do dono

- A ISO na raiz do repositório é dump do disco do dono, e o uso dela pelo contêiner foi autorizado (2026-10-18).
- A conferência visual em janela de verdade será pelo WSLg (Tarefa 23).
- O trabalho dos agentes roda em esforço alto (2026-10-24).
- Exceção pontual (2026-10-25): a desmontagem do vertex shader `E91536F60A69ABF3`, e só dele, pode ser lida para
  depuração. Ela nunca é commitada. A regra geral continua valendo: dados do jogo nunca entram no contexto nem no
  repositório.

## Decisões técnicas do caminho (resumo; os detalhes estão na ADR 0005)

- **Plano B** (M7, M8): o portão do plano A mostrou que o jogo carrega microcódigo alterado pelo D3D e microcódigo sem
  contêiner. Por isso nenhum cache feito na instalação cobre 100%. Os shaders passaram a ser traduzidos em tempo de
  execução, por um tradutor próprio, sem DXC.
- **DXC** (M2): os releases oficiais pedem glibc 2.38. O desenvolvimento usou o DXC do sistema até ele ser aposentado
  com o plano B.
- **Fluxo de controle dos shaders**: limite de 2^24 desvios para trás; `return` fora de função não faz nada; o `aL` é
  restaurado ao sair de um laço por salto.
- **Casos extremos do ALU escalar**: decididos pela entrada, para não depender do driver. Números denormais contam como
  zero, supondo que o Xenos descarta denormais; isso não foi conferido.
- **Componentes de vértice ausentes** saem como (0,0,0,1), e não como 0 (o Xenia usa 0).
- **Fila de comandos** (M12): um command buffer por quadro, com submissão e espera nos pontos de sincronização. É
  simples e seguro, mas lento.
- **Listas de retângulos** (M13): o quarto canto sai de um geometry shader, que exige `geometryShader` no dispositivo.
- **Saída ordenada**: SIGTERM, SIGINT e SIGHUP encerram com saída 0, com limite de 5 s; um segundo sinal encerra na
  hora.
- **`--gpu vulkan`** é o padrão, mesmo sem janela (lavapipe no contêiner). `--gpu null` volta à GPU sem desenho.

## Itens em aberto

Para a Tarefa 22, se aparecerem:
- shader com memexport (15 contêineres de pixel shader no XEX usam). Se o jogo carregar um, é decisão de plano;
- ordem S/T do cubo, formatos de render target não suportados (`k_16_16`, `k_16_16_16_16`, 7e3) e texturas com
  sinal;
- registradores de recorte ou de índice máximo zerados: os desenhos somem, agora com aviso;
- `Flat` a partir de `param_shade`, que é escolha nossa (o Xenia nunca marca `Flat`);
- ordem das metades da cor de limpeza de 64 bpp;
- consultas de oclusão (`EVENT_WRITE_ZPD`), registrador de status MMIO, ritmo do vblank sem janela;
- 7 ordinais da XAM pedidos por `XexGetProcedureAddress` falham (0x305, 0x30B, 0x48C, 0xAFF, 0xB00, 0xB0B, 0xB10),
  sem efeito visível até agora;
- thunks de ordinal sem nome ainda não ficam fora da descoberta de funções.

Para a Tarefa 23: janela, swapchain, redimensionamento e apresentação nunca rodaram, porque o contêiner não tem tela.

Para a Tarefa 24:
- a biblioteca `liblr_vulkan.a` e o `legacyrecomp-vkprobe` empacotados nunca foram compilados com o zig/glibc 2.31;
- a flag de teste `--gpu test-stall` deve ficar fora da ajuda do usuário.

Para o M4: as respostas provisórias de hardware (armazenamento, controle, Kinect, voz, rede, privilégios) e o
`XamContentCreateEnumerator` ignorando `device_id`.

## Observações de processo

- As sessões e o Docker Desktop caíram várias vezes, e o trabalho foi retomado de onde parou. Agora cada correção é
  commitada assim que os testes passam, antes de qualquer rodada longa com a ISO.
- O limite mensal de gastos da organização interrompeu a Tarefa 22 por dois dias (2026-10-25 a 2026-10-27).
- Os diagnósticos que derivam do jogo ficam fora do repositório, no volume de dados do Docker
  (`/data/scratch/sc-diag-*`, `/data/scratch/probe-cache-15.11`). Podem ser apagados quando convier.
