<!-- SPDX-License-Identifier: MIT -->
# XEX2 (cabeçalho)

`src/formats/xex2.{h,cpp}` lê apenas o cabeçalho de um executável Xbox 360 para inspeção e
verificação. Descriptografia e descompressão ficam com o XenonUtils.

Todos os campos são big-endian.

| Offset | Campo |
|---|---|
| 0x00 | magic `"XEX2"` |
| 0x04 | module flags |
| 0x08 | offset dos dados PE |
| 0x10 | offset das informações de segurança |
| 0x14 | quantidade de cabeçalhos opcionais |
| 0x18 | pares `{u32 chave, u32 valor}` |

Byte baixo da chave: `0x00`/`0x01` = valor inline; `0xFF` = valor é offset para um bloco que começa
com seu tamanho (u32); demais = offset para `(chave & 0xFF) * 4` bytes.

Chaves lidas: ponto de entrada `0x00010100`, base da imagem `0x00010201`, bibliotecas importadas
`0x000103FF` (tamanho, tamanho da tabela de strings, contagem, strings terminadas em NUL alinhadas a
4 bytes), informações de execução `0x00040006` (media id, versão, versão base, title id, plataforma,
tipo, disco, total de discos).

Regras: chaves desconhecidas são ignoradas; em chaves duplicadas, a última vence. Todo offset é
verificado contra o tamanho do buffer recebido (erro `OutOfRange`); magic incorreto dá
`InvalidFormat`. `format_version` usa: major = bits 31-28, minor = 27-24, build = 23-8, qfe = 7-0.

## Achados nos XEX reais (Task 6)

- `default.xexp` (patch do TU1): `module_flags=0x50`, sem ponto de entrada, base de imagem, informações de execução nem bibliotecas importadas nos cabeçalhos opcionais lidos (é um patch delta; esses campos vêm do XEX base). `pe_data_offset=0x5000`.
- `default.xex` do pacote: `module_flags=0x1`, `pe_data_offset=0x6000`.
- `default.xex` do disco: `module_flags=0x1`, `pe_data_offset=0x2000`.

## Importações, TLS e pilha (Task 14 do M2)

Chaves adicionais lidas por `parse_xex2` (`Xex2File` é um alias de `Xex2Info`):

- `0x00020104` TLS (`TlsInfo`, como `xex2_opt_tls_info` do Xenia): quantidade de slots, endereço dos dados
  brutos, tamanho dos dados, tamanho dos dados brutos (4 x u32).
- `0x00020200` tamanho padrão da pilha (valor inline). Sem ele, o loader usa 256 KiB
  (`runtime::kDefaultStackSize`).
- `0x000103FF` agora também lê os **registros** de cada biblioteca. Depois da tabela de strings vêm as
  bibliotecas em sequência, cada uma com: tamanho (u32), digest da próxima importação (0x14 bytes), id,
  versão, versão mínima (u32 cada), índice do nome na tabela de strings (u16), quantidade de registros (u16) e
  os registros (u32 cada, 0x28 bytes de cabeçalho). Cada registro é o **endereço convidado de um slot** da
  imagem; o slot contém `tipo << 24 | ordinal` (`tipo = v >> 24`, `ordinal = v & 0xFFFF`). Tipo 0 = slot de
  variável (IAT), tipo 1 = thunk de função. Seguindo o Xenia (`XexModule::SetupLibraryImports`), o registro de
  tipo 1 de uma função vem logo depois do registro de tipo 0 do mesmo ordinal. `Xex2File::imports` guarda
  `{nome, registros}` por biblioteca (nome pelo índice); `import_libraries` continua sendo a tabela de strings.
  Registros que passam do bloco dão `OutOfRange`; índice de nome inválido dá `InvalidFormat`.

### O que fica na memória do convidado

