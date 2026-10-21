<!-- SPDX-License-Identifier: MIT -->
# Importações do kernel por subsistema

Esta é a classificação das 459 importações de função (de `docs/kernel-coverage.md`) por subsistema e por marco,
feita em 2026-10-16 para a spec do M2 (`docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`). Ela se
baseia no conhecimento da semântica do Xenia (usado apenas como referência). A ordem de inicialização abaixo é a real,
registrada com `--trace-imports` na Task 28 do M2.

Fontes: `docs/kernel-coverage.md` (459 nomes e estados) e `docs/reverse-engineering/*`. Categorias e marcos
vêm do conhecimento da ABI do kernel/XAM do Xbox 360 e das implementações `xboxkrnl_*.cc`/`xam_*.cc` do Xenia. A
lista foi gerada com um script que verifica que cada um dos 459 nomes está em exatamente uma categoria. Onde as
notas originais divergiam da spec do M2, a spec prevalece (tempo, LZX e consultas de vídeo, ver categorias 10, 12
e 17).

Marcos: **M2** = necessário (ou barato o suficiente para fazer agora e provavelmente atingido) para passar pela
inicialização sem interface gráfica até a primeira chamada de vídeo que cria ou usa a GPU; **M3** gráficos; **M4**
áudio/entrada/saves; **M5** depois/online/UI. Uma importação M5 ainda precisa de um *stub registrado* que retorne
uma falha sensata (nenhuma importação pode abortar com exit 3 em jogo normal), mas sem esforço de design.

## Resumo

| # | Categoria | Quantidade | Padrão | M2 | M3 | M4 | M5 |
|---|---|---|---|---|---|---|---|
| 1 | Memória virtual/física, pools, heap Xam | 18 | M2 | 17 | 1 | 0 | 0 |
| 2 | Threads, processo, TLS, APC, estado da FPU | 26 | M2 | 26 | 0 | 0 | 0 |
| 3 | Objetos de sincronização, esperas, spinlocks, IRQL/DPC, seções críticas | 29 | M2 | 29 | 0 | 0 | 0 |
| 4 | Gerenciador de objetos e handles | 9 | M2 | 9 | 0 | 0 | 0 |
| 5 | Sistema de arquivos e E/S | 15 | M2 | 13 | 0 | 2 | 0 |
| 6 | XContent, dispositivos de save, cache Xam | 20 | M4 | 3 | 0 | 15 | 2 |
| 7 | Infraestrutura assíncrona Xam: enumeradores, overlapped, tarefas, XMsg, notificações, sessões | 15 | M2 | 11 | 0 | 0 | 4 |
| 8 | Strings Rtl, auxiliares de memória, printf da CRT | 18 | M2 | 18 | 0 | 0 | 0 |
| 9 | Exceções / SEH / contexto | 4 | M2 | 4 | 0 | 0 | 0 |
| 10 | Tempo / relógio | 4 | M2 | 4 | 0 | 0 | 0 |
| 11 | Módulo XEX / loader / dados de lançamento | 13 | M2 | 10 | 0 | 0 | 3 |
| 12 | Descompressão LZX (LDI*, base do XMemDecompress) | 4 | M2 | 4 | 0 | 0 | 0 |
| 13 | Criptografia e chaves | 12 | M2 | 9 | 0 | 3 | 0 |
| 14 | Configuração do sistema, locale, informações do título | 9 | M2 | 6 | 0 | 0 | 3 |
| 15 | Depuração, tracing, bugcheck | 7 | M2 | 7 | 0 | 0 | 0 |
| 16 | Hardware / HAL / controle de cache | 3 | M2 | 1 | 2 | 0 | 0 |
| 17 | Vídeo (Vd*, consultas de modo de vídeo) | 26 | M3 | 8 | 18 | 0 | 0 |
| 18 | Áudio (driver de render XAudio, XMA, ducker) | 13 | M4 | 0 | 0 | 13 | 0 |
| 19 | Entrada (gamepad, keystroke) | 4 | M4 | 0 | 0 | 4 | 0 |
| 20 | Usuários, sign-in, configurações de perfil, gamer tiles | 19 | M2 | 8 | 0 | 3 | 8 |
| 21 | UI do sistema (XamShow*, guide) | 26 | M5 | 2 | 0 | 5 | 19 |
| 22 | Avatares | 13 | M5 | 1 | 0 | 0 | 12 |
| 23 | Voz / headset | 5 | M5 | 0 | 0 | 0 | 5 |
| 24 | Kinect / NUI / câmera / biometria | 15 | M5 | 0 | 0 | 0 | 15 |
| 25 | Rede: Winsock (NetDll_* BSD/WSA) | 35 | M5 | 4 | 0 | 0 | 31 |
| 26 | Rede: XNet / Xnp / XNetLogon (pilha de rede segura, QoS, chaves) | 62 | M5 | 5 | 0 | 0 | 57 |
| 27 | Rede: XHttp | 22 | M5 | 0 | 0 | 0 | 22 |
| 28 | Xbox LIVE / online diversos (XAuth, fila de upload Xlfs, tokens, Live hive, download em segundo plano, XStudio) | 13 | M5 | 0 | 0 | 0 | 13 |
| | **Total** | **459** | | **199** | **21** | **45** | **194** |

Como ler a tabela: cerca de 199 importações caem no M2, mas só ~110 delas precisam de lógica real (memória,
threads/TLS, sincronização, handles, arquivos, strings Rtl/printf, consultas ao cabeçalho XEX, SHA, LZX). As demais
entradas M2 são stubs de uma linha, constantes ou no-op (configuração, depuração, locale, inicialização de
Winsock/XNet, estado de sign-in, XamIsUIActive, as 8 consultas de vídeo que descrevem 1280x720 widescreen HDMI).
191 dos 459 nomes (rede 119, LIVE diversos 13, UI 26, Kinect 15, avatares 13, voz 5) quase certamente são puxados
por bibliotecas do XDK ligadas estaticamente (XOnline/XNet/XHttp/Avatar/NUI) e podem nunca executar numa sessão
single-player offline.

Correções em relação à classificação original (a spec do M2 prevalece): as 8 consultas de vídeo
(`XGetVideoMode`, `VdQueryVideoMode`, `XGetAVPack`, `XGetVideoCapabilities`, `VdQueryVideoFlags`,
`VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdIsHSIOTrainingSucceeded`) passaram de M3 para M2,
levando os totais de M2 186 / M3 29 para M2 194 / M3 21; `KeQueryPerformanceFrequency` retorna a frequência do TSC
do host (não 50 MHz); o LZX usa o `lzxd.c` da libmspack já compilado na `libXenonUtils.a`. Na Task 28 o trace real
mostrou `XexLoadImage`/`XexUnloadImage` e `XMsgStartIORequest`/`XMsgInProcessCall`/`XMsgSystemProcessCall` no boot,
antes da GPU: as cinco passaram de M5 para M2 (totais M2 199 / M5 194; ADR 0004, M8).

## Exportações de DADOS do kernel (variáveis) -- NÃO estão nesta lista

A lista **exclui importações de variáveis**. Evidência (do repositório, sem ler dados do jogo):
`thirdparty/XenonRecomp/XenonUtils/xex.cpp` (`Image::ParseImage`, ~linha 329) percorre todos os descritores de
importação, mas só cria um símbolo `__imp__<Name>` e reescreve o thunk quando `originalData.type != 0`, isto é,
para thunks de função. Registros de variável (tipo 0) não ganham símbolo, então `render_import_thunks` /
`lr_import_names` (459 entradas) nunca os veem. Nenhum de `KeTimeStampBundle`, `XboxHardwareInfo`,
`XexExecutableModuleHandle`, `ExLoadedImageName`, `KeDebugMonitorData`, `XboxKrnlVersion`, `ExConsoleGameRegion`,
`Ex*ObjectType`/`Ob*ObjectType`/`IoFileObjectType`, `VdGlobalDevice`, `VdGpuClockInMHz`, `KiBugCheckData` aparece
nas 459.

Consequências para o M2:

1. **O título quase certamente importa algumas variáveis mesmo assim.** Indício forte: ele importa
   `ObReferenceObjectByHandle`, cujo segundo argumento é um ponteiro de tipo de objeto (`ExThreadObjectType`,
   `ExEventObjectType`, ...) que só existe como exportação de dados do kernel; a inicialização da CRT/XAPI do XDK
   também lê `XexExecutableModuleHandle` (para chamar `RtlImageXexHeaderField` sobre o seu cabeçalho) e
   tipicamente `XboxHardwareInfo` (número de CPUs / flags) e `KeTimeStampBundle` (contador de ticks para
   `GetTickCount`).
2. **ParseImage corrompe os slots de variáveis.** Ele executa `originalData->data = ByteSwap(originalData->data)`
   *antes* da verificação de tipo, então a palavra da IAT de uma variável fica com bytes invertidos na imagem que
   `src/runtime/loader.cpp` carrega. O loader do M2 precisa percorrer a tabela de importações por conta própria,
   identificar os registros de tipo 0 (ordinal -> nome pelas mesmas tabelas de exportação do XenonUtils), alocar
   as estruturas de dados emuladas do kernel na memória do guest e escrever seus **endereços de guest em
   big-endian** nesses slots antes de o ponto de entrada executar. Um slot nulo/lixo aparece como falha do guest
   (exit 4), não como exit 3, então o relatório de cobertura não consegue detectá-lo.
