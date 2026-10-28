# ADR 0004: Desvios do plano do M2

- Status: aceita
- Data: 2026-10-16

Registro de cada decisão que se afastou do plano (`docs/superpowers/plans/2026-10-16-legacyrecomp-m2.md`) ou
do spec (`docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`), ou que preencheu uma lacuna dele.
Formato: o quê, por quê, custo se estiver errada.

## Decisões

### M1. O modelo `LR_KERNEL_EXPORT` é substituído por thunks sobre uma ABI fixa (spec §4.1/§4.2)
- O quê: o M1 gerava um stub fraco `PPC_FUNC(__imp__X)` por importação, que lançava `UnimplementedImport`, e uma
  implementação seria uma definição forte `LR_KERNEL_EXPORT(X)` no runtime. Agora o gerador
  (`render_import_thunks`, antes `render_import_stubs`) emite um thunk forte por importação, compilado com o jogo,
  que procura a implementação pelo nome no registro (uma vez, num `static` local), copia os registradores para o
  `GuestCall` de layout fixo e chama a `KernelFn` registrada. O registro passa a ter três estados (`Implemented`,
  `Stub`, `Missing`), com `LR_KERNEL(Nome)` e `LR_KERNEL_STUB(Nome, valor)`; `register_implemented`,
  `is_implemented` e `LR_KERNEL_EXPORT` foram removidos. `--coverage` passa a imprimir `| Import | Kind | Status |`
  e as contagens (hoje em duas linhas, `Functions: implemented a, stub b, missing c / N` e `Variables: ... / M`,
  ver M17). Toda chamada de thunk entra na trilha, então o relatório de saída 3 lista todas as importações chamadas
  antes da que falta (e termina nela).
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

### M7. Mudanças no XenonRecomp como patches no repositório, não fork (Tarefa 7)
- O quê: o `main` do upstream (hedge-dev/XenonRecomp) continua em `ddd128b` e não tem as instruções que o jogo usa;
  atualizar o pin não resolve. O plano previa publicar as nossas mudanças num fork no GitHub (passo humano); como o
  dono não escolheu um fork, elas ficam no próprio repositório, em `thirdparty/patches/xenonrecomp/NNNN-*.patch`
  (`git format-patch ddd128b..legacyrecomp`, com autoria, mensagens e créditos). O gitlink do submódulo volta para
  `ddd128b`, que qualquer clone consegue buscar. No configure, `cmake/LrSubmodulePatches.cmake` (antes `LrXenonRecompPatches.cmake`) espelha o
  submódulo em `build/<preset>/_deps/XenonRecomp-patched`, normaliza para LF os arquivos que os patches tocam,
  aplica os patches com `git apply` e compila essa cópia; o checkout do submódulo nunca muda. A reaplicação
  acontece quando um patch ou um original tocado muda (carimbo com hash). Tudo o que lia arquivos do XenonRecomp
  (`ppc_context.h`, simde, libmspack, o harness `tests/recomp`) usa `LR_XENONRECOMP_DIR`. O hash dos patches entra
  em `LR_XENONRECOMP_COMMIT` (`+patches:<hash>`), a impressão digital das etapas `analyse` e `recompile`. O fluxo
  para editar o recompilador está em `docs/development.md` (branch local `legacyrecomp`,
  `-DLR_XENONRECOMP_APPLY_PATCHES=OFF`, regenerar os patches).
- Conteúdo (10 patches, todos em `XenonRecomp/recompiler.cpp`, cada um coberto por `tests/recomp`): `bdzf`, `bso`,
  `bns`, `bdnzt` e a correção do `bdnzf`; loads/stores com update (`lfsu`, `lfsux`, `stfsu`, `stfsux`, `lhzu`, `sthu`,
  `lbzux`, `lhzux`, `lhau`, `lwzux`, `ldux`, `lfdu`, `lfdux`, `stbux`, `sthux`, `stdux`, `stfdu`), `lhbrx`, `lvehx`;
  `eqv`, `subfze`, `addc`, `addme`, `mulhd`; VMX/VMX128 `vsel128`, `vmaxuh`, `vminuh`, `vadduhs`, `vsubuws`,
  `vavguw`, `vslh`, `vrlw`/`vrlw128`, `vnor`/`vnor128`, `vcmpequh`, `vcmpgtuw`, `vpkswss`, `vpkuwum`, `vpkuwus`,
  `vpkuhum`, `vpkuhus` (e as formas 128), `vsl`, `vslo`/`vslo128`, `vctuxs`/`vcfpuxws128`; e o `vpkd3d128` FLOAT16_4.
