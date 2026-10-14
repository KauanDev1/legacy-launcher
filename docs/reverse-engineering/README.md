# Engenharia reversa: achados iniciais

Resultado de `legacyrecomp inspect` sobre a ISO XGD2 real (partição XDVDFS no offset `0xFD90000`). Apenas nomes, tamanhos, versões e hashes; nenhum dado do jogo.

## Estrutura do disco

`$SystemUpdate/`, `$TitleUpdate/584111F7/tu00000001_00000000` (STFS, TU1), `AvatarAssetPack`, `Content/0000000000000000/584111F7/000D0000/49AAD81B9FCDA45E4A03D71BFCB353F8FADB236C58` (STFS, jogo), `Default.xex`, `media/`, `nxeart`.

## Os três executáveis

| Arquivo | Tamanho | SHA-256 | Entrada | Base | Título | Versão / base | Importações |
|---|---|---|---|---|---|---|---|
| Disco `Default.xex` | 1 613 824 | `43d21981ee1cde7282da0e3650f9faec21384443db7542283505104a57f1fbd1` | `0x8214AB10` | `0x82000000` | `0x4D530A81` | 0.0.0.5 / 0.0.0.5 | `xam.xex`, `xboxkrnl.exe` |
| Pacote `default.xex` | 20 619 264 | `6d79bb823091890dabbe586e15c0744f707565c980ad998eaf43c808731308d1` | `0x822B6EA8` | `0x82000000` | `0x584111F7` | 0.0.0.1 / 0.0.0.1 | `xam.xex`, `xboxkrnl.exe` |
| TU `default.xexp` | 4 962 304 | `51dc6789fb23a3d5643db2cfdb506f5038f84b15d44e2f9d2ee8f07d3944d20e` | n/d (patch) | n/d | n/d | n/d | nenhuma nos cabeçalhos |

## Conclusões

- O `Default.xex` do disco é um carregador (stub): tem 1,6 MB contra 20,6 MB do jogo, outro ponto de entrada, outro title id (`0x4D530A81`, não o do Minecraft) e outra versão. O código do jogo está no `default.xex` dentro do pacote `000D0000`, que o carregador executa.
- O jogo real (pacote) e o carregador importam as mesmas duas bibliotecas: `xam.xex` e `xboxkrnl.exe`.
- O TU1 traz um `default.xexp` (patch delta, flags `0x50`) que se aplica ao XEX do pacote; a imagem final depende de aplicar o patch (Task 10).
- Versão do XEX do pacote: 0.0.0.1 (base 0.0.0.1). O cabeçalho do xexp não expõe versão nos campos lidos.

As fontes e os hashes estão fixados em `config/minecraft-tu1/game.toml`.
