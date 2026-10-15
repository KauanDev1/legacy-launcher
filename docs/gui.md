# Launcher gráfico (`legacyrecomp-gui`)

Interface gráfica (SDL2 + Dear ImGui) para a mesma instalação feita pela CLI `legacyrecomp`: escolha a
ISO do jogo, clique em **Instalar** e, quando todas as etapas estiverem concluídas, em **Jogar**.

![Launcher com a instalação concluída](assets/gui-m1.png)

## Escolher a ISO

- Arraste o arquivo `.iso` para a janela (ignorado enquanto uma instalação está em andamento);
- ou clique em **Procurar…** (ou na área tracejada), que abre o diálogo do `zenity` ou do `kdialog`
  (o botão só aparece se um dos dois estiver instalado);
- ou clique em **Digitar caminho** e digite o caminho completo (Enter confirma, Esc desfaz).

## Opções de linha de comando

| Opção | Efeito |
|---|---|
| `--root <pasta>` | Pasta de dados. Padrão: `$LEGACYRECOMP_ROOT`, senão `$XDG_DATA_HOME/legacyrecomp` (ou `~/.local/share/legacyrecomp`). |
| `--iso <arquivo>` | Preenche a ISO. |
| `--install` | Começa a instalação ao abrir. |
| `--screenshot <arquivo.bmp>` | Salva a janela num BMP e fecha (depois da instalação, se `--install` foi dado). Útil para documentação e testes sem mouse. |

Exemplo: `legacyrecomp-gui --iso ~/Jogos/minecraft.iso --install`.

## Variáveis de ambiente

- `LEGACYRECOMP_ROOT`: pasta de dados (como `--root`, que tem precedência).
- `LEGACYRECOMP_UI_SCALE`: escala da interface (0.5 a 4, ex. `1.25`). Sem ela: no Wayland e em telas
  HiDPI o próprio sistema escala a janela, e a escala fica 1; no X11 vale `GDK_SCALE` ou, na falta dele,
  `QT_SCALE_FACTOR` (1 a 4), senão 1. O DPI informado pelo X11 não é usado: é o DPI físico do monitor,
  não a escala da área de trabalho.

A janela abre com 1100×720 vezes a escala, limitada a 90% da área útil da tela.
- `LEGACYRECOMP_RESOURCES`: pasta `share/legacyrecomp` alternativa (ferramentas, template, fontes).

## Fontes

Inter (interface) e JetBrains Mono (registro), licença SIL OFL 1.1, em `assets/fonts/` (origem e SHA-256
em `assets/fonts/fonts.lock`), instaladas em `share/legacyrecomp/fonts/`. Sem elas, a GUI usa a fonte
padrão do ImGui e avisa no terminal.

## Encerrar

Fechar a janela, encerrar a sessão (SIGHUP) ou mandar SIGTERM fecham a GUI do mesmo jeito: uma instalação
em andamento é cancelada e as ferramentas que ela rodava são encerradas antes de o processo sair. As
etapas já concluídas ficam em cache.
