<!-- SPDX-License-Identifier: MIT -->
# LegacyRecomp — Marco 2: Kernel HLE e inicialização completa sem gráficos

- **Data:** 2026-10-16
- **Status:** Aguardando revisão
- **Escopo:** Marco 2 (M2) do LegacyRecomp
- **Antecessor:** M1 (`docs/superpowers/specs/2026-10-14-legacyrecomp-m1-design.md`), concluído

## 1. Objetivo e direção

A direção do projeto é o **Minecraft: Xbox 360 Edition jogável por completo no Linux**: menu, mundos,
som, controle e saves (M5). O M2 é o primeiro passo dessa direção depois do pipeline do M1 e deve ser
desenhado para não atrapalhar os marcos seguintes (M3 gráficos, M4 áudio/entrada/saves, M5 jogável).

Hoje o executável recompilado roda até a primeira importação do kernel, `NtAllocateVirtualMemory`, e sai
com código 3. O M2 entrega:

1. **Um kernel HLE** (`xboxkrnl.exe` e `xam.xex`) suficiente para o jogo atravessar toda a inicialização:
   memória, threads, TLS, sincronização, arquivos, tempo, configuração do console e o mínimo de XAM.
2. **Código recompilado correto**: as instruções PowerPC que o XenonRecomp fixado não traduz hoje (3 279
   ocorrências) passam a ser traduzidas.
3. **Ferramentas de diagnóstico** para a fase de "rodar, ver onde para, implementar": rastreio de
   importações, relatório de cobertura com três estados, detecção de impasse.

**Ponto de chegada do M2:** `legacyrecomp run -- --headless` com a ISO suportada atravessa a
inicialização e para, com código 3, na **primeira importação de vídeo que cria ou usa a GPU**
(inicialização de engines, callback de interrupção gráfica, ring buffer, EDRAM, apresentação), que é onde
começa o M3. Até lá: nenhuma falha (saída 4), nenhum impasse, nenhuma importação do M2 faltando.

As **consultas** de vídeo que o XAPI/CRT faz cedo (`XGetVideoMode`, `VdQueryVideoMode`, `XGetAVPack`,
`XGetVideoCapabilities`, `VdGetCurrentDisplayInformation` e afins) não são o ponto de chegada: viram
`stub` do M2 que descreve uma saída 1280x720 widescreen por HDMI. Decisão do agente: o dono pediu o jogo
completo como direção, então o M2 vai o mais longe possível antes de depender de GPU de verdade.

O trabalho é por subsistema (memória, threads, sincronização e arquivos completos, com testes), mais um
laço final "rodar, ver onde para, implementar" guiado pelo rastreio. Importações classificadas como M2
que o jogo não chama até o ponto de chegada não são obrigatórias; ficam `missing` e entram quando
aparecerem.

## 2. Decisões

Todas as decisões abaixo foram tomadas pelo agente a partir da documentação do projeto, da pesquisa no
código (XenonRecomp, Xenia como referência de semântica, UnleashedRecomp só como referência de estrutura)
e das regras do `AGENTS.md`. O dono do projeto pediu que o spec fosse escrito sem rodada de perguntas;
cada decisão fica aberta à revisão dele.

| Tema | Decisão |
|------|---------|
| Instruções que faltam | **Fork do XenonRecomp** (`legacyrecomp` sobre `ddd128b`), submódulo aponta para o fork; implementação nossa, aproveitando o PR #60 do upstream (MIT) com crédito |
| Ligação do kernel | **ABI fixa + thunks gerados**: o código do kernel fica no `lr_runtime_core` pré-compilado e nunca vê `PPCContext` (R16 continua valendo); só os thunks gerados tocam `ctx` |
| Importações de variáveis | O loader resolve as importações de **dados** do kernel (que hoje ficam com lixo) escrevendo endereços de blocos do kernel na memória do convidado |
| Estados de importação | Três: `implemented`, `stub` (valor de retorno fixo e registrado no rastreio) e `missing` (lança `UnimplementedImport`, saída 3) |
| Memória | Mapa fixo de regiões; alocador próprio por página; vistas físicas sem espelhamento no M2 |
| Threads | Uma thread do host por thread do convidado, com PCR/KTHREAD/TLS próprios na memória do convidado |
| Objetos | Memória do convidado é a fonte da verdade (`DISPATCHER_HEADER`); tabela de handles própria |
| Arquivos | Links simbólicos (`game:`, `d:`, `cache:`…) para pastas da raiz de dados, com busca sem diferenciar maiúsculas |
| Ambiente de desenvolvimento | Contêiner Docker (Arch) versionado no repositório, para desenvolver e testar fora do Linux |

