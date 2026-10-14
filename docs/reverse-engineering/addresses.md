# Endereços da imagem TU1 usados pelo XenonRecomp

Imagem: `work/image/default_tu1.xex`, produzida pela etapa `patch` (XenonUtils `XexPatcher` sobre o
`default.xex` do pacote + `default.xexp` do TU1). SHA-256 em `config/minecraft-tu1/game.toml`
(`[hashes] patched_image`). Somente endereços, nomes, tamanhos e contagens; nenhum byte do jogo.

## Layout carregado (XenonUtils `Image::ParseImage`)

`legacyrecomp inspect --sections work/image/default_tu1.xex`:

| Seção | Base | Tamanho | Tipo |
|---|---|---|---|
| `.rdata` | `0x82000400` | `0x001B59F4` | dados |
| `.pdata` | `0x821B5E00` | `0x00046008` | dados |
| `.text` | `0x82200000` | `0x0096428C` | código |
| `.data` | `0x82B70000` | `0x000FEB20` | dados |
| `.XBMOVIE` | `0x82C6EC00` | `0x0000000C` | dados |
| `.NSPCH` | `0x82C6EE00` | `0x00000004` | dados |
| `.idata` | `0x82C70000` | `0x000007E0` | dados |
| `.XBLD` | `0x82C80000` | `0x000001F0` | dados |
| `.reloc` | `0x82C80200` | `0x00098CDC` | dados |

Base `0x82000000`, tamanho `0x032D0000`, entrada `0x82412DD0` (o TU1 muda a entrada; o `default.xex`
sem patch declara `0x822B6EA8`). 459 thunks de importação (`xam.xex` e `xboxkrnl.exe`).

### O que aprendemos sobre `XexPatcher` e `ParseImage`

- `XexPatcher::apply(xex, xexp, out, skipData=false)` devolve um XEX2 completo **sem criptografia e sem
  compressão** (53 309 440 bytes). O resultado é determinístico (mesmo SHA-256 em execuções repetidas),
  e é esse arquivo que `XenonAnalyse`, `XenonRecomp` e `legacyrecomp inspect` consomem.
- `ParseImage` precisa do XEX inteiro (verifica a assinatura `XEX2`; não confere tamanhos). Ele
  reescreve cada thunk de importação com `nop; nop; nop; blr` e cria um símbolo `__imp__<Nome>`
  (ordinais resolvidos pelas tabelas de `xam.xex`/`xboxkrnl.exe`) — o dump de `--dump-image` contém
  essas reescritas.
- Os arquivos `.xex` são criptografados/comprimidos: buscas por padrão de bytes devem rodar no dump da
  imagem carregada, nunca no `.xex`.

## Funções auxiliares e CRT

Ferramentas: `legacyrecomp inspect --dump-image <xex> <dump.bin> > layout.txt` e
`python3 -I tools/find_helpers.py <dump.bin> --layout layout.txt`. O script gera a sequência
**completa** esperada de cada auxiliar (todas as instruções até o `blr`) e conta as referências
`bl`/`b` vindas de fora dela. Conferência manual com `llvm-objdump --triple=powerpc64` sobre o dump
(envolvido em ELF com `llvm-objcopy -I binary`). Ghidra + XEXLoaderWV não estava disponível nesta
máquina: a coluna "Confirmado por" diz exatamente o que foi verificado.