3. Ação: adicionar um modo `inspect --imports` (ou estender `--coverage`) que liste as importações de variáveis por
   nome, para que a spec possa enumerá-las; o cabeçalho do documento "459 importações" deveria dizer "459 function
   imports (+N variables)".

Estado: os itens 2 e 3 foram feitos na Task 14 do M2 (`kernel::resolve_variable_imports`, `inspect --imports`,
linhas `variable` em `--coverage --image`); detalhes em `docs/formats/xex2.md`.

## Ordem real das chamadas na inicialização

Registrada na Task 28 do M2 (2026-10-18) com `legacyrecomp run -- --headless --trace-imports` sobre a imagem TU1:
2428 chamadas, todas na thread 1 (o jogo não cria nenhuma thread antes da GPU), terminando com saída 3 em
`VdInitializeEngines` (a linha de chegada do M2). A tabela lista cada importação na ordem da sua primeira chamada,
com o número de chamadas até a parada. Apenas nomes, contagens e constantes do kernel; nada do jogo.

| # | Importação | Chamadas | Observação |
|---|---|---|---|
| 1 | `RtlImageXexHeaderField` | 1 | primeira chamada; chave `0x00020401` ausente -> 0 |
| 2 | `NtAllocateVirtualMemory` | 5 | heaps da XAPI/CRT |
| 3 | `KeGetCurrentProcessType` | 2 | título (1) |
| 4 | `RtlInitializeCriticalSection` | 38 | |
| 5 | `RtlEnterCriticalSection` | 1159 | daqui em diante o tempo todo |
| 6 | `RtlLeaveCriticalSection` | 1159 | |
| 7 | `XexCheckExecutablePrivilege` | 1 | privilégio 10 -> 1 |
| 8 | `XGetAVPack` | 1 | 6 (HDMI), confirmado: o jogo segue adiante |
| 9 | `RtlInitializeCriticalSectionAndSpinCount` | 14 | |
| 10 | `KeTlsAlloc` | 9 | |
| 11 | `KeTlsSetValue` | 1 | |
| 12 | `KeQuerySystemTime` | 1 | |
| 13 | `KeQueryPerformanceFrequency` | 10 | |
| 14 | `XamNotifyCreateListener` | 1 | |
| 15 | `XNotifyPositionUI` | 1 | stub |
| 16 | `XexLoadImage` | 1 | `xam.xex`, já carregado -> handle |
| 17 | `XexGetProcedureAddress` | 6 | ordinais `0xAFF`, `0xB00`, `0xB0B`, `0xB10`, `0x305`, `0x30B` do `xam.xex` que o executável não importa: `0xC0000263`, e o jogo segue |
| 18 | `RtlNtStatusToDosError` | 6 | uma por falha acima |
| 19 | `XexUnloadImage` | 1 | |
| 20 | `KeTlsGetValue` | 1 | |
| 21 | `XamUserGetSigninState` | 4 | usuários 0..3 |
| 22 | `XamUserGetSigninInfo` | 1 | |
| 23 | `XMsgStartIORequest` | 1 | app XAM `0xFE`, mensagem `0x00022005` |
| 24 | `XamLoaderGetLaunchDataSize` | 1 | sem launch data (`ERROR_NOT_FOUND`) |
| 25 | `XGetVideoMode` | 1 | stub de consulta (1280x720) |
| 26 | `MmAllocatePhysicalMemoryEx` | 1 | `0x12C0` bytes, proteção `0x404` |
| 27 | `VdInitializeEngines` | 1 | **linha de chegada do M2**: primeira importação Vd* que cria a GPU (M3) |

A estimativa anterior (40 passos) errou sobretudo no que *não* acontece antes da GPU: nenhum arquivo é aberto,
nenhuma thread é criada, nada de rede, áudio, entrada, LZX ou criptografia. As importações que faltavam para chegar
aqui foram `XexLoadImage`/`XexUnloadImage` e `XMsgStartIORequest` (Task 28).

Conselho prático para a spec: registrar cada uma das 459 com pelo menos um stub que registra no log e retorna a
falha padrão da categoria, e manter `UnimplementedImport` só para importações marcadas explicitamente como "abort"
(ex.: o conjunto M2 de lógica real enquanto não implementado). Assim cada execução avança até encontrar
*comportamento* realmente ausente, em vez de parar no primeiro nome. Manter os três estados da tabela de cobertura:
implemented / stub / missing.

## Estado no M2: XAM mínima, consultas de vídeo e sondas de hardware ausente

Registradas pela Task 26 (`src/runtime/kernel/exports/xam.cpp`, `video.cpp`, `net.cpp`; detalhes em
`docs/architecture.md`, seção "XAM mínima, vídeo e rede"). Todo o resto das categorias 6, 7, 17 a 28 continua
`missing` até o rastreio real (Task 28) mostrar que o jogo chama. A Task 28 acrescentou `XexLoadImage`/`XexUnloadImage`
(categoria 11) e `XMsgStartIORequest`/`XMsgInProcessCall`/`XMsgSystemProcessCall` (categoria 7), que o boot chama.

- **implemented**: `XamCreateEnumeratorHandle`, `XamEnumerate`, `XamGetPrivateEnumStructureFromHandle`,
  `XamGetOverlappedResult`, `XamTaskSchedule`, `XamNotifyCreateListener`, `XNotifyGetNext`,
  `XamUserGetSigninState`, `XamUserGetXUID`, `XamUserGetName`, `XamUserGetSigninInfo`,
  `XamUserReadProfileSettings`, `XamContentGetDeviceState` (sem dispositivo: `ERROR_DEVICE_NOT_CONNECTED`),
  `XamContentCreateDeviceEnumerator` (vazio), `XamContentGetLicenseMask` (licença completa),
  `XamShowDirtyDiscErrorUI` (`guest_fatal("dirty disc")`, saída 5).
- **stub**: `XNotifyPositionUI`, `XamIsUIActive` (0), `XamInputGetCapabilities` (`ERROR_DEVICE_NOT_CONNECTED`),
  `XAudioGetSpeakerConfig` (estéreo `0x00010001`), `XGetVideoMode`, `VdQueryVideoMode`,
  `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdQueryVideoFlags` (3), `XGetAVPack` (6),
  `XGetVideoCapabilities` (0), `VdIsHSIOTrainingSucceeded` (1), `NetDll_WSAStartup` (0), `NetDll_XNetStartup` (0),
  `NetDll_XNetGetEthernetLinkStatus` (0, sem link).
- **missing de propósito** (no M2): os `Vd*` que criam ou usam a GPU (`VdInitializeEngines`,
  `VdSetGraphicsInterruptCallback`, `VdInitializeRingBuffer`, `VdSwap`...): a primeira chamada a um deles é a linha de
  chegada do M2. Implementados no M3 (seção seguinte).

## Estado no M3: a GPU por trás dos Vd* (Task 10 do M3)

`src/runtime/kernel/exports/video.cpp` e `memory.cpp`, sobre `gpu::GpuSystem` (detalhes em `docs/architecture.md`,
seção "GPU emulada"). Semântica do `xboxkrnl_video.cc` / `graphics_system.cc` / `command_processor.cc` do Xenia, só
como referência.

- **implemented**: `VdInitializeEngines` (1), `VdSetGraphicsInterruptCallback` (callback(fonte, dado): fonte 0 =
  vblank, 1 = pacote `INTERRUPT`; roda no worker do kernel), `VdInitializeRingBuffer` (anel de 2^(log2 + 3) bytes no
  endereço físico; inicia a thread da GPU), `VdEnableRingBufferRPtrWriteBack` (RPTR em dwords, big-endian, no
  endereço físico), `VdSwap` (10 argumentos, os dois últimos na pilha: buffer, fetch constant, 3 desconhecidos,
  ponteiro do front buffer, formato, espaço de cor, largura, altura; enche os 64 dwords reservados de NOPs e
  escreve a fetch constant tipo 0 e um `LR_SWAP`; ponteiro inválido ou front buffer não físico é `guest_fatal`), `MmMapIoSpace` (devolve o próprio endereço, como o Xenia).
- **stub**: `VdGetSystemCommandBuffer` (`0xBEEF0000`/`0xBEEF0001`), `VdPersistDisplay` (1, e um bloco físico de 64
  bytes no ponteiro opcional), `VdInitializeScalerCommandBuffer` (destino cheio de NOPs tipo 2, devolve a contagem),
  e com 0: `VdShutdownEngines`, `VdSetSystemCommandBuffer`, `VdQuerySystemCommandBuffer`,
  `VdSetSystemCommandBufferGpuIdentifierAddress`, `VdRetrainEDRAM`, `VdRetrainEDRAMWorker`,
  `VdCallGraphicsNotificationRoutines`, `VdSetDisplayMode`, `VdSetDisplayModeOverride`, `VdEnableDisableClockGating`.
