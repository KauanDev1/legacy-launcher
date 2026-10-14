<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
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