- Crédito: várias dessas instruções seguem o PR #60 do upstream, de Mystixor (MIT); os commits dizem quais. Onde o
  PR erra, divergimos: `mulhd` usa produto de 128 bits (o PR multiplica em 64), `lfdu`/`lfdux` escrevem no FPR (o PR
  escrevia no GPR) e `vnor` com vA == vB dá o complemento (o PR zerava).
- `bdnzf`: o upstream supunha sempre o bit `eq` ("todas as instruções do jogo fazem isso", válido para o jogo do
  UnleashedRecomp), o que geraria código errado em silêncio para qualquer outro bit; agora o BI é decodificado
  inteiro (campo BI/4, bit BI%4).
- FLOAT16_4: os 20 sites do jogo são todos `pack=3 shift=0` (classificados com `tools/vpkd3d_sites.py` sobre o dump
  da imagem). Implementamos a tabela inteira de permutação do Xenia (`InstrEmit_vpkd3d128`: pack 1, 2 e 3, shifts 0 a
  3) e corrigimos vD == vB com shift diferente de 0, que sobrescrevia a fonte antes de lê-la toda. A conversão de
  meia precisão em si não mudou.
- Por quê: um repositório publicado não pode apontar para um commit de submódulo que ninguém consegue buscar, e os
  patches deixam explícito o que é nosso em cima do upstream.
- Custo se errado: atualizar o upstream exige reaplicar os patches (`git am` na branch local e regenerar); o
  configure passa a precisar do `git` (já exigido pelo container e pelo build do AppImage). Se o upstream aceitar
  instruções equivalentes, os patches correspondentes saem.

### M8. `XexLoadImage`, `XexUnloadImage` e `XMsg*` no M2 (Tarefa 28)

- O plano deixava `XexLoadImage`/`XexUnloadImage` (M5) e o `XMsg*` (M5) como `missing`. O trace real mostrou que o
  jogo chama `XexLoadImage("xam.xex")` e `XMsgStartIORequest(0xFE, 0x00022005)` no boot, antes da GPU.
- Decisão: `XexLoadImage` devolve o handle de um módulo já presente (executável, `xboxkrnl.exe`, `xam.xex`, como o
  Xenia faz para módulos carregados) e falha com `STATUS_NO_SUCH_FILE` para o resto (carregar XEX do disco continua
  M5); `XexUnloadImage` nunca descarrega. O `XMsg*` tem só o app XAM (`0xFE`) com as mensagens que o Xenia responde
  (`exports/msg.cpp`); `XMsgCompleteIORequest`/`XMsgCancelIORequest` continuam `missing` (o jogo não os chama antes
  da GPU).
- Custo se errado: um título que carregue um XEX de verdade recebe "arquivo não encontrado"; uma mensagem XAM
  desconhecida recebe `X_E_FAIL`, como no Xenia.

### M9. `container.sh` também sincroniza arquivos novos não rastreados (Tarefa 1)
- O quê: o plano dizia "arquivos rastreados"; a sincronização para o volume `/work` copia também os não rastreados
  que não estão no `.gitignore` (`git ls-files --others --exclude-standard`), e deixa de fora os rastreados apagados
  na árvore de trabalho.
- Por quê: o ciclo teste-falha-implementa roda no contêiner antes do commit, com arquivos novos ainda não
  adicionados; a spec (§12) quer que o contêiner teste o que o desenvolvedor tem.
