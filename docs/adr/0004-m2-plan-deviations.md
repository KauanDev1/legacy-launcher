# ADR 0004: Desvios do plano do M2

- Status: aceita
- Data: 2026-10-16

Registro de cada decisão que se afastou do plano (`docs/superpowers/plans/2026-10-16-legacyrecomp-m2.md`) ou
do spec (`docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`), ou que preencheu uma lacuna dele.
Formato: o quê, por quê, custo se estiver errada.

## Decisões

### M1. O modelo `LR_KERNEL_EXPORT` é substituído por thunks sobre uma ABI fixa (spec §4.1/§4.2)
- O quê: o M1 gerava um stub fraco `PPC_FUNC(__imp__X)` por importação, que lançava `UnimplementedImport`,
  e uma implementação seria uma definição forte `LR_KERNEL_EXPORT(X)` no runtime. Agora o gerador
  (`render_import_thunks`, antes `render_import_stubs`) emite um thunk forte por importação, compilado com o
  jogo, que procura a implementação pelo nome no registro (uma vez, num `static` local), copia os
  registradores para o `GuestCall` de layout fixo e chama a `KernelFn` registrada. O registro passa a ter
  três estados (`Implemented`, `Stub`, `Missing`), com `LR_KERNEL(Nome)` e `LR_KERNEL_STUB(Nome, valor)`;
  `register_implemented`, `is_implemented` e `LR_KERNEL_EXPORT` foram removidos. `--coverage` passa a
  imprimir `| Import | Kind | Status |` e `Implemented: a, stub: b, missing: c / total`. Toda chamada de
  thunk entra na trilha, então o relatório de saída 3 lista todas as importações chamadas antes da que
  falta (e termina nela).
- Por quê: `LR_KERNEL_EXPORT` gera `PPC_FUNC`, que precisa do `ppc_config.h` gerado, mas o `lr_runtime_core`
  é compilado antes de existir código gerado (R16 do ADR 0003). O modelo "forte vence fraco" não podia ser
  usado no runtime pré-compilado. Com a ABI fixa, só o thunk (compilado com o jogo) toca o `PPCContext`.
  O arquivo gerado continua se chamando `import_stubs.cpp` (e o módulo `pipeline/import_stubs`) para não
  mudar o template, a impressão digital da etapa `build` nem builds incrementais existentes.
- Custo se errado: uma cópia de 24 valores (8 GPR, 13 FPR, r1, r13, lr) e uma chamada indireta por
  importação chamada (barato perto de qualquer syscall do kernel); se algum dia uma importação precisar de
  registradores fora do `GuestCall` (por exemplo r11/r12 ou vetores), o `GuestCall` e o helper do thunk
  ganham campos.