## 3. Instruções que o XenonRecomp não traduz

### 3.1 Fatos

- O XenonRecomp fixado (`ddd128b`, 2025-08-04) ainda é a ponta do `main` do upstream: atualizar o
  submódulo não resolve nada.
- Uma instrução não reconhecida vira só um comentário no C++ gerado (`recompiler.cpp`, `default:` do
  `switch` e laço em torno da linha 2432). O código **compila e roda errado**: loads/stores somem, o
  registrador base não é atualizado, `bdzf` perde o desvio e o decremento de CTR (laços infinitos).
- Contagem no jogo (`docs/reverse-engineering/addresses.md`): `bdzf` 754, `stfsu` 507, `lfsu` 350,
  `vsel128` 253, `lhzu` 219, `sthu` 211, `eqv` 94, `subfze` 65, `bso` 65, `lfsux` 63, mais ~200 de
  mnemônicos raros, e 20 avisos "Unexpected float16_4 pack instruction".
- Os opcodes já existem no desmontador (`thirdparty/disasm/ppc-inst.h`); o trabalho é só emitir o C++,
  copiando o padrão de casos vizinhos (`bdnz`, `lwzu`/`stwu`, `lfs`/`stfs`, `vsel`).
- O PR #60 do upstream (aberto, 2025-03-05) implementa todas as dez principais menos `bso`, no estilo do
  `ddd128b` (simde). O XenonRecomp é MIT, então aproveitar esse código é compatível com a nossa licença;
  os commits dão crédito ao autor.

### 3.2 Decisão

- Criar o fork `XenonRecomp` na conta do projeto no GitHub, branch `legacyrecomp` a partir de `ddd128b`.
  `.gitmodules` passa a apontar para o fork, num PR dedicado (regra da ADR 0001). **Criar o fork no GitHub
  é um passo humano** (ação externa na conta do dono).
- Enquanto o fork não existe, o trabalho nas instruções acontece em commits locais dentro do submódulo,
  num branch `legacyrecomp`. Nenhum commit do repositório principal que aponte para esses commits é
  publicado antes de eles estarem no fork: o repositório publicado nunca referencia um commit que outra
  pessoa não consegue baixar.
- Commits pequenos, um por instrução ou família, prontos para virar PR no upstream.
- Corrigir também o `bdnzf` existente, que só olha o bit `eq` ("assuming eq here as a shortcut").
- `bso`: ler o bit `so` do campo de CR indicado. Antes de implementar, conferir se essas 65 ocorrências
  seguem `fcmpu` (teste de "não ordenado"); nesse caso o `so` de CR precisa refletir o resultado não
  ordenado da comparação, e não o `XER.so`.
- `vsel128`: no desmontador o registrador de máscara é o próprio destino (`VS128 = VD128`); semântica
  conferida contra o Xenia, sem copiar código.
- float16_4: antes de mudar, um script em `tools/` lista os 20 sítios com `(tipo, pack, shift)`. A
  correção provável é tratar `pack = 1` (duas metades) e `shift = 3` com rotação de lanes. Erro aqui
  corrompe dados de vértice sem travar, então cada caso precisa de teste.
- O pipeline passa a contar os avisos "Unrecognized instruction" no `recompile.log` e grava o número no
  `analyse.log`/resumo; o critério de aceite exige zero para as instruções acima.

### 3.3 Testes

Cada instrução nova tem um teste que monta um ELF sintético (`tests/support/elf_builder.h`, já aceito pelo
`Image::ParseImage`) com a instrução, roda o XenonRecomp no build dos testes, compila o C++ gerado em
`lr_tests` e confere o resultado em registradores e memória. O mesmo padrão do `lr_fake_game`.

## 4. Como o kernel é escrito e ligado

### 4.1 Problema herdado do M1

`LR_KERNEL_EXPORT` gera `PPC_FUNC(__imp__X)`, que precisa do `ppc_config.h` gerado, mas o
`lr_runtime_core` é pré-compilado antes de existir código gerado (R16). O modelo "implementação forte no
arquivo `--whole-archive` vence o stub fraco" não funciona como escrito.