| Chave (`recomp.toml`) | Endereço | Como foi achado | Confirmado por |
|---|---|---|---|
| `savegprlr_14_address` | `0x828AEB00` | único candidato; sequência completa `std r14..r31,-0x98..-0x10(r1)`, `stw r12,-8(r1)`, `blr` (0x50 bytes) | 13 125 `bl` para dentro dela; disassembly |
| `restgprlr_14_address` | `0x828AEB50` | único candidato; `ld r14..r31`, `lwz r12,-8(r1)`, `mtlr r12`, `blr` (0x54 bytes); começa logo após `savegprlr_14` | 15 374 `b` (chamadas de cauda) para dentro dela |
| `savefpr_14_address` | `0x828AF240` | único candidato; `stfd f14..f31,-0x90..-0x8(r12)`, `blr` | 650 `bl` |
| `restfpr_14_address` | `0x828AF28C` | único candidato; `lfd f14..f31`, `blr`; começa logo após `savefpr_14` | 745 `bl` |
| `savevmx_14_address` | `0x828AF350` | único candidato; pares `li r11,-0x120+16k` / `stvx v14+k,r11,r12` até v31, `blr` | 0 referências (não usada pelo jogo, mas o XenonRecomp exige a chave); layout relativo idêntico ao exemplo do README |
| `savevmx_64_address` | `0x828AF3E4` | único candidato; `li r11,-0x400+16k` / `stvx128 v64+k,r11,r12` até v127, `blr`; = `savevmx_14 + 0x94` | 44 `bl` |
| `restvmx_14_address` | `0x828AF5E8` | único candidato; mesmo formato com `lvx`; = `savevmx_64 + 0x204` | 0 referências (idem `savevmx_14`) |
| `restvmx_64_address` | `0x828AF67C` | único candidato; mesmo formato com `lvx128`; = `restvmx_14 + 0x94` | 44 `bl` |
| `longjmp_address` | `0x828B70F0` | uma das 3 funções que chamam `__imp__RtlUnwind` (`0x82B63FEC`); entrada do `.pdata` (tamanho `0x314`). Lê `jmp_buf` (r3) e o valor (r4, 0 vira 1), restaura f14–f31 de `0..136(r3)`, GPRs, `mtlr` de `308(r3)`, `r1` de `144(r3)`, `mtcr` de `304(r3)`; se `312(r3)` ≠ 0 desenrola com `RtlUnwind` | 23 referências; disassembly |
| `setjmp_address` | `0x828B7410` | logo após `longjmp` (o intervalo `0x828B7404..0x828B740F` é preenchimento), como o README prevê. Grava f14–f31 em `0..136(r3)`, `r1` em `144(r3)`, CR em `304(r3)`, LR em `308(r3)`, zera `312(r3)` e retorna 0 — os mesmos deslocamentos que `longjmp` lê. Folha sem entrada no `.pdata` | 13 `bl` |

As outras duas chamadoras de `RtlUnwind` (`0x828C50A0`, `0x828CA770`) são do tratamento de exceções
C++ (desenrolamento de quadros), não `longjmp`.

## Dados entre funções (`invalid_instructions`)

A análise de lacunas do XenonRecomp (código não coberto pelo `.pdata` nem por alvos de `bl`) tratava como
código os 8 bytes que precedem cada função com `ExceptionFlag` no `.pdata` (endereço do handler + dado do
handler). Em `0x822805F0` isso criou uma "função" falsa sobreposta a `0x822805F8`, cujo fim caía numa tabela
de saltos embutida em `0x822809A4`; a análise dessa tabela como código entrou em laço e o processo foi morto
pelo OOM killer (> 8 GiB, sem saída). Correção: pular essas palavras.

| `data` | `size` | Evidência |
|---|---|---|
| `0x00000000` | 4 | preenchimento entre funções (como no exemplo do README) |
| `0x828B0CD0` | 8 | handler C++ antes de 4 021 das 4 116 funções com `ExceptionFlag` |
| `0x82B62F1C` | 8 | thunk `__imp____C_specific_handler`, antes de 91 funções |
| `0x828C5170` | 8 | handler próprio (função do `.pdata`, `0x28` bytes) antes de `0x828C514C` |

(As outras três funções com `ExceptionFlag` são as próprias auxiliares `restgprlr_14`, `restfpr_14` e
`restvmx_14`, que não têm handler antes delas.)

## Tabelas de salto (`switch`) e funções folha

O `XenonAnalyse` (padrões fixos `lis, addi, rlwinm, lwzx, mtctr, bctr` etc.) **não encontra nenhuma**
tabela neste executável: o compilador agenda as instruções em outra ordem (`lis, slwi, addi, lwzx`) e
insere `nop` entre elas. Sem tabela, o XenonRecomp traduz o `bctr` como chamada indireta, o que falha em
tempo de execução. Por isso a etapa `analyse` roda também `src/pipeline/switch_scan.cpp`:

- Para cada `bctr`, procura para trás a guarda `cmplwi crN, rI, max` + `bgt crN` (ou `bgtlr`), e avalia o
  código linear entre ela e o `bctr` para cada índice `0..max` (lê a tabela da imagem). Independe da
  ordem das instruções e dos `nop`; instruções que não tocam a cadeia (FPU, VMX/VMX128, stores) são
  ignoradas; cópias do índice com `mr` antes da guarda são seguidas.
- Sem guarda, aceita um índice limitado por máscara (`clrlwi`/`rlwinm ..., 31`/`andi.`) com `2^k` entradas.
- Resultado na imagem TU1: **222 tabelas** (7 298 rótulos, até 440 casos), todas com rótulos em código.

