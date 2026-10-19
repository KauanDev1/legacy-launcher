# Roteiro

Fonte: spec do M1 (`docs/superpowers/specs/2026-10-14-legacyrecomp-m1-design.md`, seção 13).

| Marco | Escopo | Estado |
|---|---|---|
| M1 | Pipeline completo (doctor a build), formatos, runtime mínimo, CLI e GUI, documentação. O jogo compila e para na primeira importação do kernel sem implementação (saída 3) | feito |
| M1, Task 17 | AppImage autossuficiente com toolchain embutido (zig c++, cmake, ninja, SDL2), `THIRD-PARTY-LICENSES` e arquivo-fonte completo (`docs/packaging.md`) | feito |
| M2 | Kernel HLE: threads, arquivos, sincronização, TLS/KTHREAD completos | futuro |
| M3 | GPU: Vulkan + XenosRecomp (shaders) | futuro |
| M4 | Áudio, entrada e saves | futuro |
| M5 | Jogável: menu e mundo | futuro |

## Bloqueios conhecidos

- **459 importações do kernel** (`xboxkrnl` e `xam`) sem implementação; a lista está em
  `docs/kernel-coverage.md`. É o trabalho do M2.
- ~~Instruções que o XenonRecomp (`ddd128b`) não implementa~~ (resolvido no M2, Tarefa 7): as 3 279 ocorrências
  "Unrecognized instruction" e os 20 avisos float16_4 do M1 caíram para 0 com os patches de
  `thirdparty/patches/xenonrecomp/` (ADR 0004, M7; contagens em `docs/reverse-engineering/addresses.md`). Os
  patches não estão no upstream: atualizar o XenonRecomp exige reaplicá-los (`docs/development.md`).
- **23 jump tables a confirmar no Ghidra** (de 221 `switch` detectados): o `analyse` as acha por varredura própria, mas a confirmação
  manual ainda não foi feita.
- **Endereços dos helpers (prólogo/epílogo etc.) não confirmados no Ghidra**: aceitos por evidência
  estrutural (ruling R13); um endereço errado quebraria em tempo de execução.
- **CPU com AVX obrigatória** enquanto o build for `-march=sandybridge` (ruling R20).
- Distribuição binária exige entregar licenças de terceiros e o código-fonte (rulings R24 e R25, ADR 0002).