### 4.2 Decisão: ABI fixa

- `src/runtime/kernel/abi.h` define uma estrutura de layout fixo, sem depender do `PPCContext`:
  - `GuestCall`: ponteiro `base`; `gpr[8]` (r3..r10, 64 bits); `fpr[13]` (f1..f13); `r1`, `r13`, `lr`;
    retorno inteiro e retorno de ponto flutuante.
- O gerador de stubs (`src/pipeline/import_stubs.cpp`) passa a gerar **thunks**, um por importação,
  compilados no template do jogo. O thunk é o único código que toca `ctx`: copia os registradores para um
  `GuestCall`, chama a implementação (procurada uma vez pelo nome no registro e guardada em `static`),
  escreve de volta `r3` (e `f1`). Se não houver implementação, chama `unimplemented_import` como hoje.
  Os stubs fracos e a disputa fraco/forte deixam de existir.
- O registro (`runtime/kernel/registry`) mapeia nome → (estado, ponteiro de função). O `--whole-archive`
  continua só para que os registros estáticos sejam ligados.
- Cada chamada de thunk passa pelo rastreio (`trace`), então a trilha do relatório de saída 3 passa a
  ter todas as importações chamadas, não só as que se lembram de chamar `trace`.

### 4.3 Conversão de argumentos

Modelos C++ nossos convertem o `GuestCall` em parâmetros tipados (`LR_KERNEL(NtReadFile)` sobre uma função
C++ comum). Regras (convenção do Xbox 360, conferidas contra `shim_utils.h` do Xenia):

- Argumentos inteiros pela posição: r3..r10; a partir do nono, na pilha do convidado em
  `r1 + 0x54 + (i - 8) * 8` (32 bits, big-endian). `NtCreateFile` tem 9 argumentos: a pilha é necessária
  desde o começo, com teste.
- Ponto flutuante vem de f1..f13 e também consome uma posição de GPR.
- `GuestPtr<T>`/`be<T>*` são endereços de 32 bits do convidado traduzidos por `base`; endereço 0 vira
  ponteiro nulo.
- Retorno `NTSTATUS`/`DWORD` com extensão de sinal para 64 bits em `r3`; `float`/`double` em `f1`.
- Argumentos de 64 bits nunca são truncados para 32.

### 4.4 Chamadas do kernel para o convidado

Criar thread, APCs, DPCs, `XamTaskSchedule`, callbacks de notificação: o kernel precisa chamar funções
recompiladas. O `main.cpp` do template (R2) registra no início uma "cola" de ABI fixa:
`call_guest(endereço, args, n, r1, r13)`, que monta um `PPCContext` novo, procura a função com
`PPC_LOOKUP_FUNC` e a chama. Assim o runtime pré-compilado continua sem conhecer o layout do contexto.
Endereço sem função recompilada gera erro com o endereço do convidado, nunca salto para o endereço 0 do
host.

### 4.5 Exceções

- `UnimplementedImport` continua a única exceção que atravessa o código gerado para o caso de importação
  faltando.
- **Nova:** `GuestThreadExit`, lançada por `ExTerminateThread` (e pelo retorno da rotina de início) e
  capturada no topo da thread. Precisa de ADR (0004) e de atualizar a regra do `AGENTS.md`, que hoje diz
  "única exceção".
- SEH do convidado: no M2, `RtlRaiseException` e `KeBugCheck*` encerram com relatório (saída 5). SEH
  completo só se o jogo precisar.

## 5. Importações

### 5.1 Números

As 459 importações de função foram classificadas por subsistema; a lista completa está em
`docs/reverse-engineering/kernel-imports.md`. Estimativa:

| Marco | Importações | Observação |
|---|---|---|
| M2 | ~194 | ~110 com lógica de verdade; o resto são stubs de uma linha (inclui as 8 consultas de vídeo) |
| M3 | ~21 | `Vd*` que criam ou usam a GPU |
| M4 | ~45 | XAudio/XMA, entrada, conteúdo e saves |
| M5 ou nunca | ~199 | UI do sistema, avatares, voz, Kinect, rede (Winsock, XNet, XHttp, LIVE) |

