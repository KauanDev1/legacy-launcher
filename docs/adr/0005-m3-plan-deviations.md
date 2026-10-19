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
