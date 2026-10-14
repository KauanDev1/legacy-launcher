# XDVDFS (sistema de arquivos de discos Xbox 360)

Implementação: `src/formats/xdvdfs.{h,cpp}`. Testes: `tests/test_xdvdfs.cpp` (imagens sintéticas geradas por `tests/support/xdvdfs_builder.h`).

## Estrutura

- Setor = 2048 bytes.
- A imagem pode conter a partição de jogo em offsets diferentes. Sondamos, em ordem: `0x0` (XISO simples), `0xFD90000` (XGD2), `0x2080000` (XGD3), `0x18300000` (XGD1).
- Uma partição é válida quando os 20 bytes em `partição + 32*2048` são `MICROSOFT*XBOX*MEDIA`.
- Nesse setor de cabeçalho: setor do diretório raiz = `u32 LE @0x14`, tamanho do diretório raiz = `u32 LE @0x18`.
- Setores de arquivos/diretórios são relativos ao início da partição.

## Diretório

Árvore binária de entradas (cabeçalho de 14 bytes, little-endian):

| campo | tipo | nota |
|---|---|---|
| left | u16 | offset em unidades de 4 bytes desde o início do diretório; 0 = nenhum |
| right | u16 | idem |
| sector | u32 | setor inicial (relativo à partição) |
| size | u32 | tamanho em bytes |
| attrs | u8 | `0x10` = diretório |
| name_len | u8 | tamanho do nome |
| name | bytes | nome (Latin-1), sem terminador |

- Entradas são alinhadas a 4 bytes e nunca cruzam o limite de um setor; o resto do setor é preenchido com `0xFF`.
- Uma entrada com `left == 0xFFFF` é preenchimento (também indica diretório vazio na raiz).
- A raiz da árvore está no offset 0. A ordenação não é confiável: percorremos a árvore inteira (in-order, iterativo).
- Defesas: conjunto de visitados (ciclo gera `InvalidFormat`), verificação de `offset*4 + 14 + name_len <= tamanho do diretório` e do diretório dentro da imagem (`OutOfRange` se truncada).
- Busca de caminhos é case-insensitive (ASCII), separador `/`.

## Achados da ISO real

- Layout XGD2: partição do jogo em `0xFD90000`, title id `584111F7`.
- A raiz contém:
  - `$SystemUpdate/`
  - `$TitleUpdate/584111F7/tu00000001_00000000` (TU1, pacote STFS de ~30 MB)
  - `AvatarAssetPack`
  - `Content/0000000000000000/584111F7/000D0000/<nome de 40 hex>` (pacote STFS XBLA de ~118 MB com o jogo de fato)
  - `Default.xex` (1,6 MB, provavelmente um loader de disco)
  - `media/`
  - `nxeart`