Subsistemas do M2: memória (virtual, física, pools, heap da XAM), threads/processo/TLS/APC, sincronização
(eventos, semáforos, mutantes, timers, seções críticas, spinlocks, IRQL/DPC), gerenciador de objetos e
links simbólicos, E/S de arquivos, strings e `printf` da Rtl, SEH mínimo, tempo, consultas de módulo XEX e
dados de lançamento, descompressão LZX (`LDI*`), criptografia (`XeCrypt*`), configuração/idioma/região,
depuração, HAL, e o mínimo de XAM (notificações, tarefas assíncronas, usuário local, sondas de
dispositivo e licença).

### 5.2 Política de stubs

- Toda importação que o jogo chama antes do ponto de chegada tem que estar `implemented` ou `stub`.
- `stub` é uma decisão consciente, com o valor de retorno documentado no código (ex.: rede `WSAStartup`
  devolve sucesso; `XNetGetEthernetLinkStatus` devolve "sem cabo"; consultas de UI devolvem "inativo").
- Importações de M3 que criam ou usam a GPU continuam `missing`: são elas que marcam o ponto de chegada.
  As consultas de modo de vídeo são `stub` (seção 1).
- Importações de M4/M5 chamadas durante a inicialização viram `stub` com o comportamento de "recurso
  ausente" (sem áudio, sem controle conectado, sem rede, sem conta LIVE).

### 5.3 Importações de variáveis

- Fato: o `Image::ParseImage` do XenonUtils só transforma em símbolo as importações de **função**
  (`type != 0`, `xex.cpp` ~linha 333) e troca a ordem de bytes de todos os registros. As importações de
  **dados** (ex.: `KeTimeStampBundle`, `XboxHardwareInfo`, `XexExecutableModuleHandle`,
  `ExLoadedCommandLine`, `ExThreadObjectType`) ficam com lixo no espaço do convidado, e as 459 não as
  incluem. Um acesso a elas hoje é uma falha (saída 4), não um relatório de saída 3.
- Decisão: o loader percorre as bibliotecas de importação, identifica os registros de dados pelo ordinal
  (tabelas de nomes do XenonUtils) e escreve no slot o endereço big-endian de um bloco do kernel na
  memória do convidado. Variável sem implementação aponta para uma página `PROT_NONE` própria; o handler
  de falhas reconhece essa página e informa o nome da variável.
- `legacyrecomp inspect --imports <xex>` lista funções e variáveis importadas; `--coverage` e
  `docs/kernel-coverage.md` passam a contar as duas ("N funções + M variáveis").

## 6. Memória do convidado

### 6.1 Mapa

Continua um único mapeamento de 4 GiB (D5). Regiões fixas:

| Faixa | Uso |
|---|---|
| `0x00000000`–`0x0000FFFF` | Guarda de ponteiro nulo, `PROT_NONE` |
| `0x00010000`–`0x3FFFFFFF` | Heap virtual de páginas de 4 KiB: `NtAllocateVirtualMemory`, pool do kernel, corpos de objetos, PCR/KTHREAD/TLS |
| `0x40000000`–`0x6FFFFFFF` | Heap virtual de páginas de 64 KiB (`MEM_LARGE_PAGES`) |
| `0x70000000`–`0x7EFFFFFF` | Pilhas das threads do convidado, com página de guarda |
| `0x7F000000`–`0x7FFFFFFF` | Reservada para MMIO e write-back da GPU (M3) |
| `0x80000000`–`0x81FFFFFF` | Dados do kernel: cópia do cabeçalho XEX, `LDR_DATA_TABLE_ENTRY`, variáveis exportadas, páginas de variáveis faltando |
| `0x82000000`–`~0x8659FFFF` | Imagem do jogo e tabela de funções (ponteiros do host). Nunca entregue por um heap |
| `0xA0000000`–`0xFFFFFFFF` | Vistas físicas: `0xA0` (64 KiB), `0xC0` (16 MiB), `0xE0` (4 KiB) |

- Os blocos fixos do M1 em `0x7FFD0000`/`0x7FFE0000`/`0x7FFF0000` saem dessa faixa (ela é da GPU) e
  passam a ser alocados por thread, inclusive para a thread principal.
- O cabeçalho XEX passa a ser copiado para a memória do convidado (`RtlImageXexHeaderField`,
  `XexGetModuleSection`, `XexExecutableModuleHandle`).

### 6.2 Alocadores

- Alocador próprio por página (um byte de estado por página e mapa de regiões, protegido por mutex), com
  reservar, confirmar, desconfirmar, liberar e consultar; suporta `MEM_TOP_DOWN` e endereço pedido.
  Modelo: `BaseHeap` do `memory.cc` do Xenia (só referência). Confirmar zera a memória, salvo
  `MEM_NOZERO`. Códigos de status como em `xboxkrnl_memory.cc`.
