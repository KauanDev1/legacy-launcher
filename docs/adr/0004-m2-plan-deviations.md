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

### M2. `GuestThreadExit` é a segunda exceção permitida (spec §4.5, Tarefa 17)
- O quê: `ExTerminateThread(código)` lança `lr::kernel::GuestThreadExit{código}`, capturada só no topo da thread do
  convidado (`runtime/kernel/threads.cpp`), que termina aquela thread com o código. A regra do `AGENTS.md` ("única
  exceção") passa a listar duas: `UnimplementedImport` e `GuestThreadExit`. Nenhuma das duas sai do runtime.
- Por quê: `ExTerminateThread` não retorna e pode ser chamada no meio de qualquer pilha de código recompilado
  (inclusive dentro de uma APC ou callback chamado por `call_guest`); desenrolar a pilha do host com uma exceção é
  o único jeito de sair dela sem `longjmp` sobre quadros C++ com destrutores.
- Custo se errado: código gerado compilado sem exceções quebraria o término; hoje ele já precisa delas para
  `UnimplementedImport`.

### M3. Códigos de saída 5 e 6 (spec §11)
- O quê: além de 0, 2, 3 e 4 do M1, o jogo sai com 5 (`kExitGuestFatal`, `guest_fatal`: `KeBugCheck`, exceção do
  convidado sem tratamento, chamada a endereço sem função) e 6 (`kExitDeadlock`, reservado para o detector de
  impasse). Saída 3 vale para importação faltando em qualquer thread: o relatório ganha a linha `Thread: <id>` e o
  processo inteiro termina com `_exit(3)`.
- Por quê: distinguir no launcher e na GUI um encerramento pedido pelo jogo de uma falha do host e de um impasse.
- Custo se errado: só a tabela de códigos do launcher muda.

### M4. Os blocos fixos do M1 saem de `0x7FFD0000`–`0x7FFF0000` (spec §6.1, Tarefa 17)
- O quê: KTHREAD, TLS e KPCR deixam de ser blocos fixos na faixa reservada à GPU e passam a ser alocados por
  thread no pool do kernel (heap de 4 KiB); as pilhas vêm de um `PageHeap` próprio sobre `0x70000000`–`0x7EFFFFFF`
  (`Kernel::stacks()`), com página de guarda `PROT_NONE` abaixo. A thread principal usa o mesmo caminho
  (`run_main_thread`), com a pilha do cabeçalho XEX em vez do 1 MiB fixo. `init_guest_pcr`, `run_guest` e as
  constantes `kGuestPcrAddress`/`kGuestTlsAddress`/`kGuestThreadAddress`/`kGuestStackBase` foram removidos.
- Por quê: várias threads precisam de PCR, KTHREAD, TLS e pilha próprios, e a faixa `0x7F000000`+ é do M3 (GPU).
- Custo se errado: nenhum código do jogo pode depender de endereços fixos desses blocos (ele os lê por `r13`).

### M5. Detalhes de threads que completam o plano (Tarefa 17)
- O quê: (a) slots de TLS: no mínimo 64 (o plano), mas o `slot_count` do cabeçalho TLS do XEX vale se for maior
  (o Xenia usa o do cabeçalho); (b) a suspensão cooperativa de outra thread é checada por um gancho em
  `lr::kernel::trace`, que todo thunk já chama, em vez de mudar o gerador de thunks; (c) `KeResumeThread` devolve
  um status, como o Xenia (o NT devolveria a contagem anterior); (d) quando a thread principal termina o processo
  sai com 0 por `_exit`, qualquer que seja o código de saída dela, sem rodar destrutores estáticos com outras
  threads ainda vivas; (e) `NtAlertThread` não é importado pelo jogo, então `STATUS_ALERTED` nunca é produzido.
- Por quê: (a) o jogo pode pedir mais slots do que o mínimo; (b) mantém a impressão digital da etapa `build` e o
  código gerado; o custo é uma leitura atômica por importação; (c)–(e) seguem o comportamento observado no Xenia.
- Custo se errado: (a) memória de TLS por thread um pouco maior; (b) uma thread suspensa que nunca chama uma
  importação nunca para (aceitável: suspender outra thread é raro e o jogo só o faz em pontos de sincronização).

### M6. Validação do cabeçalho TLS e limpeza do despachante no fim da thread (Tarefa 17, correção)
- O quê: `Threads::set_image` valida o cabeçalho TLS do XEX (no máximo 4096 slots e 4 MiB de dados,
  `raw_data_size` <= `data_size`, dados brutos dentro da imagem carregada) e o jogo sai com 2 se ele for inválido;
  o bloco por thread é dimensionado em 64 bits. `set_image` é recusado depois da primeira thread. No fim de uma
  thread, `Dispatcher::thread_exited` apaga o despertar de APC pendente e abandona os mutantes que ela possui.
  `KeDelayExecutionThread` com ponteiro de intervalo nulo faz só um yield.
- Por quê: um cabeçalho malformado faria alocações gigantes ou um bloco curto (estouro de 32 bits) e a cópia do
  modelo escreveria sobre outros objetos do kernel; e o estado por thread do despachante é indexado pelo endereço do
  KTHREAD, que o pool reutiliza para a próxima thread.
- Custo se errado: um título legítimo acima dos limites seria recusado na carga (limites bem acima do uso real).
