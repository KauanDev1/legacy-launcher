# Roteiro

Fontes: spec do M1 (`docs/superpowers/specs/2026-10-14-legacyrecomp-m1-design.md`, seção 13) e spec do M2
(`docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`).

| Marco | Escopo | Estado |
|---|---|---|
| M1 | Pipeline completo (doctor a build), formatos, runtime mínimo, CLI e GUI, documentação. O jogo compila e para na primeira importação do kernel sem implementação (saída 3) | feito |
| M1, Task 17 | AppImage autossuficiente com toolchain embutido (zig c++, cmake, ninja, SDL2), `THIRD-PARTY-LICENSES` e arquivo-fonte completo (`docs/packaging.md`) | feito |
| M2 | Kernel HLE: memória virtual e física, threads/TLS/KTHREAD, sincronização, timers/DPC, handles, arquivos (VFS), strings/printf, XEX, LZX, XAM mínima, consultas de vídeo. O boot sem interface gráfica roda até a primeira chamada que cria a GPU, `VdInitializeEngines` (saída 3); 0 avisos do recompilador | feito |
| M3 | GPU: Vulkan + tradutor próprio de shaders (plano B). O jogo roda 600 quadros com a GPU emulada; falta a imagem da tela de título (`docs/m3-status.md`) | em andamento |
| M4 | Áudio, entrada e saves | futuro |
| M5 | Jogável: menu e mundo | futuro |

## Bloqueios conhecidos

- **Importações do kernel**: das 459 importações de função, 163 estão `implemented`, 29 `stub` e 267 `missing`; das
  15 de variável, 10 `implemented` e 5 `missing` (`docs/kernel-coverage.md`). As que faltam são sobretudo vídeo
  (M3), áudio, entrada e saves (M4), rede, LIVE e UI do sistema (M5).
- **Pouco do kernel do M2 foi exercitado pelo jogo real**: o boot faz só 27 importações, numa única thread, antes de
  `VdInitializeEngines` (`docs/reverse-engineering/kernel-imports.md`). Threads, esperas, arquivos, timers, LZX e
  printf foram testados por testes de unidade e pelo jogo falso; o M3 é o primeiro a exercitá-los de verdade.
- **Decisões não conferidas no jogo** (ADR 0004): argumentos de 64 bits e floats na pilha (M10), layout do `CONTEXT`
  em `RtlCaptureContext` (M16), layout do LDI do `XMemDecompress` (API pública da Microsoft, sem implementação no
  Xenia), `XGetAVPack` = 6 e `XamUserGetXUID` com máscara 4 (M14), `NtOpenFile` com 6 argumentos e link simbólico
  repetido (M12), vista física `0xE0` com `+0x1000` (M11), semântica MSVC do printf (M13).
- **Aliasing de memória física**: as vistas `0xA0`/`0xC0`/`0xE0` da memória física não são espelhadas entre si
  (spec do M2, §15); a GPU do M3 pode precisar disso.
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