- **variáveis**: `VdGlobalDevice` e `VdGlobalXamDevice` (0), `VdGpuClockInMHz` (500), `VdHSIOCalibrationLock` (seção
  crítica, spin 10000), `KeCertMonitorData` (ponteiro nulo, o padrão do Xenia).
- Dados do jogo real (experimento de 2026-10-18, spec seção 2): `VdInitializeRingBuffer(0x1FFF4000, 12)`, write-back
  em `0x1FFFD03C`, `VdSetGraphicsInterruptCallback(0x824371B0, 0x40055980)`. Ainda não confirmados com esta
  implementação: isso é a Task 12.
- `KeLockL2`/`KeUnlockL2` (categoria 16, M3) continuam missing até o jogo chamá-los.

## Categorias (listas completas)


### 1. Memória virtual/física, pools, heap Xam

**Quantidade:** 18 | **Marco padrão:** M2 | **Exceções por importação:** `MmMapIoSpace` M3

**HLE mínima:** REAL: alocador do espaço de endereçamento do guest (reserve/commit/decommit/release, páginas de 64K/4K, flags de proteção, consulta), alocações físicas nas visões 0xA0000000/0xC0000000/0xE0000000 com alinhamento e endereço mínimo/máximo, pool = heap simples. MmGetPhysicalAddress = addr & 0x1FFFFFFF. MmMapIoSpace/MmQueryStatistics podem ser stubs. Heap Xam* = encaminhar para o pool.

`NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`, `MmGetPhysicalAddress`, `MmQueryAddressProtect`, `MmSetAddressProtect`, `MmQueryAllocationSize`, `MmQueryStatistics`, `MmMapIoSpace`, `ExAllocatePool`, `ExAllocatePoolTypeWithTag`, `ExFreePool`, `XamAlloc`, `XamAllocEx`, `XamFree`

### 2. Threads, processo, TLS, APC, estado da FPU

**Quantidade:** 26 | **Marco padrão:** M2

**HLE mínima:** REAL: ExCreateThread (thread do host + pilha do guest + bloco PCR/KTHREAD/TLS, flag suspended, wrapper da rotina de início da XAPI), KeTls* (array de slots por thread), contadores de resume/suspend, NtYieldExecution/KeDelayExecutionThread (sleep, alertável), prioridade/afinidade = registrar + sucesso, tipo de processo = valor por thread (0 idle/1 title/2 system). Fila de APC necessária para esperas alertáveis e rotinas de conclusão de IO overlapped; Ke*FloatingPointState/KeEnableFpuExceptions = no-op com sucesso. Critical region = contador de desabilitação de APC. ExRegisterTitleTerminateNotification = guardar numa lista, chamar na saída.

`ExCreateThread`, `ExTerminateThread`, `ExRegisterTitleTerminateNotification`, `KeGetCurrentProcessType`, `KeSetCurrentProcessType`, `KeTlsAlloc`, `KeTlsFree`, `KeTlsGetValue`, `KeTlsSetValue`, `KeResumeThread`, `NtResumeThread`, `NtSuspendThread`, `NtYieldExecution`, `KeDelayExecutionThread`, `KeQueryBasePriorityThread`, `KeSetBasePriorityThread`, `KeSetAffinityThread`, `KeSetDisableBoostThread`, `ObLookupThreadByThreadId`, `NtQueueApcThread`, `KiApcNormalRoutineNop`, `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeSaveFloatingPointState`, `KeRestoreFloatingPointState`, `KeEnableFpuExceptions`

### 3. Objetos de sincronização, esperas, spinlocks, IRQL/DPC, seções críticas

**Quantidade:** 29 | **Marco padrão:** M2

**HLE mínima:** REAL: objetos dispatcher vivendo na memória do GUEST (tipo/estado de sinal do X_DISPATCHER_HEADER: KEVENT, KSEMAPHORE, KMUTANT, KTIMER) mapeados para objetos do host; as versões Nt* criam objetos com handle. As esperas devem respeitar timeouts (negativo = relativo em 100 ns, positivo = absoluto), WaitAny/WaitAll, alertável, STATUS_TIMEOUT/STATUS_WAIT_n/STATUS_USER_APC. Rtl*CriticalSection opera sobre o layout RTL_CRITICAL_SECTION do guest (dispatcher header + LockCount/RecursionCount/OwningThread) - implementar nativamente com atômicos na memória do guest mais uma espera no host. Spinlocks = atômico sobre u32 do guest; IRQL = valor por thread retornado/restaurado. DPC = fila executada numa thread de trabalho do host (necessária para o callback de interrupção Vd no M3).

`KeInitializeMutant`, `KeReleaseMutant`, `KeSetEvent`, `KeResetEvent`, `KeWaitForSingleObject`, `KeWaitForMultipleObjects`, `NtCreateEvent`, `NtSetEvent`, `NtClearEvent`, `NtPulseEvent`, `NtCreateSemaphore`, `NtReleaseSemaphore`, `NtCreateTimer`, `NtSetTimerEx`, `NtCancelTimer`, `NtWaitForSingleObjectEx`, `NtWaitForMultipleObjectsEx`, `NtSignalAndWaitForSingleObjectEx`, `RtlInitializeCriticalSection`, `RtlInitializeCriticalSectionAndSpinCount`, `RtlEnterCriticalSection`, `RtlLeaveCriticalSection`, `RtlTryEnterCriticalSection`, `KfAcquireSpinLock`, `KfReleaseSpinLock`, `KeAcquireSpinLockAtRaisedIrql`, `KeReleaseSpinLockFromRaisedIrql`, `KeInitializeDpc`, `KeInsertQueueDpc`

### 4. Gerenciador de objetos e handles

**Quantidade:** 9 | **Marco padrão:** M2

**HLE mínima:** REAL (pequena): tabela de handles (handle -> objeto do host, refcount), NtClose, NtDuplicateObject, ObReferenceObjectByHandle (retorna ponteiro do guest para a struct do lado do guest do objeto; pseudo-handle 0xFFFFFFFE = thread atual), ObDereferenceObject, ObOpenObjectByPointer. ObCreateSymbolicLink/ObDeleteSymbolicLink alimentam a tabela de montagem do VFS (ex.: `\??\game:` -> `\Device\Cdrom0`; a montagem de conteúdo usa a mesma tabela). ObOpenObjectByName/ObCreateObject: raras, retornam STATUS_NOT_IMPLEMENTED até serem atingidas.

`NtClose`, `NtDuplicateObject`, `ObReferenceObjectByHandle`, `ObDereferenceObject`, `ObOpenObjectByPointer`, `ObOpenObjectByName`, `ObCreateObject`, `ObCreateSymbolicLink`, `ObDeleteSymbolicLink`

### 5. Sistema de arquivos e E/S

**Quantidade:** 15 | **Marco padrão:** M2 | **Exceções por importação:** `NtWriteFileGather` M4, `NtFlushBuffersFile` M4

**HLE mínima:** REAL: VFS mapeando game:/d:/`\Device\Cdrom0` para o diretório do pacote extraído (somente leitura), mais caminhos cache:/update:; parsing de caminho de OBJECT_ATTRIBUTES + ANSI_STRING, IO_STATUS_BLOCK, leituras síncronas e overlapped (conclusão por evento/APC), classes FILE_*_INFORMATION (Position, Standard, Basic, NetworkOpen, EndOfFile, Disposition), FileFsSizeInformation/FileFsAttributeInformation para consultas de volume, enumeração de diretórios com curingas e flag de restart. Scatter/gather = laço sobre a lista de páginas. NtDeviceIoControlFile: stub (geometria de disco só se pedida). Fsc* = no-op / contagem fixa.

`NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtReadFileScatter`, `NtWriteFile`, `NtWriteFileGather`, `NtFlushBuffersFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `NtQueryFullAttributesFile`, `NtQueryDirectoryFile`, `NtQueryVolumeInformationFile`, `NtDeviceIoControlFile`, `FscGetCacheElementCount`, `FscSetCacheElementCount`

### 6. XContent, dispositivos de save, cache Xam

**Quantidade:** 20 | **Marco padrão:** M4 | **Exceções por importação:** `XamContentGetLicenseMask` M2, `XamContentGetDeviceState` M2, `XamContentCreateDeviceEnumerator` M2, `XamContentLaunchImage` M5, `XamContentInstall` M5

**HLE mínima:** M2: XamContentGetDeviceState / XamContentCreateDeviceEnumerator / XamContentGetLicenseMask podem ser consultadas no boot -> 'nenhum dispositivo' / enumeração vazia (ERROR_NO_MORE_FILES) / licença completa. M4 REAL: pacotes de conteúdo como diretórios do host sob a raiz de dados (create/open/close por root name -> link simbólico, enumerar, apagar, miniaturas, criador = XUID local). XamContentLaunchImage/Install: stub. XamCache*: stub de falha (sem partição de cache).

`XamContentClose`, `XamContentCreateDeviceEnumerator`, `XamContentCreateEnumerator`, `XamContentCreateEx`, `XamContentDelete`, `XamContentFlush`, `XamContentGetCreator`, `XamContentGetDeviceData`, `XamContentGetDeviceName`, `XamContentGetDeviceState`, `XamContentGetLicenseMask`, `XamContentGetThumbnail`, `XamContentInstall`, `XamContentLaunchImage`, `XamContentOpenFile`, `XamContentResolve`, `XamContentSetThumbnail`, `XamCacheOpenFile`, `XamCacheCloseFile`, `XamCacheReset`

### 7. Infraestrutura assíncrona Xam: enumeradores, overlapped, tarefas, XMsg, notificações, sessões

**Quantidade:** 15 | **Marco padrão:** M2 | **Exceções por importação:** `XMsgCompleteIORequest` M5, `XMsgCancelIORequest` M5, `XamSessionCreateHandle` M5, `XamSessionRefObjByHandle` M5

**HLE mínima:** REAL (infraestrutura genérica reutilizada por conteúdo/usuário/UI): objetos de handle XAM para enumeradores (XamCreateEnumeratorHandle/XamEnumerate/XamGetPrivateEnumStructureFromHandle), conclusão de XOVERLAPPED (XamGetOverlappedResult lê InternalLow/InternalHigh, espera em hEvent), XamTaskSchedule (executa o callback numa thread de trabalho). Notificações: XamNotifyCreateListener retorna um handle de listener (um objeto esperável); XNotifyGetNext retorna FALSE, ou entrega eventos iniciais enfileirados (sign-in alterado, dispositivos de armazenamento alterados). XMsg*: só o app XAM (0xFE), com as mensagens que o Xenia responde (`XMsgStartIORequest`, `XMsgInProcessCall` e `XMsgSystemProcessCall`, implementadas na Task 28 porque o jogo as chama no boot); `XMsgCompleteIORequest`/`XMsgCancelIORequest` continuam M5. XNotifyPositionUI = no-op. Sessões: M5.

`XamCreateEnumeratorHandle`, `XamEnumerate`, `XamGetPrivateEnumStructureFromHandle`, `XamGetOverlappedResult`, `XamTaskSchedule`, `XamNotifyCreateListener`, `XNotifyGetNext`, `XNotifyPositionUI`, `XMsgStartIORequest`, `XMsgCompleteIORequest`, `XMsgCancelIORequest`, `XMsgInProcessCall`, `XMsgSystemProcessCall`, `XamSessionCreateHandle`, `XamSessionRefObjByHandle`

### 8. Strings Rtl, auxiliares de memória, printf da CRT

**Quantidade:** 18 | **Marco padrão:** M2

**HLE mínima:** REAL, mas trivial: init/free/conversão de ANSI/UNICODE_STRING (UTF-16 big-endian do guest), MultiByte<->Unicode, RtlCompareMemory(Ulong)/RtlFillMemoryUlong, RtlNtStatusToDosError (tabela). Família printf: os varargs ficam em r3..r10 e na pilha do guest (vsprintf/_vsnprintf/vswprintf/_vscwprintf recebem um ponteiro para va_list do guest) - um único interpretador de formato que tira os argumentos do contexto PPC / va_list do guest, nunca de um va_list do host.

`RtlInitAnsiString`, `RtlInitUnicodeString`, `RtlFreeAnsiString`, `RtlUnicodeStringToAnsiString`, `RtlMultiByteToUnicodeN`, `RtlUnicodeToMultiByteN`, `RtlCompareMemory`, `RtlCompareMemoryUlong`, `RtlFillMemoryUlong`, `RtlNtStatusToDosError`, `sprintf`, `_snprintf`, `vsprintf`, `_vsnprintf`, `swprintf`, `_snwprintf`, `vswprintf`, `_vscwprintf`

### 9. Exceções / SEH / contexto

**Quantidade:** 4 | **Marco padrão:** M2

**HLE mínima:** MÍNIMA: RtlCaptureContext = preencher o CONTEXT do guest a partir do PPCContext; RtlRaiseException = log + saída fatal por enquanto (o `throw` de C++ do MSVC passa por ela); RtlUnwind só é alcançada pelo longjmp quando jmp_buf+312 != 0 e pelo EH de C++; `__C_specific_handler` é referenciado pelas palavras de handler do .pdata e chamado só pelo unwinder - stub que registra no log. Desenrolamento real de SEH / EH de C++ fica fora do escopo até que algum caminho de inicialização precise dele.

`RtlCaptureContext`, `RtlRaiseException`, `RtlUnwind`, `__C_specific_handler`

### 10. Tempo / relógio

**Quantidade:** 4 | **Marco padrão:** M2

**HLE mínima:** REAL: KeQueryPerformanceFrequency = frequência do TSC do host medida na inicialização (não 50 MHz), porque o XenonRecomp traduz `mftb` para `__rdtsc()` (`thirdparty/XenonRecomp/XenonRecomp/recompiler.cpp:1263`), então a frequência informada precisa casar com a do contador que o guest lê; KeQuerySystemTime = FILETIME do host (100 ns desde 1601) escrito em BE; RtlTimeToTimeFields/RtlTimeFieldsToTime = conversão de calendário. (KeTimeStampBundle é uma exportação de DADOS e não está nesta lista.)

`KeQueryPerformanceFrequency`, `KeQuerySystemTime`, `RtlTimeToTimeFields`, `RtlTimeFieldsToTime`

### 11. Módulo XEX / loader / dados de lançamento

**Quantidade:** 13 | **Marco padrão:** M2 | **Exceções por importação:** `XexUnloadImageAndExitThread` M5, `XamLoaderSetLaunchData` M5, `XamLoaderLaunchTitle` M5

**HLE mínima:** REAL: RtlImageXexHeaderField (percorre os cabeçalhos opcionais da cópia do cabeçalho XEX na memória do guest; retorna valor ou ponteiro conforme o byte baixo da chave), XexGetModuleHandle (NULL/'default.xex' -> entrada LDR do título; 'xboxkrnl.exe'/'xam.xex' -> entradas LDR falsas), XexCheckExecutablePrivilege (bits de privilégio do cabeçalho), XexGetProcedureAddress (por ordinal -> endereço do guest de um trampolim do host; necessário se o título resolver ordinais dinamicamente), XexGetModuleSection (tabela de seções da imagem carregada). XamLoaderGetLaunchDataSize/GetLaunchData = ERROR_NOT_FOUND (sem launch data). TerminateTitle = sair. XexLoadImage/Unload: só módulos já carregados (o jogo carrega `xam.xex` no boot); carregar outro XEX, LaunchTitle, SetLaunchData: M5 (falha).

`RtlImageXexHeaderField`, `XexGetModuleHandle`, `XexCheckExecutablePrivilege`, `XexGetModuleSection`, `XexGetProcedureAddress`, `XexLoadImage`, `XexUnloadImage`, `XexUnloadImageAndExitThread`, `XamLoaderGetLaunchDataSize`, `XamLoaderGetLaunchData`, `XamLoaderSetLaunchData`, `XamLoaderLaunchTitle`, `XamLoaderTerminateTitle`

### 12. Descompressão LZX (LDI*, base do XMemDecompress)

**Quantidade:** 4 | **Marco padrão:** M2

**HLE mínima:** REAL, provavelmente necessária durante a inicialização: XMemCreateDecompressionContext/XMemDecompress(Stream) no título chamam LDI* no xboxkrnl. Os arquivos/assets comprimidos do Minecraft provavelmente passam por ela. Usar o `lzxd.c` da libmspack que já é compilado na `libXenonUtils.a` (LGPL-2.1, já coberto por `docs/adr/0002-license.md`): nenhum decodificador novo e nenhuma nova obrigação de licença. Contexto = handle alocado no guest; Reset = reinicializar a janela.

`LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression`, `LDIResetDecompression`

### 13. Criptografia e chaves

**Quantidade:** 12 | **Marco padrão:** M2 | **Exceções por importação:** `XeCryptBnQwBeSigVerify` M4, `XeKeysGetKey` M4, `XeKeysGetConsoleID` M4

**HLE mínima:** REAL para hashes (SHA-1 init/update/final + XeCryptSha de uma vez, SHA-256, XeCryptRotSumSha) - usados para assinar saves/perfis e possivelmente verificar assets; XeCryptRandom = CSPRNG do host. Stubs do M4: XeCryptBnQwBeSigVerify = TRUE (pula a verificação RSA), XeKeysGetKey/XeKeysGetConsoleID = valores falsos fixos.

`XeCryptBnQwBeSigVerify`, `XeCryptRandom`, `XeCryptRotSumSha`, `XeCryptSha`, `XeCryptSha256Final`, `XeCryptSha256Init`, `XeCryptSha256Update`, `XeCryptShaFinal`, `XeCryptShaInit`, `XeCryptShaUpdate`, `XeKeysGetConsoleID`, `XeKeysGetKey`

### 14. Configuração do sistema, locale, informações do título

**Quantidade:** 9 | **Marco padrão:** M2 | **Exceções por importação:** `XamGetActiveDashAppInfo` M5, `XamGetAppliesTo` M5, `XamReadString` M5

**HLE mínima:** Constantes: ExGetXConfigSetting (tabela de categoria/configuração: idioma, região, região AV, flags de vídeo, configurações do usuário -> padrões sensatos, senão STATUS_INVALID_PARAMETER), XGetLanguage (1 = inglês), XGetGameRegion (ex.: 0x00FF NTSC-U), XamGetCurrentTitleId (0x584111F7), XamGetExecutionId (ponteiro para o XEX_EXECUTION_ID do cabeçalho), XamGetSystemVersion (versão fixa do dashboard). XamGetActiveDashAppInfo/XamGetAppliesTo/XamReadString: stub (M5).

`ExGetXConfigSetting`, `XGetLanguage`, `XGetGameRegion`, `XamGetCurrentTitleId`, `XamGetExecutionId`, `XamGetSystemVersion`, `XamGetActiveDashAppInfo`, `XamGetAppliesTo`, `XamReadString`

### 15. Depuração, tracing, bugcheck

**Quantidade:** 7 | **Marco padrão:** M2

**HLE mínima:** TRIVIAL: DbgPrint -> log (reutiliza o interpretador de printf do guest); DbgBreakPoint -> log e continua; Etx* (event tracing, ETX) -> no-op com sucesso; KeBugCheck/KeBugCheckEx -> log + abortar com um código de saída dedicado.

`DbgPrint`, `DbgBreakPoint`, `EtxProducerRegister`, `EtxProducerUnregister`, `EtxProducerLog`, `KeBugCheck`, `KeBugCheckEx`

### 16. Hardware / HAL / controle de cache

**Quantidade:** 3 | **Marco padrão:** M2 | **Exceções por importação:** `KeLockL2` M3, `KeUnlockL2` M3

**HLE mínima:** TRIVIAL: KeLockL2/KeUnlockL2 (travam vias da L2 para streaming da GPU / XPS) -> no-op com sucesso; HalReturnToFirmware -> encerra o processo (saída do título / volta ao dashboard).

`KeLockL2`, `KeUnlockL2`, `HalReturnToFirmware`

### 17. Vídeo (Vd*, consultas de modo de vídeo)

**Quantidade:** 26 | **Marco padrão:** M3 | **Exceções por importação:** `XGetVideoMode` M2, `VdQueryVideoMode` M2, `XGetAVPack` M2, `XGetVideoCapabilities` M2, `VdQueryVideoFlags` M2, `VdGetCurrentDisplayInformation` M2, `VdGetCurrentDisplayGamma` M2, `VdIsHSIOTrainingSucceeded` M2

**HLE mínima:** M2: as chamadas só de consulta (XGetVideoMode, VdQueryVideoMode, XGetAVPack, XGetVideoCapabilities, VdQueryVideoFlags, VdGetCurrentDisplayInformation, VdGetCurrentDisplayGamma, VdIsHSIOTrainingSucceeded = 1) são stubs com respostas fixas descrevendo 1280x720 widescreen HDMI. A linha de chegada do M2 é a primeira chamada Vd* que cria ou usa a GPU. M3 REAL: ring buffer + processador de comandos, callback de interrupção (VdSetGraphicsInterruptCallback -> interrupções de vsync/swap via uma thread de DPC), buffer de comandos do sistema, VdSwap; VdPersistDisplay, VdRetrainEDRAM* e VdEnableDisableClockGating são no-ops do M3.

`VdCallGraphicsNotificationRoutines`, `VdEnableDisableClockGating`, `VdEnableRingBufferRPtrWriteBack`, `VdGetCurrentDisplayGamma`, `VdGetCurrentDisplayInformation`, `VdGetSystemCommandBuffer`, `VdInitializeEngines`, `VdInitializeRingBuffer`, `VdInitializeScalerCommandBuffer`, `VdIsHSIOTrainingSucceeded`, `VdPersistDisplay`, `VdQuerySystemCommandBuffer`, `VdQueryVideoFlags`, `VdQueryVideoMode`, `VdRetrainEDRAM`, `VdRetrainEDRAMWorker`, `VdSetDisplayMode`, `VdSetDisplayModeOverride`, `VdSetGraphicsInterruptCallback`, `VdSetSystemCommandBuffer`, `VdSetSystemCommandBufferGpuIdentifierAddress`, `VdShutdownEngines`, `VdSwap`, `XGetVideoMode`, `XGetVideoCapabilities`, `XGetAVPack`

### 18. Áudio (driver de render XAudio, XMA, ducker)

**Quantidade:** 13 | **Marco padrão:** M4

**HLE mínima:** M4 REAL: XAudioRegisterRenderDriverClient (callback a cada quadro de 256 amostras a partir de uma thread de áudio do host), XAudioSubmitRenderDriverFrame (6 canais x 256 floats BE), contextos XMA (decodificador XMA2; o Xenia usa ffmpeg). M2 sem interface: o registro retorna um id de cliente e nunca chama de volta; consultas de ducker/alto-falantes -> valores fixos; a criação de XMA pode retornar um contexto falso.

`XAudioEnableDucker`, `XAudioGetDuckerAttackTime`, `XAudioGetDuckerHoldTime`, `XAudioGetDuckerLevel`, `XAudioGetDuckerReleaseTime`, `XAudioGetDuckerThreshold`, `XAudioGetSpeakerConfig`, `XAudioGetVoiceCategoryVolume`, `XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame`, `XAudioUnregisterRenderDriverClient`, `XMACreateContext`, `XMAReleaseContext`

### 19. Entrada (gamepad, keystroke)

**Quantidade:** 4 | **Marco padrão:** M4

**HLE mínima:** M2: ERROR_DEVICE_NOT_CONNECTED (0x48F) para todo índice de usuário (ou um controle conectado em estado ocioso, se o título travar em 'press start'). M4 REAL: gamepad SDL2 -> XINPUT_STATE/CAPABILITIES (big-endian), vibração via SetState, keystroke para chat / teclado virtual.

`XamInputGetCapabilities`, `XamInputGetState`, `XamInputSetState`, `XamInputGetKeystrokeEx`

### 20. Usuários, sign-in, configurações de perfil, gamer tiles

**Quantidade:** 19 | **Marco padrão:** M2 | **Exceções por importação:** `XamUserAreUsersFriends` M5, `XamUserGetMembershipTierFromXUID` M5, `XamUserGetOnlineCountryFromXUID` M5, `XamUserCreateAchievementEnumerator` M5, `XamUserCreateStatsEnumerator` M5, `XamProfileCreateEnumerator` M4, `XamProfileEnumerate` M4, `XamParseGamerTileKey` M5, `XamReadTileToTexture` M5, `XamWriteGamerTile` M5, `XamUserWriteProfileSettings` M4

**HLE mínima:** M2: um perfil local offline conectado (XamUserGetSigninState = 1 para o usuário 0), constantes de XUID/nome/signin-info, CheckPrivilege = TRUE para privilégios locais, ReadProfileSettings retorna padrões para os IDs de configuração pedidos (blobs específicos do título vazios), GetDeviceContext/GetIndexFromXUID triviais. M4: escrita de configurações + enumeração de perfis. M5: amigos, tiers, país, enumeradores de conquistas/estatísticas (vazios), gamer tiles (falha).

`XamUserAreUsersFriends`, `XamUserCheckPrivilege`, `XamUserCreateAchievementEnumerator`, `XamUserCreateStatsEnumerator`, `XamUserGetDeviceContext`, `XamUserGetIndexFromXUID`, `XamUserGetMembershipTierFromXUID`, `XamUserGetName`, `XamUserGetOnlineCountryFromXUID`, `XamUserGetSigninInfo`, `XamUserGetSigninState`, `XamUserGetXUID`, `XamUserReadProfileSettings`, `XamUserWriteProfileSettings`, `XamProfileCreateEnumerator`, `XamProfileEnumerate`, `XamParseGamerTileKey`, `XamReadTileToTexture`, `XamWriteGamerTile`

### 21. UI do sistema (XamShow*, guide)

**Quantidade:** 26 | **Marco padrão:** M5 | **Exceções por importação:** `XamIsUIActive` M2, `XamShowDeviceSelectorUI` M4, `XamShowKeyboardUI` M4, `XamShowMessageBoxUI` M4, `XamShowMessageBoxUIEx` M4, `XamShowSigninUI` M4, `XamShowDirtyDiscErrorUI` M2

**HLE mínima:** Stub: XamIsUIActive = FALSE (M2; consultado a cada quadro). XamShow*UI: retornar ERROR_IO_PENDING e concluir o XOVERLAPPED imediatamente com ERROR_CANCELLED, sem UI. Valem implementações reais no M4: MessageBoxUI(Ex) (escolhe automaticamente o botão padrão ou overlay ImGui), KeyboardUI (nomes de mundos), DeviceSelectorUI (escolhe o único dispositivo de armazenamento do host; necessário para saves), SigninUI. DirtyDiscErrorUI: M2 (log + sair; significa que uma leitura do disco falhou).

`XamShowAchievementsUI`, `XamShowCustomMessageComposeUI`, `XamShowCustomPlayerListUI`, `XamShowDeviceSelectorUI`, `XamShowDirtyDiscErrorUI`, `XamShowFriendRequestUI`, `XamShowFriendsUI`, `XamShowGameInviteUI`, `XamShowGameVoiceChannelUI`, `XamShowGamerCardUIForXUID`, `XamShowGoldUpgradeUI`, `XamShowKeyboardUI`, `XamShowMarketplaceDownloadItemsUI`, `XamShowMarketplaceUI`, `XamShowMessageBoxUI`, `XamShowMessageBoxUIEx`, `XamShowMessageComposeUI`, `XamShowMessagesUI`, `XamShowPlayerReviewUI`, `XamShowPlayersUI`, `XamShowQuickChatUI`, `XamShowSigninUI`, `XamShowSigninUIp`, `XamShowVoiceMailUI`, `XamShowWordRegisterUI`, `XamIsUIActive`

### 22. Avatares

**Quantidade:** 13 | **Marco padrão:** M5 | **Exceções por importação:** `XamAvatarInitialize` M2

**HLE mínima:** Stub de falha (E_FAIL / ERROR_NOT_SUPPORTED). XamAvatarInitialize pode rodar no boot (o disco traz um AvatarAssetPack) -> retornar erro para que o título desative os recursos de avatar; verificar se o título tolera isso.

`XamAvatarGenerateMipMaps`, `XamAvatarGetAssets`, `XamAvatarGetAssetsResultSize`, `XamAvatarGetManifestLocalUser`, `XamAvatarGetManifestsByXuid`, `XamAvatarGetMetadataRandom`, `XamAvatarGetMetadataSignedOutProfile`, `XamAvatarGetMetadataSignedOutProfileCount`, `XamAvatarInitialize`, `XamAvatarLoadAnimation`, `XamAvatarManifestGetBodyType`, `XamAvatarSetCustomAsset`, `XamAvatarShutdown`

### 23. Voz / headset

**Quantidade:** 5 | **Marco padrão:** M5

**HLE mínima:** Stub: XamVoiceHeadsetPresent = FALSE, XamVoiceCreate falha, XamVoiceIsActiveProcess = FALSE.

`XamVoiceClose`, `XamVoiceCreate`, `XamVoiceHeadsetPresent`, `XamVoiceIsActiveProcess`, `XamVoiceSubmitPacket`

### 24. Kinect / NUI / câmera / biometria

**Quantidade:** 15 | **Marco padrão:** M5

**HLE mínima:** Stub: dispositivo ausente (XamNuiGetDeviceStatus), todas as outras com erro; PsCamDeviceRequest -> STATUS_NO_SUCH_DEVICE. Provavelmente puxadas por bibliotecas do XDK, não chamadas pelo código do título.

`XamNuiCameraElevationGetAngle`, `XamNuiCameraElevationSetAngle`, `XamNuiCameraElevationStopMovement`, `XamNuiCameraRememberFloor`, `XamNuiCameraTiltGetStatus`, `XamNuiCameraTiltReportStatus`, `XamNuiCameraTiltSetCallback`, `XamNuiGetDeviceStatus`, `XamNuiIdentityGetSessionId`, `XamUserNuiEnableBiometric`, `XamUserNuiGetEnrollmentIndex`, `XamUserNuiGetUserIndex`, `XamReadBiometricData`, `XamWriteBiometricData`, `PsCamDeviceRequest`

### 25. Rede: Winsock (NetDll_* BSD/WSA)

**Quantidade:** 35 | **Marco padrão:** M5 | **Exceções por importação:** `NetDll_WSAStartup` M2, `NetDll_WSACleanup` M2, `NetDll_WSAGetLastError` M2, `NetDll_WSASetLastError` M2

**HLE mínima:** M2: NetDll_WSAStartup/WSACleanup = 0 se chamadas no boot, Get/SetLastError reais (por thread); todo o resto SOCKET_ERROR com WSAENETDOWN. M5 REAL (jogo em LAN via System Link): mapear para sockets do host. Observação: as NetDll_* recebem um argumento 'caller' extra no início, em r3.

`NetDll_WSACancelOverlappedIO`, `NetDll_WSACleanup`, `NetDll_WSACloseEvent`, `NetDll_WSACreateEvent`, `NetDll_WSAEventSelect`, `NetDll_WSAGetLastError`, `NetDll_WSAGetOverlappedResult`, `NetDll_WSARecv`, `NetDll_WSARecvFrom`, `NetDll_WSAResetEvent`, `NetDll_WSASend`, `NetDll_WSASendTo`, `NetDll_WSASetEvent`, `NetDll_WSASetLastError`, `NetDll_WSAStartup`, `NetDll_WSAWaitForMultipleEvents`, `NetDll___WSAFDIsSet`, `NetDll_accept`, `NetDll_bind`, `NetDll_closesocket`, `NetDll_connect`, `NetDll_getpeername`, `NetDll_getsockname`, `NetDll_getsockopt`, `NetDll_inet_addr`, `NetDll_ioctlsocket`, `NetDll_listen`, `NetDll_recv`, `NetDll_recvfrom`, `NetDll_select`, `NetDll_send`, `NetDll_sendto`, `NetDll_setsockopt`, `NetDll_shutdown`, `NetDll_socket`

### 26. Rede: XNet / Xnp / XNetLogon (pilha de rede segura, QoS, chaves)

**Quantidade:** 62 | **Marco padrão:** M5 | **Exceções por importação:** `NetDll_XNetStartup` M2, `NetDll_XNetCleanup` M2, `NetDll_XNetGetEthernetLinkStatus` M2, `NetDll_XNetRandom` M2, `NetDll_XNetGetTitleXnAddr` M2

**HLE mínima:** M2: NetDll_XNetStartup/XNetCleanup = 0, XNetGetEthernetLinkStatus = 0 (sem link), XNetGetTitleXnAddr = XNET_GET_XNADDR_NONE, XNetRandom = RNG do host. Xnp* são a infraestrutura interna do XNet exportada pelo xam e puxada pelas bibliotecas estáticas XNet/XOnline (não deveriam ser alcançadas offline). M5 REAL só para System Link.

`NetDll_XNetCleanup`, `NetDll_XNetConnect`, `NetDll_XNetCreateKey`, `NetDll_XNetDnsLookup`, `NetDll_XNetDnsRelease`, `NetDll_XNetDnsReverseLookup`, `NetDll_XNetDnsReverseRelease`, `NetDll_XNetGetBroadcastVersionStatus`, `NetDll_XNetGetConnectStatus`, `NetDll_XNetGetDebugXnAddr`, `NetDll_XNetGetEthernetLinkStatus`, `NetDll_XNetGetOpt`, `NetDll_XNetGetSystemLinkPort`, `NetDll_XNetGetTitleXnAddr`, `NetDll_XNetGetXnAddrPlatform`, `NetDll_XNetInAddrToServer`, `NetDll_XNetInAddrToString`, `NetDll_XNetInAddrToXnAddr`, `NetDll_XNetQosGetListenStats`, `NetDll_XNetQosListen`, `NetDll_XNetQosLookup`, `NetDll_XNetQosRelease`, `NetDll_XNetQosServiceLookup`, `NetDll_XNetRandom`, `NetDll_XNetRegisterKey`, `NetDll_XNetReplaceKey`, `NetDll_XNetServerToInAddr`, `NetDll_XNetSetOpt`, `NetDll_XNetSetSystemLinkPort`, `NetDll_XNetStartup`, `NetDll_XNetTsAddrToInAddr`, `NetDll_XNetUnregisterInAddr`, `NetDll_XNetUnregisterKey`, `NetDll_XNetXnAddrToInAddr`, `NetDll_XNetXnAddrToMachineId`, `NetDll_XnpCapture`, `NetDll_XnpConfig`, `NetDll_XnpConfigUPnP`, `NetDll_XnpEthernetInterceptRecv`, `NetDll_XnpEthernetInterceptSetCallbacks`, `NetDll_XnpEthernetInterceptXmit`, `NetDll_XnpGetActiveSocketList`, `NetDll_XnpGetConfigStatus`, `NetDll_XnpGetVlanXboxName`, `NetDll_XnpLoadConfigParams`, `NetDll_XnpLoadMachineAccount`, `NetDll_XnpLogonClearQEvent`, `NetDll_XnpLogonGetQFlags`, `NetDll_XnpLogonGetQVals`, `NetDll_XnpLogonGetStatus`, `NetDll_XnpLogonSetPState`, `NetDll_XnpLogonSetQEvent`, `NetDll_XnpLogonSetQFlags`, `NetDll_XnpLogonSetQVals`, `NetDll_XnpNoteSystemTime`, `NetDll_XnpRegisterKeyForCallerType`, `NetDll_XnpSaveConfigParams`, `NetDll_XnpSaveMachineAccount`, `NetDll_XnpSetVlanXboxName`, `NetDll_XnpUnregisterKeyForCallerType`, `XNetLogonGetMachineID`, `XNetLogonGetTitleID`

### 27. Rede: XHttp

**Quantidade:** 22 | **Marco padrão:** M5

**HLE mínima:** Stub: XHttpStartup = FALSE (offline); toda requisição falha.

`NetDll_XHttpCloseHandle`, `NetDll_XHttpConnect`, `NetDll_XHttpCrackUrl`, `NetDll_XHttpCrackUrlW`, `NetDll_XHttpCreateUrl`, `NetDll_XHttpCreateUrlW`, `NetDll_XHttpDoWork`, `NetDll_XHttpOpen`, `NetDll_XHttpOpenRequest`, `NetDll_XHttpOpenRequestUsingMemory`, `NetDll_XHttpQueryAuthSchemes`, `NetDll_XHttpQueryHeaders`, `NetDll_XHttpQueryOption`, `NetDll_XHttpReadData`, `NetDll_XHttpReceiveResponse`, `NetDll_XHttpSendRequest`, `NetDll_XHttpSetCredentials`, `NetDll_XHttpSetOption`, `NetDll_XHttpSetStatusCallback`, `NetDll_XHttpShutdown`, `NetDll_XHttpStartup`, `NetDll_XHttpWriteData`

### 28. Xbox LIVE / online diversos (XAuth, fila de upload Xlfs, tokens, Live hive, download em segundo plano, XStudio)

**Quantidade:** 13 | **Marco padrão:** M5

**HLE mínima:** Códigos de falha em stub (offline). XamBackgroundDownloadGetMode/SetMode = 0 / sucesso (podem ser tocadas no boot). XamGetToken/XamFreeToken são tokens de autenticação internos do XAM -> falha.

`XampXAuthStartup`, `XampXAuthShutdown`, `XampXAuthGetTitleBuffer`, `XamXlfsInitializeUploadQueue`, `XamXlfsMountUploadQueueInstance`, `XamXlfsUninitializeUploadQueue`, `XamXlfsUnmountUploadQueueInstance`, `XamGetToken`, `XamFreeToken`, `XamQueryLiveHiveA`, `XamBackgroundDownloadGetMode`, `XamBackgroundDownloadSetMode`, `XamXStudioRequest`

## Importações por marco


**M2 (199):** `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`, `MmGetPhysicalAddress`, `MmQueryAddressProtect`, `MmSetAddressProtect`, `MmQueryAllocationSize`, `MmQueryStatistics`, `ExAllocatePool`, `ExAllocatePoolTypeWithTag`, `ExFreePool`, `XamAlloc`, `XamAllocEx`, `XamFree`, `ExCreateThread`, `ExTerminateThread`, `ExRegisterTitleTerminateNotification`, `KeGetCurrentProcessType`, `KeSetCurrentProcessType`, `KeTlsAlloc`, `KeTlsFree`, `KeTlsGetValue`, `KeTlsSetValue`, `KeResumeThread`, `NtResumeThread`, `NtSuspendThread`, `NtYieldExecution`, `KeDelayExecutionThread`, `KeQueryBasePriorityThread`, `KeSetBasePriorityThread`, `KeSetAffinityThread`, `KeSetDisableBoostThread`, `ObLookupThreadByThreadId`, `NtQueueApcThread`, `KiApcNormalRoutineNop`, `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeSaveFloatingPointState`, `KeRestoreFloatingPointState`, `KeEnableFpuExceptions`, `KeInitializeMutant`, `KeReleaseMutant`, `KeSetEvent`, `KeResetEvent`, `KeWaitForSingleObject`, `KeWaitForMultipleObjects`, `NtCreateEvent`, `NtSetEvent`, `NtClearEvent`, `NtPulseEvent`, `NtCreateSemaphore`, `NtReleaseSemaphore`, `NtCreateTimer`, `NtSetTimerEx`, `NtCancelTimer`, `NtWaitForSingleObjectEx`, `NtWaitForMultipleObjectsEx`, `NtSignalAndWaitForSingleObjectEx`, `RtlInitializeCriticalSection`, `RtlInitializeCriticalSectionAndSpinCount`, `RtlEnterCriticalSection`, `RtlLeaveCriticalSection`, `RtlTryEnterCriticalSection`, `KfAcquireSpinLock`, `KfReleaseSpinLock`, `KeAcquireSpinLockAtRaisedIrql`, `KeReleaseSpinLockFromRaisedIrql`, `KeInitializeDpc`, `KeInsertQueueDpc`, `NtClose`, `NtDuplicateObject`, `ObReferenceObjectByHandle`, `ObDereferenceObject`, `ObOpenObjectByPointer`, `ObOpenObjectByName`, `ObCreateObject`, `ObCreateSymbolicLink`, `ObDeleteSymbolicLink`, `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtReadFileScatter`, `NtWriteFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `NtQueryFullAttributesFile`, `NtQueryDirectoryFile`, `NtQueryVolumeInformationFile`, `NtDeviceIoControlFile`, `FscGetCacheElementCount`, `FscSetCacheElementCount`, `XamContentCreateDeviceEnumerator`, `XamContentGetDeviceState`, `XamContentGetLicenseMask`, `XamCreateEnumeratorHandle`, `XamEnumerate`, `XamGetPrivateEnumStructureFromHandle`, `XamGetOverlappedResult`, `XamTaskSchedule`, `XamNotifyCreateListener`, `XNotifyGetNext`, `XNotifyPositionUI`, `RtlInitAnsiString`, `RtlInitUnicodeString`, `RtlFreeAnsiString`, `RtlUnicodeStringToAnsiString`, `RtlMultiByteToUnicodeN`, `RtlUnicodeToMultiByteN`, `RtlCompareMemory`, `RtlCompareMemoryUlong`, `RtlFillMemoryUlong`, `RtlNtStatusToDosError`, `sprintf`, `_snprintf`, `vsprintf`, `_vsnprintf`, `swprintf`, `_snwprintf`, `vswprintf`, `_vscwprintf`, `RtlCaptureContext`, `RtlRaiseException`, `RtlUnwind`, `__C_specific_handler`, `KeQueryPerformanceFrequency`, `KeQuerySystemTime`, `RtlTimeToTimeFields`, `RtlTimeFieldsToTime`, `RtlImageXexHeaderField`, `XexGetModuleHandle`, `XexCheckExecutablePrivilege`, `XexGetModuleSection`, `XexGetProcedureAddress`, `XamLoaderGetLaunchDataSize`, `XamLoaderGetLaunchData`, `XamLoaderTerminateTitle`, `LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression`, `LDIResetDecompression`, `XeCryptRandom`, `XeCryptRotSumSha`, `XeCryptSha`, `XeCryptSha256Final`, `XeCryptSha256Init`, `XeCryptSha256Update`, `XeCryptShaFinal`, `XeCryptShaInit`, `XeCryptShaUpdate`, `ExGetXConfigSetting`, `XGetLanguage`, `XGetGameRegion`, `XamGetCurrentTitleId`, `XamGetExecutionId`, `XamGetSystemVersion`, `DbgPrint`, `DbgBreakPoint`, `EtxProducerRegister`, `EtxProducerUnregister`, `EtxProducerLog`, `KeBugCheck`, `KeBugCheckEx`, `HalReturnToFirmware`, `VdGetCurrentDisplayGamma`, `VdGetCurrentDisplayInformation`, `VdIsHSIOTrainingSucceeded`, `VdQueryVideoFlags`, `VdQueryVideoMode`, `XGetVideoMode`, `XGetVideoCapabilities`, `XGetAVPack`, `XamUserCheckPrivilege`, `XamUserGetDeviceContext`, `XamUserGetIndexFromXUID`, `XamUserGetName`, `XamUserGetSigninInfo`, `XamUserGetSigninState`, `XamUserGetXUID`, `XamUserReadProfileSettings`, `XamShowDirtyDiscErrorUI`, `XamIsUIActive`, `XamAvatarInitialize`, `NetDll_WSACleanup`, `NetDll_WSAGetLastError`, `NetDll_WSASetLastError`, `NetDll_WSAStartup`, `NetDll_XNetCleanup`, `NetDll_XNetGetEthernetLinkStatus`, `NetDll_XNetGetTitleXnAddr`, `NetDll_XNetRandom`, `NetDll_XNetStartup`, `XMsgStartIORequest`, `XMsgInProcessCall`, `XMsgSystemProcessCall`, `XexLoadImage`, `XexUnloadImage`

