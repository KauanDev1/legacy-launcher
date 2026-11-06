# Roteiro

Fontes: spec do M1 (`docs/superpowers/specs/2026-10-14-legacyrecomp-m1-design.md`, seção 13), spec do M2
(`docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`) e spec do M3
(`docs/superpowers/specs/2026-10-19-legacyrecomp-m3-design.md`). Para retomar o trabalho: `docs/handoff.md`.

| Marco | Escopo | Estado |
|---|---|---|
| M1 | Pipeline completo (doctor a build), formatos, runtime mínimo, CLI e GUI, documentação. O jogo compila e para na primeira importação do kernel sem implementação (saída 3) | feito |
| M1, Task 17 | AppImage autossuficiente com toolchain embutido (zig c++, cmake, ninja, SDL2), `THIRD-PARTY-LICENSES` e arquivo-fonte completo (`docs/packaging.md`) | feito |
| M2 | Kernel HLE: memória virtual e física, threads/TLS/KTHREAD, sincronização, timers/DPC, handles, arquivos (VFS), strings/printf, XEX, LZX, XAM mínima, consultas de vídeo. O boot sem interface gráfica roda até a primeira chamada que cria a GPU, `VdInitializeEngines` (saída 3); 0 avisos do recompilador | feito |
| M3 | GPU: Vulkan + tradutor próprio de shaders (plano B). O jogo chega à tela de título sem janela (lavapipe) pelos critérios objetivos: `frame_stats` em N=300 com 73 762 cores, luminância 0,39 e 4,9% de bordas; N=600 com a camada de validação e 0 erros; teste de integração `real ISO: the game reaches its title screen` (Tarefas 1 a 22). Falta: a conferência visual com janela no Linux nativo (a Tarefa 23 pelo WSLg foi cancelada; janela, swapchain, redimensionamento e apresentação nunca rodaram), três detalhes de documentação, a Tarefa 24 (AppImage, licenças, documentação), a revisão final da branch, o push e o PR (`docs/m3-status.md`, `docs/handoff.md`) | em andamento |
| M4 | Áudio, entrada e saves, sem spec ainda. Áudio: callback real do driver de render XAudio (hoje nunca chamado) e decodificador XMA (hoje contextos zerados que ninguém lê). Entrada: controle do SDL2 → XInput, vibração, teclado virtual. Saves: XContent como pastas do host, seletor de dispositivo, perfil gravável, `XeKeys*`/assinaturas. Revisitar as respostas provisórias do M3: sem armazenamento, sem controle, sem Kinect, voz inativa, rede offline, todos os privilégios | futuro |
| M5 | Jogável: menu e mundo. Previsto: shaders do mundo (15 contêineres de pixel shader do XEX usam memexport, que o tradutor ainda não faz), desempenho numa GPU real (60 Hz; hoje o envio é esperado a cada resolve e swap), a UI do sistema que o jogo exigir, rede local (System Link) se valer. LIVE, avatares, voz e Kinect provavelmente ficam como stubs de falha | futuro |

## Bloqueios conhecidos

- **Importações do kernel**: das 459 importações de função, 176 estão `implemented`, 58 `stub` e 225 `missing`; as
  15 de variável estão `implemented` (`docs/kernel-coverage.md`, atualizado na Tarefa 22 do M3). Das que faltam, 114
  são de rede e 95 da XAM (UI do sistema, avatares, Kinect, conteúdo e saves, perfil, LIVE, voz); o resto é
  áudio (ducker), chaves e assinaturas (M4) e casos isolados. Divisão por área em `docs/handoff.md`.
- **Pouco do kernel do M2 foi exercitado pelo jogo real**: o boot faz só 27 importações, numa única thread, antes de
  `VdInitializeEngines` (`docs/reverse-engineering/kernel-imports.md`). Threads, esperas, arquivos, timers, LZX e
  printf foram testados por testes de unidade e pelo jogo falso; o M3 é o primeiro a exercitá-los de verdade. Até
  a tela de título o jogo já faz 117 importações distintas em 15 threads (Tarefa 12 do M3), mas nada além disso
  (menu com controle, saves, mundo) rodou.
- **Decisões não conferidas no jogo** (ADR 0004): argumentos de 64 bits e floats na pilha (M10), layout do `CONTEXT`
  em `RtlCaptureContext` (M16), layout do LDI do `XMemDecompress` (API pública da Microsoft, sem implementação no
  Xenia), `XGetAVPack` = 6 e `XamUserGetXUID` com máscara 4 (M14), `NtOpenFile` com 6 argumentos e link simbólico
  repetido (M12), vista física `0xE0` com `+0x1000` (M11), semântica MSVC do printf (M13).
- ~~Aliasing de memória física~~ (resolvido no M3, Tarefa 22): as vistas `0xA0`/`0xC0`/`0xE0` não eram espelhadas
  (spec do M2, §15), e o jogo preenche texturas por uma vista e as lê por outra, o que deixava as fontes zeradas.
  Agora um `memfd` de 512 MiB é mapeado nas três vistas (`563485e`, ADR 0005 M14).
- **SEH incompleto**: não há despacho de exceções do convidado; `RtlUnwind` e `__C_specific_handler` terminam
  com `guest_fatal` (saída 5), como qualquer exceção sem tratamento. Fica para quando um trace mostrar o jogo
  dependendo disso.
- ~~Instruções que o XenonRecomp (`ddd128b`) não implementa~~ (resolvido no M2, Tarefa 7): as 3 279 ocorrências
  "Unrecognized instruction" e os 20 avisos float16_4 do M1 caíram para 0 com os patches de
  `thirdparty/patches/xenonrecomp/` (ADR 0004, M7; contagens em `docs/reverse-engineering/addresses.md`). Os
  patches não estão no upstream: atualizar o XenonRecomp exige reaplicá-los (`docs/development.md`).
- **23 jump tables a confirmar no Ghidra** (de 221 `switch` detectados): o `analyse` as acha por varredura própria,
  mas a confirmação manual ainda não foi feita.
- ~~Mapeamento duplicado de funções~~ (resolvido no M3, Tarefa 22): a descoberta de funções só por ponteiro
  emitia uma função em 3 endereços de thunks de importação (alcançados só por tabela de ponteiros, como o
  `NtReadFile` da CRT), e a tabela de chamadas indiretas ficava com a primeira entrada sem avisar. Agora o `analyse`
  passa os thunks como funções conhecidas, e `install_function_table` devolve e o jogo imprime cada duplicata
  (`[runtime] warning: N duplicate function mappings dropped`); a primeira continua valendo (os thunks vêm antes).
  Thunks de ordinal sem nome no XenonUtils não viram símbolo e continuam fora dessa lista.
- **Endereços dos helpers (prólogo/epílogo etc.) não confirmados no Ghidra**: aceitos por evidência
  estrutural (ruling R13); um endereço errado quebraria em tempo de execução.
- **CPU com AVX obrigatória** enquanto o build for `-march=sandybridge` (ruling R20).
- Distribuição binária exige entregar licenças de terceiros e o código-fonte (rulings R24 e R25, ADR 0002).