- Custo se errado: um arquivo de rascunho não rastreado pode quebrar o build do contêiner (visível na hora).
  Arquivos não rastreados dentro de submódulos não são copiados.

### M10. Convenções da ABI das importações que o plano não fixava (Tarefa 8) — não conferidas no jogo
- O quê: (a) o `marshal` aceita um `GuestCall&` opcional como primeiro parâmetro da implementação, repassado sem
  consumir posição de argumento (o printf da CRT e `RtlCaptureContext` precisam dos registradores e da base);
  (b) argumentos de 64 bits na pilha (a partir do 9º) são lidos do slot inteiro de 8 bytes em
  `r1 + 0x50 + (i − 8)·8`, e floats vêm de `f1` em diante, em sequência, apesar de um resumo do Xenia dizer `0x54` e
  `f[1 + ordinal]` (o `0x54` é a palavra baixa do mesmo slot).
- Por quê: (a) evita uma segunda forma de registro só para essas importações; (b) é a ABI PowerPC de 64 bits
  do Xbox 360 lida diretamente; nenhuma importação do M2 com mais de 8 argumentos recebe 64 bits ou float na pilha.
- Custo se errado: uma linha em `read_int_arg` ou no índice de float; não verificado pelo jogo real (o boot chama só
  27 importações antes da GPU).

### M11. Memória física: vista `0xE0` com `+0x1000`, `free` só pela base e `STATUS_CONFLICTING_ADDRESSES` (Tarefas 12 e 13)
- O quê: (a) `MmGetPhysicalAddress` de um endereço da vista `0xE0000000` dá `(endereço & 0x1FFFFFFF) + 0x1000`, e o
  endereço `0xE0` de um físico é `0xE0000000 + físico − 0x1000`, como o `PhysicalHeap::GetPhysicalAddress` do Xenia;
  a spec (§6.2) dizia "menos 0x1000" e foi corrigida; (b) `PhysicalHeap::free` aceita só a base da alocação
  (`MmFreePhysicalMemory` passa a base), enquanto `allocation_size` aceita qualquer endereço de dentro; (c)
  `NtAllocateVirtualMemory` com base fixa que conflita devolve `STATUS_CONFLICTING_ADDRESSES` (semântica do NT), não
  o `STATUS_NO_MEMORY` do Xenia.
- Por quê: (a) o Xenia é a referência citada pela spec; (b) é o que o kernel faz; (c) o jogo trata os dois como
  falha.
- Custo se errado: (a) troca de sinal em duas linhas, visível como endereço de GPU errado no M3; (b) uma busca; (c)
  uma constante. O aliasing de memória física entre as vistas continua fora do escopo (spec §15).

### M12. Arquivos e objetos: `NtOpenFile` com 6 argumentos e link simbólico repetido (Tarefas 21 e 22)
- O quê: (a) `NtOpenFile(handle, access, attrs, iosb, share_access, open_options)` com 6 argumentos, como o NT e o
  xenia-canary (o Xenia master tem 5); (b) `ObCreateSymbolicLink` com um nome que já existe mantém o link antigo e
  devolve `STATUS_SUCCESS` (registrando em stderr e no trace), como o Xenia, em vez de `STATUS_OBJECT_NAME_COLLISION`.
- Por quê: (a) o canary é a branch mantida e bate com o NT; (b) o código de boot pode registrar `game:`/`cache:` de
  novo, e a leniência do Xenia é a aposta segura.
- Custo se errado: (a) flags de compartilhamento lidas como opções de abertura; (b) um título que dependa da colisão
  (improvável).

### M13. Família printf com a semântica do CRT da Microsoft (Tarefa 24)
- O quê: onde o Xenia diverge, segue-se o CRT/kernel da Microsoft: `_vsnprintf` devolve −1 quando trunca, e `%s` em
  `swprintf` é string larga.
- Por quê: o jogo foi compilado contra o CRT da Microsoft; o Xenia aproxima.
- Custo se errado: um comprimento de string diferente, visível no log.