Funções folha (sem quadro de pilha) não têm entrada no `.pdata`, e a análise do XenonRecomp para no
`bctr` — os rótulos dos casos caíam "fora da função" (1 329 erros `Switch case ... jump outside function`
em 46 tabelas). `fit_switch_functions` deriva a extensão delas: início = alvo de `bl` mais próximo (ou a
primeira palavra não nula após a função anterior do `.pdata`), fim = primeiro alvo de `bl` após o último
rótulo, senão a próxima função do `.pdata` (descontando os 8 bytes de handler e o preenchimento). Na TU1:
**38 funções folha**, gravadas em `work/analysis/functions.toml` e mescladas em `[main] functions` pela
etapa `recompile` (entradas escritas à mão em `recomp.toml` têm prioridade); 31 das 38 começam num alvo de
`bl`.

Verificações de segurança (`fit_switch_functions`): uma faixa derivada é rejeitada — e suas tabelas
descartadas — se sobrepõe outra faixa derivada ou uma entrada `functions` escrita à mão, se alguma chamada
(`bl`/`bcl`) cai estritamente dentro dela, ou se algum desvio vindo de fora dela cai no seu interior (sinal
de que engoliu outra função ou um final compartilhado). Na TU1: **0 faixas rejeitadas**. Tabelas cujos
rótulos saem da própria função do `.pdata` também são descartadas (não "consertadas").

Contagem final na TU1: 222 tabelas encontradas, **221 mantidas**, 1 descartada (`0x8243C83C`), 38 funções
folha, 0 faixas rejeitadas.

### Pendências para revisão humana (Ghidra + XEXLoaderWV)

Listadas também em `work/logs/analyse.log` a cada execução.

| Endereço do `bctr` | Situação | O que verificar |
|---|---|---|
| `0x8243C83C` | descartada: índice limitado por `clrlwi ..., 26` (64 entradas), mas parte dos rótulos cai na função seguinte (`0x8243C9B0`) | o número real de casos (a tabela de bytes em `0x8202F558` provavelmente tem menos de 64 entradas válidas); depois, `[[switch]]` manual |
| `0x826796FC` | guarda `cmpw` com sinal: muito provavelmente chamada virtual num vetor 2D, não `switch` | confirmar que não é tabela |
| `0x8235B7AC`, `0x82366274`, `0x82366544`, `0x825495A0`, `0x82549798`, `0x82615C18`, `0x8261F07C`, `0x826282C0`, `0x8265827C`, `0x826582B4`, `0x826A6818`, `0x826A6910`, `0x826F92B4`, `0x82787348`, `0x82787370`, `0x82787398`, `0x8294E434`, `0x82A1A6C4`, `0x82AB1528`, `0x82B1AA00`, `0x82B1CA68` | sem guarda nem máscara, com carga indexada de endereço montado por `lis` | separar `switch` sem limite (ex.: `0x8235B7AC` e `0x82A1A6C4`, tabelas de deslocamentos de código) de chamadas por vetor de ponteiros (ex.: `0x82787348`, `0x82B1AA00`); para os `switch`, achar o número de casos e escrever `[[switch]]` manual |

## Resultado do XenonRecomp

Com esta configuração o XenonRecomp termina com código 0 e **nenhuma linha `ERROR:`** (a etapa `recompile`
falha se houver qualquer uma, porque o XenonRecomp sai com 0 mesmo quando gera código errado, p.ex. um caso
de `switch` fora da função vira `return;`): `PPC_IMAGE_BASE 0x82000000`,
`PPC_IMAGE_SIZE 0x32D0000`, `PPC_CODE_BASE 0x82200000`, `PPC_CODE_SIZE 0x96428C`; 219 arquivos
(215 `ppc_recomp.*.cpp`, ~162 MB), 55 343 símbolos (459 importações `__imp__*`), 221 `switch`.

Avisos que restam (não fatais, afetam a fase de runtime): 3 279 "Unrecognized instruction" — instruções
que o XenonRecomp `ddd128b` ainda não implementa (as mais frequentes: `bdzf` 754, `stfsu` 507, `lfsu` 350,
`vsel128` 253, `lhzu` 219, `sthu` 211, `eqv` 94, `subfze` 65, `bso` 65, `lfsux` 63) — e 20
"Unexpected float16_4 pack instruction". As funções afetadas ficam incompletas no C++ gerado.