- Memória física: um alocador de 512 MiB de páginas físicas; `MmAllocatePhysicalMemoryEx(flags, size,
  protect, min, max, align)` escolhe o tamanho de página pelos bits de proteção, aloca de cima para baixo
  dentro de `[min, max]` e devolve vista + deslocamento físico. Cada página física pertence a uma vista
  só; `MmGetPhysicalAddress` é uma conta (`& 0x1FFFFFFF`, mais `0x1000` na vista `0xE0` (como o `PhysicalHeap` do Xenia)).
- **Sem espelhamento entre vistas no M2.** Se o jogo acessar a mesma página física por duas vistas, isso
  aparece como dado divergente; o remédio (memfd com múltiplos mapeamentos) fica para quando acontecer.
- Modo estrito opcional (`--strict-memory`): páginas não confirmadas ficam `PROT_NONE`, então um acesso
  perdido falha com endereço do convidado em vez de ler zeros.

## 7. Threads

- Uma thread do host (pthread, pilha de 16 MiB, pilha alternativa de sinal) por thread do convidado.
- Por thread, na memória do convidado: PCR (`r13`), KTHREAD (cabeçalho de despachante tipo 6, pilha,
  TLS, id, endereço de início, último erro) e bloco TLS. Offsets conferidos contra `X_KPCR`/`X_KTHREAD`
  do Xenia e verificados com `static_assert`.
- TLS: o modelo estático do XEX (cabeçalho opcional `0x00020104`) é copiado para cada thread; os slots
  dinâmicos de `KeTlsAlloc` ficam na sequência.
- A thread principal é criada pelo mesmo caminho, com o tamanho de pilha do cabeçalho XEX (`0x00020200`)
  em vez do 1 MiB fixo do M1.
- `ExCreateThread`: lê tamanho de pilha e flags (bit 0 = suspensa; byte alto = máscara de CPU, registrada
  no PCR); se houver rotina de início XAPI, chama `rotina(início, contexto)`, senão `início(contexto)`.
  Devolve handle e id. O retorno vira o código de saída da thread.
- Suspender/retomar: criação suspensa e auto-suspensão bloqueiam no contador da KTHREAD; suspender outra
  thread em execução é cooperativo (checado na entrada de cada importação).
- Prioridade e afinidade: registradas, não aplicadas.
- Fim de thread: sinaliza o objeto da thread, acorda quem espera, libera a pilha quando a última
  referência sai.

## 8. Objetos, handles e esperas

- Tabela de handles própria (`0xF8000000 + 4·slot`), objetos com contagem de referência. Pseudo-handles:
  `0xFFFFFFFE` (thread atual) e `0xFFFFFFFF` (processo atual). Handle nunca é endereço do convidado.
- **A memória do convidado é a fonte da verdade**: todo objeto esperável tem o `DISPATCHER_HEADER` na
  memória do convidado (tipo `+0`, `signal_state` `+4`). Objetos que o próprio jogo inicializa (o
  `KeInitializeEvent` é inline, não é importação) funcionam sem registro prévio. Nenhum ponteiro do host
  é guardado dentro das estruturas do convidado.
- Motor de espera: um mutex global do despachante e um mapa "endereço do cabeçalho → esperadores", cada
  esperador com sua variável de condição. Tipos: evento (0/1), mutante (2), semáforo (5), thread (6),
  timer (8/9). Retornos `STATUS_WAIT_0 + i`, `STATUS_TIMEOUT`, `STATUS_USER_APC`, `STATUS_ALERTED`.
  Tempo limite: nulo = infinito, 0 = consulta, negativo = relativo em 100 ns, positivo = absoluto.
- APC e DPC: esperas alertáveis drenam a fila de APC da thread; DPCs e timers rodam numa thread de
  trabalho do kernel, que também atualiza `KeTimeStampBundle` (se o jogo a importar).
- Seções críticas (`RTL_CRITICAL_SECTION`, 28 bytes, campos big-endian): giro limitado, depois
  incremento atômico de `lock_count` e espera no evento embutido. Operações atômicas do host sobre as
  palavras em big-endian convivem com o `lwarx`/`stwcx.` do código gerado, que o XenonRecomp emite como
  CAS real (`__sync_bool_compare_and_swap`) sobre a mesma palavra.