### M14. Valores de vídeo e áudio fixos: `XGetAVPack` = 6 e consultas de vídeo como `stub` (Tarefa 26)
- O quê: `XGetAVPack` devolve 6, o valor do Xenia testado com jogos reais, e não um valor literal de HDMI;
  `XAudioGetSpeakerConfig` devolve estéreo com sucesso, como o Xenia. As consultas de vídeo (`VdQueryVideoMode`,
  `XGetVideoMode`, `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdQueryVideoFlags`,
  `VdIsHSIOTrainingSucceeded`, `XGetVideoCapabilities`, `XGetAVPack`) são registradas como `stub` (descrevem
  1280x720 widescreen fixo), e a cobertura as mostra assim. `XamUserGetXUID` responde à máscara de tipo 4, que o
  Xenia não responderia.
- Por quê: compatibilidade acima do texto literal do plano; a spec (§1) chama as consultas de stubs.
- Custo se errado: uma constante; a máscara 4 a rever se um trace a mostrar.

### M15. `XeCryptRotSumSha` continua `missing` (Tarefa 25)
- O quê: o plano punha a criptografia no M2, mas `XeCryptRotSumSha` não tem implementação de referência (o Xenia não
  a tem); pela regra da spec (§5.2) uma importação não atingida fica `missing`.
- Por quê: implementar sem referência seria chute; o boot real não a chama antes da GPU.
- Custo se errado: uma saída 3 nela quando o jogo a chamar (M3 em diante), a implementar pelo fluxo de engenharia
  reversa.

### M16. Exceções e contexto: `0x406D1388` retorna, `RtlCaptureContext` parcial (Tarefa 20) — não conferidos
- O quê: (a) `RtlRaiseException` com o código `0x406D1388` (o `SetThreadName` do MSVC) retorna em vez de terminar com
  `guest_fatal`, como o Xenia; (b) `RtlCaptureContext` escreve só os campos que preenche (`ContextFlags`, `iar` =
  lr, lr, r1, r3..r10, r13) no layout do `CONTEXT` público do Xbox 360, sem zerar a estrutura inteira (0xA40 bytes).
- Por quê: (a) o jogo nomeia threads assim e nada trata a exceção; (b) o layout não foi conferido, e zerar 0xA40 bytes
  com um layout errado poderia esmagar a pilha de quem chamou. O SEH completo continua fora do escopo (spec §15).
- Custo se errado: um deslocamento errado do `CONTEXT`, visível quando o jogo chamar `RtlCaptureContext`.

### M17. Detalhes de implementação que mudam o comportamento visível (Tarefas 14, 18 e 29)
- O quê: (a) criar a thread de trabalho do kernel (timers/DPC) também congela o layout de TLS
  (`mark_thread_created`), então o `main.cpp` chama `set_image` antes de `resolve_variable_imports`, que pode
  iniciá-la; uma ordem errada sai com 2; (b) `--coverage` termina com duas linhas de contagem, uma para funções e
  outra para variáveis (`Functions: implemented a, stub b, missing c / N`, `Variables: ... / M`), em vez de uma
  linha que somava as duas (spec §5.3, "N funções + M variáveis").
- Por quê: (a) uma ordem de inicialização errada falha alto em vez de corromper TLS; (b) a soma escondia quantas
  funções faltam.
- Custo se errado: nenhum além de quem lê a linha antiga (só a documentação a lia).

### M18. Trailers de coautoria dos commits (todas as tarefas)
- O quê: os commits feitos pelos subagentes trazem `Co-Authored-By: Claude Sonnet 5.5`, o modelo que os escreveu, e
  não o `Claude Opus 5.5` literal do `AGENTS.md`; os patches 0001–0003 do XenonRecomp também. O histórico não foi
  reescrito.
- Por quê: o objetivo da regra é a atribuição correta (o README diz que commits de agente se identificam).
- Custo se errado: um rebase não interativo para reescrever os trailers antes do push.