`Image::ParseImage` do XenonUtils (`xex.cpp`, laço das importações) troca a ordem dos bytes de **todo** slot de
registro no próprio buffer da imagem (`originalData->data = ByteSwap(...)`) e, só para tipo != 0, sobrescreve o
slot com o thunk `nop; nop; nop; blr` (e cria o símbolo `__imp__<Nome>`). Como o loader copia as seções desse
buffer, na memória do convidado:

- um slot de tipo 0 contém o valor do registro em ordem **do host** (lido com `memcpy` para `uint32_t`);
- um slot de tipo 1 contém o thunk (16 bytes `60000000 60000000 60000000 4E800020`) e o valor original se perde.

`runtime/xex_imports.h` (`classify_imports`, `list_imports`) reconhece os thunks pelo padrão de 16 bytes e
associa cada um ao registro de tipo 0 anterior; nomes vêm das tabelas `XboxKernelExports`/`XamExports` do
XenonUtils (as mesmas que o ParseImage usa).

### Loader v2 e variáveis do kernel

`load_image` (para XEX2, depois de `init_kernel`) copia os `pe_data_offset` bytes do cabeçalho para o início
da área de dados do kernel (`0x80000000`) e monta logo depois um `X_LDR_DATA_TABLE_ENTRY` (layout do Xenia,
`xmodule.h`; listas apontando para a própria entrada, `dll_base`/`image_base`, tamanhos, ponto de entrada,
`xex_header_base` = cópia do cabeçalho). `LoadedImage` traz `header_guest`, `ldr_entry`, `tls` e `stack_size`.

`kernel::resolve_variable_imports` (chamado pelo `main.cpp` na ordem `init_kernel` -> `load_image` ->
`resolve_variable_imports` -> tabela de funções -> glue -> execução) aloca os blocos de todas as variáveis
registradas (`register_variable`, em `src/runtime/kernel/variables.cpp`) e reescreve cada slot de tipo 0 com um
endereço convidado big-endian:

- função (há thunk depois): o endereço do thunk (o Xenia escreve `0xDEADC0DE` aqui; seguimos o plano do M2);
- variável registrada: o endereço do bloco (`kernel::variable_address(nome)`);
- qualquer outra: uma página nova `PROT_NONE` de 4 KiB na área de dados do kernel; uma falha nela é relatada
  como `SIGSEGV at guest 0x... (missing kernel variable <nome>)` (tabela fixa de `note_fault_page`, segura em
  handler de sinal).

Conteúdo das variáveis (valores do Xenia, `xboxkrnl_module.cc`): `XexExecutableModuleHandle` (4 bytes com o
endereço do `LDR_DATA_TABLE_ENTRY`), `ExLoadedCommandLine` (1 KiB com `"default.xex"`, aspas incluídas),
`ExLoadedImageName` (256 bytes zerados), `KeTimeStampBundle` (24 bytes, atualizado a cada 1 ms quando importado, ver `docs/architecture.md`), `XboxHardwareInfo`
(flags `0x20`, 6 CPUs), `XboxKrnlVersion` (`2.0xFFFF.0xFFFF.0x8000`), `KeDebugMonitorData` (ponteiro nulo) e os
`X_OBJECT_TYPE` (0x1C bytes, `xobject.h` do Xenia) de `ExThreadObjectType`, `ExEventObjectType`,
`ExSemaphoreObjectType`, `ExMutantObjectType`, `ExTimerObjectType` e `IoFileObjectType` (só o endereço importa;
o `pool_tag` em ASCII é escolha nossa).

`legacyrecomp inspect --imports <xex>` imprime uma linha por registro de tipo 0: `biblioteca 0xORDINAL nome
tipo`, com tipo `function` ou `variable` (`?` para ordinal desconhecido). `legacyrecomp-game --coverage --image
<xex>` acrescenta as importações de variável como linhas `| nome | variable | estado |`, com o estado dado no
registro (`register_variable(..., ImportState)`, padrão `implemented`; `stub` para um bloco que existe mas não é mantido
atualizado; não registrada = `missing`). A linha final conta funções e variáveis separadamente
(spec §5.3): `Implemented: a, stub: b, missing: c / N functions + M variables`.
