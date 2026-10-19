# ADR 0001: Recompilação estática com XenonRecomp

- Status: aceita
- Data: 2026-10-14

## Contexto

Queremos rodar o Minecraft: Xbox 360 Edition (XBLA, TU1) nativamente no Linux, a partir da ISO que o
usuário já possui. As alternativas realistas são:

1. **Emulação** (Xenia e similares): interpreta ou faz JIT do PowerPC em tempo de execução. Funciona para
   muitos jogos, mas o Xenia não roda nativamente no Linux de forma estável para este título e o resultado
   continua sendo um emulador, não um executável do jogo.
2. **Reimplementação a partir de decompilação manual**: inviável sem o código-fonte.
3. **Recompilação estática**: traduzir o PowerPC do XEX para C++ em tempo de instalação e compilar com um
   compilador comum, ligando a um runtime próprio que implementa o kernel (`xboxkrnl`, `xam`).

## Decisão

Usamos recompilação estática com o **XenonRecomp** de hedge-dev (MIT), o mesmo caminho do UnleashedRecomp.
O XenonRecomp entra como submódulo em `thirdparty/XenonRecomp`, fixado por commit
(`ddd128bcca99fe8bfbb99bea583c972351fa6ace`, branch `main` na data desta ADR). Os submódulos de
`thirdparty/` (XenonRecomp com tomlplusplus, fmt e xxHash embutidos; Catch2 v3.16.1; imgui) só mudam por
PR dedicado. Desde o M2 o XenonRecomp continua fixado em `ddd128b`, mas o build aplica por cima as mudanças do
LegacyRecomp guardadas em `thirdparty/patches/xenonrecomp/` (instruções que faltavam e correções; ADR 0004, M7).

O pipeline roda na máquina do usuário (`doctor`, `extract`, `verify`, `patch`, `analyse`, `recompile`,
`build`; ver `docs/pipeline.md`). Nenhum código ou dado do jogo é distribuído: o C++ gerado e o executável
`legacyrecomp-game` existem apenas no diretório de dados do usuário.

## Consequências

- O jogo roda como código nativo, sem interpretador; o custo é um passo de instalação longo (cerca de 6
  minutos e 8 GiB de disco no hardware de referência) porque o C++ gerado tem ~160 MB.
- O runtime precisa implementar cada importação do kernel que o jogo usa (459 no total); importação sem
  implementação aborta com código 3 (`UnimplementedImport`). Esse é o trabalho do M2.
- Limitações do XenonRecomp aparecem como instruções não reconhecidas no código gerado (ver
  `docs/roadmap.md`) e precisam de correção upstream ou patch nosso (hoje: os patches de
  `thirdparty/patches/xenonrecomp/`).
- Endereços específicos da versão (helpers de prólogo/epílogo, jump tables) vêm de engenharia reversa
  (`docs/reverse-engineering/`) e ficam em `config/minecraft-tu1/recomp.toml`.
- Trocar de abordagem depois (emulação) descartaria `pipeline/` e `runtime/kernel`, mas manteria
  `formats/` e `common/`.