- **Detector de impasse:** se todas as threads do convidado estão em espera infinita, sem timer ou APC
  pendente, por mais de 10 s, o runtime imprime cada thread com o objeto que espera e a trilha de
  importações, e sai com código 6.

## 9. Arquivos

- Links simbólicos (`ObCreateSymbolicLink` é importado):
  - `game:`, `d:` e `\Device\Cdrom0` → `<raiz>/game` (só leitura);
  - `cache:` e partições de disco → `<raiz>/user/cache`;
  - conteúdo XContent (quando aparecer na inicialização) → `<raiz>/user/content/<xuid>/<titleid>/<tipo>/<nome>`.
- `DataPaths` ganha `user/`. `user/` nunca é apagado pelo pipeline (diferente de `game/`, R11).
- Caminhos: remove `\??\`, aceita handle de diretório raiz, troca `\` por `/`, recusa `..` que saia da
  raiz. Busca sem diferenciar maiúsculas: índice em minúsculas por diretório, montado sob demanda (a árvore
  `game/` não muda); em pastas graváveis, busca componente a componente, e um arquivo novo recebe a grafia
  do convidado.
- `NtCreateFile`/`NtOpenFile`, `NtReadFile`/`NtWriteFile` (síncronos; sinalizam o evento e enfileiram a APC
  quando pedidos), `NtQueryInformationFile` (classes 5, 6, 14, 34), `NtQueryDirectoryFile` com curingas,
  `NtQueryFullAttributesFile`, `NtClose`. Status do Xbox (`0xC0000034`, `0xC000003A`, `0xC0000011`…).
- LZX (`LDICreateDecompression`, `LDIDecompress`, `LDIResetDecompression`, `LDIDestroyDecompression`):
  usam o `lzxd.c` da libmspack que já está dentro da `libXenonUtils.a` ligada ao jogo. Nenhuma obrigação
  de licença nova (a LGPL-2.1 já está coberta na ADR 0002).

## 10. Tempo, configuração e XAM mínima

- O XenonRecomp traduz `mftb` para `__rdtsc()` (`recompiler.cpp:1263`). Por isso
  `KeQueryPerformanceFrequency` devolve a frequência do TSC do host, **medida na inicialização**, e não
  a constante de 50 MHz do console. `KeQuerySystemTime` usa o relógio do host em FILETIME.
- `ExGetXConfigSetting`, `XGetLanguage`, `XGetGameRegion`, `XGetLocale`: idioma derivado do `LANG` do
  host (fallback inglês), região de valor fixo documentado.
- Usuário: um usuário local "Player" conectado offline, XUID fixo, sem LIVE.
- Notificações (`XamNotifyCreateListener`, `XNotifyGetNext`), tarefas (`XamTaskSchedule`) e
  `XOVERLAPPED` assíncrono: implementação mínima real, porque a inicialização espera por elas.
- Família `printf` (`_snprintf`, `_vsnprintf`, `sprintf`…): interpretador de formato próprio que lê os
  argumentos dos registros do convidado ou de uma `va_list` do convidado na memória.

## 11. Diagnóstico

- `legacyrecomp-game --trace-imports` (repassado por `legacyrecomp run -- --trace-imports`): uma linha por
  chamada de importação com thread, nome, r3..r6 e retorno, num arquivo em `work/logs/game-trace.log`.
- `--coverage`: tabela com os três estados (`implemented`/`stub`/`missing`) para funções e variáveis.
  `docs/kernel-coverage.md` regenerado a partir de um build real, como hoje.
- Códigos de saída do jogo: 0 normal, 2 uso/carga, 3 importação faltando, 4 falha, **5** encerramento
  pedido pelo convidado (`KeBugCheck`, exceção do convidado sem tratamento), **6** impasse.
- O relatório de saída 3 inclui a thread que chamou e a trilha completa.

## 12. Ambiente de desenvolvimento

- O dono do projeto desenvolve no Windows; o projeto só roda em Linux. O repositório ganha um contêiner
  Docker (Arch, as mesmas dependências do `AGENTS.md`) e um script que monta o repositório, compila no
  volume do contêiner (não na pasta do Windows, por desempenho) e roda `ctest`. Documentado no
  `AGENTS.md` e em `docs/development.md`.
- Testes de integração com a ISO continuam opcionais (`LEGACYRECOMP_ISO`) e rodam na máquina de quem tem
  a imagem do próprio disco. No Windows, rodam no contêiner com a ISO montada só para leitura.
- Tarefas que precisam do jogo real (contar os sítios de `bso` e float16_4, o laço final, regenerar
  `kernel-coverage.md`, os critérios de aceite 1 e 5) são executadas pelo agente no contêiner quando o
  dono autoriza o uso da ISO; sem autorização, viram pontos de verificação humanos, em que o dono roda o
  comando e devolve o log de rastreio.
- A VM do Docker Desktop desta máquina tem 4 GB: compilar o build `dev` com 6 jobs derrubou o Docker
  duas vezes. O script do contêiner limita os jobs pela memória da VM (1 job por 2 GB) e a documentação
  orienta subir a memória da VM nas configurações do Docker Desktop.

## 13. Testes

- **Unitários (sem o jogo):**
  - ABI: função de 9 argumentos (pilha), extensão de sinal, ponteiro nulo, posições de ponto flutuante.
  - Layouts: `static_assert(offsetof(...))` de PCR, KTHREAD, `DISPATCHER_HEADER`, `RTL_CRITICAL_SECTION`,
    `OBJECT_ATTRIBUTES`, `IO_STATUS_BLOCK`.
  - Alocadores: alinhamento, de cima para baixo, consulta, liberar e realocar, vistas físicas.
  - Esperas: eventos, semáforos, mutantes, timers e seções críticas entre 2 a 4 threads do host, com
    tempos limite curtos; detector de impasse.
  - Arquivos: pasta temporária com nomes de grafia mista, caminhos maliciosos, leitura além do fim.
  - `printf` do convidado, LZX, configuração.
  - Instruções novas do XenonRecomp (seção 3.3).
- **Jogo falso (`lr_fake_game`):** funções "do convidado" escritas à mão chamam `ExCreateThread`,
  `KeWaitForSingleObject` e `NtCreateFile` pelos thunks gerados e pela cola, cobrindo o caminho do
  template de ponta a ponta.
- **Integração (ISO real):** o teste ponta a ponta passa a exigir saída 3 numa importação de vídeo, sem
  falha e sem impasse, dentro de um tempo limite.

## 14. Critérios de aceite do M2

1. Com a ISO suportada, `legacyrecomp install` e depois `legacyrecomp run -- --headless` saem com código 3
   numa importação de vídeo que cria ou usa a GPU (seção 1); as consultas de modo de vídeo já passaram como
   `stub`.
2. Nenhum aviso "Unrecognized instruction" das instruções da seção 3.1 no `recompile.log`; os avisos de
   float16_4 resolvidos ou documentados um a um.
3. Todas as importações chamadas antes do ponto de chegada estão `implemented` ou `stub`;
   `docs/kernel-coverage.md` regenerado com funções e variáveis.
4. `ctest -L unit` passa (no Linux e no contêiner).
5. O mesmo resultado pelo AppImage, com o PATH sem clang/cmake/ninja.
6. Docs atualizados: `architecture.md` (mapa de memória, ABI, threads), `roadmap.md`, ADR 0004 com os
   desvios do M2, `AGENTS.md` (exceção `GuestThreadExit`, ambiente Docker).

## 15. Fora do escopo do M2

Gráficos e GPU (M3), áudio real, entrada real e saves (M4), rede e LIVE, UI do sistema, Kinect,
avatares, espelhamento de memória física, SEH completo, Windows e macOS nativos.

## 16. Riscos

| Risco | Mitigação |
|-------|-----------|
| A ordem real das importações difere da estimada | O plano trabalha por subsistema e fecha com um laço "rodar, ver onde para, implementar" guiado pelo rastreio |
| Funções recompiladas erradas fora das instruções conhecidas | `--strict-memory`, relatório de falha com endereço do convidado, conferência no Ghidra dos helpers e das 23 jump tables pendentes quando uma falha apontar para elas |
| Impasses por semântica de espera errada | Testes de concorrência, detector de impasse com o estado de cada thread |
| Fork do XenonRecomp divergir do upstream | Commits pequenos e rebase fácil; PRs para o upstream |
| O jogo depender de espelhamento físico | Detectável por dado divergente; memfd fica como plano B documentado |
| Exceções C++ do convidado na inicialização | Saída 5 com relatório; SEH entra só se aparecer |