**M3 (21):** `MmMapIoSpace`, `KeLockL2`, `KeUnlockL2`, `VdCallGraphicsNotificationRoutines`, `VdEnableDisableClockGating`, `VdEnableRingBufferRPtrWriteBack`, `VdGetSystemCommandBuffer`, `VdInitializeEngines`, `VdInitializeRingBuffer`, `VdInitializeScalerCommandBuffer`, `VdPersistDisplay`, `VdQuerySystemCommandBuffer`, `VdRetrainEDRAM`, `VdRetrainEDRAMWorker`, `VdSetDisplayMode`, `VdSetDisplayModeOverride`, `VdSetGraphicsInterruptCallback`, `VdSetSystemCommandBuffer`, `VdSetSystemCommandBufferGpuIdentifierAddress`, `VdShutdownEngines`, `VdSwap`

**M4 (45):** `NtWriteFileGather`, `NtFlushBuffersFile`, `XamContentClose`, `XamContentCreateEnumerator`, `XamContentCreateEx`, `XamContentDelete`, `XamContentFlush`, `XamContentGetCreator`, `XamContentGetDeviceData`, `XamContentGetDeviceName`, `XamContentGetThumbnail`, `XamContentOpenFile`, `XamContentResolve`, `XamContentSetThumbnail`, `XamCacheOpenFile`, `XamCacheCloseFile`, `XamCacheReset`, `XeCryptBnQwBeSigVerify`, `XeKeysGetConsoleID`, `XeKeysGetKey`, `XAudioEnableDucker`, `XAudioGetDuckerAttackTime`, `XAudioGetDuckerHoldTime`, `XAudioGetDuckerLevel`, `XAudioGetDuckerReleaseTime`, `XAudioGetDuckerThreshold`, `XAudioGetSpeakerConfig`, `XAudioGetVoiceCategoryVolume`, `XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame`, `XAudioUnregisterRenderDriverClient`, `XMACreateContext`, `XMAReleaseContext`, `XamInputGetCapabilities`, `XamInputGetState`, `XamInputSetState`, `XamInputGetKeystrokeEx`, `XamUserWriteProfileSettings`, `XamProfileCreateEnumerator`, `XamProfileEnumerate`, `XamShowDeviceSelectorUI`, `XamShowKeyboardUI`, `XamShowMessageBoxUI`, `XamShowMessageBoxUIEx`, `XamShowSigninUI`

