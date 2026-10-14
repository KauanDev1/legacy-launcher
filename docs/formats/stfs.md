# STFS (pacotes de conteúdo Xbox 360)

Implementação: `src/formats/stfs.{h,cpp}`. Testes: `tests/test_stfs.cpp` (pacotes sintéticos de `tests/support/stfs_builder.h`).

O jogo vem no disco como pacote XBLA (tipo de conteúdo `0x000D0000`) e há também um pacote de Title Update. Todos os offsets abaixo são a partir do início do pacote.

## Cabeçalho

| offset | tipo | campo |
|---|---|---|
| 0x000 | 4 bytes | magic: `"CON "`, `"LIVE"` ou `"PIRS"` |
| 0x340 | u32 BE | tamanho do cabeçalho |
| 0x344 | u32 BE | tipo de conteúdo |
| 0x360 | u32 BE | title id |
| 0x379 | u8 | tamanho do volume descriptor |
| 0x37B | u8 | flags do volume; bit 0 = formato somente leitura |
| 0x37C | u16 LE | número de blocos da tabela de arquivos |
| 0x37E | int24 LE | primeiro bloco da tabela de arquivos |
| 0x411 | 0x80 bytes | nome de exibição, UTF-16BE (termina em NUL) |

## Tabela de arquivos

Blocos de 0x1000 bytes com entradas de 0x40 bytes:

| offset | tipo | campo |
|---|---|---|
| 0x00 | 0x28 bytes | nome ASCII (preenchido com NUL) |
| 0x28 | u8 | bits 0-5 = tamanho do nome, bit 6 = blocos consecutivos, bit 7 = diretório |
| 0x29 | int24 LE | número de blocos alocados |
| 0x2F | int24 LE | primeiro bloco de dados |
| 0x32 | i16 BE | índice do pai na lista de entradas (-1 = raiz) |
| 0x34 | u32 BE | tamanho em bytes |

Uma entrada com tamanho de nome 0 encerra a tabela. Os caminhos completos (`dir/arquivo`) são montados seguindo o índice do pai; pai fora da tabela, pai que não é diretório ou ciclo resultam em `InvalidFormat`. A busca por caminho ignora maiúsculas/minúsculas.

## Bloco para offset

Os blocos de dados são intercalados com tabelas de hash: uma a cada 0xAA (170) blocos, uma de nível 1 a cada 0xAA² blocos, e assim por diante. Cada tabela ocupa 1 bloco no formato somente leitura e 2 blocos caso contrário. Fórmula (confirmada contra `StfsContainerDevice::BlockToOffsetSTFS`, em `src/xenia/vfs/devices/stfs_container_device.cc` do Xenia, e contra `blocks_per_hash_table_ = read_only_format ? 1 : 2` em `ReadHeaderAndVerify`):

```
por_tabela = somente_leitura ? 1 : 2
blk = bloco; base = 0xAA
para nivel em 0..2:
    blk += ((bloco + base) / base) * por_tabela
    se bloco < base: pare
    base *= 0xAA
offset = arredonda_para_cima(tamanho_cabecalho, 0x1000) + blk * 0x1000
```

Diferença em relação ao Xenia: nenhuma na fórmula (o Xenia expressa o fator como `blocks_per_hash_table_`, que o rascunho original escrevia como `<< shift`, com o mesmo resultado). O bit de somente leitura é o bit 0 do byte de flags (`read_only_format`). O Xenia segue a cadeia de blocos pelas tabelas de hash (`level0_next_block`) em vez de usar a flag de consecutivo; nós não lemos as tabelas de hash.

Consequência importante: mesmo um arquivo "consecutivo" cruza blocos de hash a cada 0xAA blocos, então ele não é uma janela contínua do pacote. `open_file` devolve um leitor que converte cada bloco com `block_to_offset`.

## Limitações

- Arquivos não consecutivos (e tabelas de arquivos não consecutivas) não são suportados: `open_file` devolve `InvalidFormat` com "non-consecutive STFS files are not supported yet". A tabela de arquivos é lida como blocos consecutivos a partir do primeiro bloco. Se os pacotes reais precisarem disso, será preciso seguir a cadeia das tabelas de hash (tarefa de acompanhamento).
- Os hashes não são verificados.

## Achados nos pacotes reais (Task 6)

- Pacote do jogo (`000D0000`, `PIRS`, somente leitura, title id `0x584111F7`): 525 entradas, todas consecutivas, nomes e hierarquia coerentes. O `default.xex` (20 619 264 bytes) extraído com `open_file` começa com `XEX2` e tem tamanho igual ao da entrada. O arquivo mais distante termina no bloco 28 788, antes do limite de nível 1 (28 900 = 0xAA²), então nenhum arquivo cruza a tabela de hash de nível 1. Já o `default.xex` (blocos 852 a ~5 886) cruza várias tabelas de nível 0 e foi lido corretamente.
- Title Update (`000B0000`, `PIRS`, somente leitura): 48 entradas, `default.xexp` com 4 962 304 bytes, tamanho conferido.
- A suposição de tabela de arquivos consecutiva e de entrada de nome vazio como fim da tabela funcionou nos dois pacotes. Diretórios aparecem com `start_block=0`.
