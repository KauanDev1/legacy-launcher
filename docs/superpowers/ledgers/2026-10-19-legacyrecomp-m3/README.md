<!-- SPDX-License-Identifier: MIT -->
# Registro de execução do M3

Cópia, feita em 2026-11-06, do registro que o desenvolvimento orientado por subagentes
(`superpowers:subagent-driven-development`) mantinha em `.superpowers/sdd/2026-10-19-legacyrecomp-m3/`. Essa pasta
é ignorada pelo git e ficou no Windows na mudança para o Linux; esta cópia guarda o contexto. Os arquivos estão em
inglês, como foram escritos, e sem mudanças: nenhum trazia dado do jogo (só hashes de shader, endereços e
contagens), então nada foi retirado.

| Arquivo | O que é |
|---|---|
| `progress.md` | O registro completo: varredura prévia do plano, rulings (com o custo se estiverem errados), itens carregados entre tarefas, itens estacionados, decisões do dono e o histórico de cada tarefa (despacho, implementação, revisão, rodadas de correção) até o cancelamento da Tarefa 23 |
| `global-constraints.md` | As restrições globais do plano coladas em cada brief, com as do plano B e as notas de ambiente do controlador (Windows, contêiner) |
| `plan-b-design.md` | Resumo do desenho do plano B (tradutor próprio de microcódigo para SPIR-V) usado nos briefs das Tarefas 15.2 a 15.11 |
| `plan-b-preflight.md` | A revisão prévia da emenda do plano B (veredito "corrigir antes"), com os achados críticos e importantes que o planejador aplicou |
| `adr-plan-b-draft.md` | Rascunho da entrada M8 do ADR 0005, que a Tarefa 15.1 copiou para `docs/adr/0005-m3-plan-deviations.md` |
| `task-20-carries.md` | Itens herdados de tarefas anteriores que a Tarefa 20 (caminho de desenho) tinha de cumprir |
| `task-22-carries.md` | Itens herdados que a Tarefa 22 (até a tela de título) tratava quando aparecessem, incluindo a regra de parada do memexport |

Ficaram de fora, de propósito, os briefs (`task-*-brief.md`), os relatórios (`task-*-report.md`), as revisões e os
diffs de revisão (`review-*.diff`). Os relatórios podem conter a análise da desmontagem de um shader do jogo, lida
com uma exceção pontual do dono, que nunca pode ir para o repositório. Os fatos úteis deles estão em
`docs/handoff.md`, `docs/m3-status.md` e no ADR 0005.

Para retomar o trabalho a partir desta cópia, veja "Registro de execução" em `docs/handoff.md`.