### M19. Lacunas menores do plano preenchidas na execução (Tarefas 11, 15, 18, 22 e ordem das tarefas)
- O quê: (a) os heaps devolvem `lr::Result` com `InvalidFormat` (argumento ruim), `OutOfRange` (sem espaço ou faixa
  em conflito) e `NotFound` (não alocado), sem o `enum class HeapError` que o plano também citava; o
  `NtAllocateVirtualMemory` e companhia mapeiam esses códigos para NTSTATUS; (b) todo objeto com handle precisa de
  endereço estável no convidado; `ObReferenceObjectByHandle` de um objeto sem endereço (nem cabeçalho nem corpo)
  devolve `STATUS_OBJECT_TYPE_MISMATCH` sem escrever nada nem fixar referência; (c) timer com período negativo →
  `STATUS_INVALID_PARAMETER`; DPC com rotina 0 → `guest_fatal`; (d) `NtReadFile`/`NtWriteFile` com deslocamento
  negativo diferente de −1/−2 → `STATUS_INVALID_PARAMETER`; (e) ordem de execução: Tarefas 1–6, 8–27 e só então 7, 28
  e 29, as que dependem da ISO e de decisões do dono.
- Por quê: (a) o plano se contradizia e os códigos de `Result` eram a metade concreta; (b) evita referências presas
  em objetos sem cabeçalho; (c)–(d) falhar alto em vez de comportamento silencioso; (e) nada de 8–27 dependia da 7.
- Custo se errado: mudanças pequenas e locais (um mapeamento, uma constante).

### M20. Outras decisões do registro de execução (várias tarefas)
- **Tarefas que usam a ISO** (7, 28 e 29): só rodaram depois que o dono do projeto confirmou que a imagem foi
  feita a partir de um disco próprio (em 2026-10-18); até lá eram pontos de verificação humanos (regra do projeto e
  spec §12). Custo se
  errado: só um ciclo mais lento.
- **Tarefa 6**: `tools/recomp_warnings.py` lista os sites float16_4 só pelo endereço, porque o aviso do XenonRecomp
  não traz os operandos; a Tarefa 7 os classificou com `tools/vpkd3d_sites.py`. Custo se errado: nenhum.
- **Tarefa 17** (ver também M6): `Threads::set_image` é recusado depois que existe qualquer thread do convidado (o
  layout de TLS fica fixo), e `KeDelayExecutionThread` com ponteiro de intervalo nulo faz só um yield, em vez de
  esperar para sempre. Custo se errado: nenhum código do jogo depende do contrário (evita corrupção e travamento).
- **Tarefas 2 e 11**: o `run_case` do harness `tests/recomp` carrega o ambiente de ponto flutuante do host
  (`fpscr.loadFromHost`) antes de cada caso; `PageHeap::allocation_size` aceita qualquer endereço de dentro da
  alocação (como o `QuerySize` do Xenia); o gancho de commit recebe uma flag de "não zerar" para `MEM_NOZERO`.
  Custo se errado: nenhum (os testes de `fcmpu`/`lfsu` e a Tarefa 13 dependem disso).
- **Tarefa 7**: o configure confere que cada patch do XenonRecomp foi de fato aplicado e reaplica quando um arquivo
  tocado sumiu da cópia; um build de release nunca sai com o recompilador sem patches em silêncio. Custo se errado:
  nenhum.
- **Tarefa 21**: uma listagem de diretório que falhou não entra no índice somente leitura do VFS (a próxima busca
  tenta de novo). Custo se errado: uma listagem a mais por busca num diretório inacessível.
- **Tarefa 26**: `XamUserGetXUID` responde às máscaras de tipo 1, 2 e 4 (o perfil local do Xenia é do tipo 1|2; a
  máscara 4 vai além do Xenia, ver M14); `XamUserGetSigninInfo` com índice >= 4 devolve `X_E_INVALIDARG`; o
  `complete_overlapped` só sinaliza o `hEvent` quando ele é um objeto Event. Custo se errado: uma resposta diferente
  numa máscara rara.
- **Tarefa 14**: `KeTimeStampBundle` apareceu como `stub` na cobertura até a Tarefa 18 passar a atualizá-lo a cada
  1 ms; desde então é `implemented`. Custo se errado: nenhum.