**M5 (194):** `XamContentInstall`, `XamContentLaunchImage`, `XMsgCompleteIORequest`, `XMsgCancelIORequest`, `XamSessionCreateHandle`, `XamSessionRefObjByHandle`, `XexUnloadImageAndExitThread`, `XamLoaderSetLaunchData`, `XamLoaderLaunchTitle`, `XamGetActiveDashAppInfo`, `XamGetAppliesTo`, `XamReadString`, `XamUserAreUsersFriends`, `XamUserCreateAchievementEnumerator`, `XamUserCreateStatsEnumerator`, `XamUserGetMembershipTierFromXUID`, `XamUserGetOnlineCountryFromXUID`, `XamParseGamerTileKey`, `XamReadTileToTexture`, `XamWriteGamerTile`, `XamShowAchievementsUI`, `XamShowCustomMessageComposeUI`, `XamShowCustomPlayerListUI`, `XamShowFriendRequestUI`, `XamShowFriendsUI`, `XamShowGameInviteUI`, `XamShowGameVoiceChannelUI`, `XamShowGamerCardUIForXUID`, `XamShowGoldUpgradeUI`, `XamShowMarketplaceDownloadItemsUI`, `XamShowMarketplaceUI`, `XamShowMessageComposeUI`, `XamShowMessagesUI`, `XamShowPlayerReviewUI`, `XamShowPlayersUI`, `XamShowQuickChatUI`, `XamShowSigninUIp`, `XamShowVoiceMailUI`, `XamShowWordRegisterUI`, `XamAvatarGenerateMipMaps`, `XamAvatarGetAssets`, `XamAvatarGetAssetsResultSize`, `XamAvatarGetManifestLocalUser`, `XamAvatarGetManifestsByXuid`, `XamAvatarGetMetadataRandom`, `XamAvatarGetMetadataSignedOutProfile`, `XamAvatarGetMetadataSignedOutProfileCount`, `XamAvatarLoadAnimation`, `XamAvatarManifestGetBodyType`, `XamAvatarSetCustomAsset`, `XamAvatarShutdown`, `XamVoiceClose`, `XamVoiceCreate`, `XamVoiceHeadsetPresent`, `XamVoiceIsActiveProcess`, `XamVoiceSubmitPacket`, `XamNuiCameraElevationGetAngle`, `XamNuiCameraElevationSetAngle`, `XamNuiCameraElevationStopMovement`, `XamNuiCameraRememberFloor`, `XamNuiCameraTiltGetStatus`, `XamNuiCameraTiltReportStatus`, `XamNuiCameraTiltSetCallback`, `XamNuiGetDeviceStatus`, `XamNuiIdentityGetSessionId`, `XamUserNuiEnableBiometric`, `XamUserNuiGetEnrollmentIndex`, `XamUserNuiGetUserIndex`, `XamReadBiometricData`, `XamWriteBiometricData`, `PsCamDeviceRequest`, `NetDll_WSACancelOverlappedIO`, `NetDll_WSACloseEvent`, `NetDll_WSACreateEvent`, `NetDll_WSAEventSelect`, `NetDll_WSAGetOverlappedResult`, `NetDll_WSARecv`, `NetDll_WSARecvFrom`, `NetDll_WSAResetEvent`, `NetDll_WSASend`, `NetDll_WSASendTo`, `NetDll_WSASetEvent`, `NetDll_WSAWaitForMultipleEvents`, `NetDll___WSAFDIsSet`, `NetDll_accept`, `NetDll_bind`, `NetDll_closesocket`, `NetDll_connect`, `NetDll_getpeername`, `NetDll_getsockname`, `NetDll_getsockopt`, `NetDll_inet_addr`, `NetDll_ioctlsocket`, `NetDll_listen`, `NetDll_recv`, `NetDll_recvfrom`, `NetDll_select`, `NetDll_send`, `NetDll_sendto`, `NetDll_setsockopt`, `NetDll_shutdown`, `NetDll_socket`, `NetDll_XNetConnect`, `NetDll_XNetCreateKey`, `NetDll_XNetDnsLookup`, `NetDll_XNetDnsRelease`, `NetDll_XNetDnsReverseLookup`, `NetDll_XNetDnsReverseRelease`, `NetDll_XNetGetBroadcastVersionStatus`, `NetDll_XNetGetConnectStatus`, `NetDll_XNetGetDebugXnAddr`, `NetDll_XNetGetOpt`, `NetDll_XNetGetSystemLinkPort`, `NetDll_XNetGetXnAddrPlatform`, `NetDll_XNetInAddrToServer`, `NetDll_XNetInAddrToString`, `NetDll_XNetInAddrToXnAddr`, `NetDll_XNetQosGetListenStats`, `NetDll_XNetQosListen`, `NetDll_XNetQosLookup`, `NetDll_XNetQosRelease`, `NetDll_XNetQosServiceLookup`, `NetDll_XNetRegisterKey`, `NetDll_XNetReplaceKey`, `NetDll_XNetServerToInAddr`, `NetDll_XNetSetOpt`, `NetDll_XNetSetSystemLinkPort`, `NetDll_XNetTsAddrToInAddr`, `NetDll_XNetUnregisterInAddr`, `NetDll_XNetUnregisterKey`, `NetDll_XNetXnAddrToInAddr`, `NetDll_XNetXnAddrToMachineId`, `NetDll_XnpCapture`, `NetDll_XnpConfig`, `NetDll_XnpConfigUPnP`, `NetDll_XnpEthernetInterceptRecv`, `NetDll_XnpEthernetInterceptSetCallbacks`, `NetDll_XnpEthernetInterceptXmit`, `NetDll_XnpGetActiveSocketList`, `NetDll_XnpGetConfigStatus`, `NetDll_XnpGetVlanXboxName`, `NetDll_XnpLoadConfigParams`, `NetDll_XnpLoadMachineAccount`, `NetDll_XnpLogonClearQEvent`, `NetDll_XnpLogonGetQFlags`, `NetDll_XnpLogonGetQVals`, `NetDll_XnpLogonGetStatus`, `NetDll_XnpLogonSetPState`, `NetDll_XnpLogonSetQEvent`, `NetDll_XnpLogonSetQFlags`, `NetDll_XnpLogonSetQVals`, `NetDll_XnpNoteSystemTime`, `NetDll_XnpRegisterKeyForCallerType`, `NetDll_XnpSaveConfigParams`, `NetDll_XnpSaveMachineAccount`, `NetDll_XnpSetVlanXboxName`, `NetDll_XnpUnregisterKeyForCallerType`, `XNetLogonGetMachineID`, `XNetLogonGetTitleID`, `NetDll_XHttpCloseHandle`, `NetDll_XHttpConnect`, `NetDll_XHttpCrackUrl`, `NetDll_XHttpCrackUrlW`, `NetDll_XHttpCreateUrl`, `NetDll_XHttpCreateUrlW`, `NetDll_XHttpDoWork`, `NetDll_XHttpOpen`, `NetDll_XHttpOpenRequest`, `NetDll_XHttpOpenRequestUsingMemory`, `NetDll_XHttpQueryAuthSchemes`, `NetDll_XHttpQueryHeaders`, `NetDll_XHttpQueryOption`, `NetDll_XHttpReadData`, `NetDll_XHttpReceiveResponse`, `NetDll_XHttpSendRequest`, `NetDll_XHttpSetCredentials`, `NetDll_XHttpSetOption`, `NetDll_XHttpSetStatusCallback`, `NetDll_XHttpShutdown`, `NetDll_XHttpStartup`, `NetDll_XHttpWriteData`, `XampXAuthStartup`, `XampXAuthShutdown`, `XampXAuthGetTitleBuffer`, `XamXlfsInitializeUploadQueue`, `XamXlfsMountUploadQueueInstance`, `XamXlfsUninitializeUploadQueue`, `XamXlfsUnmountUploadQueueInstance`, `XamGetToken`, `XamFreeToken`, `XamQueryLiveHiveA`, `XamBackgroundDownloadGetMode`, `XamBackgroundDownloadSetMode`, `XamXStudioRequest`
