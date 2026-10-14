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
- `LEGACYRECOMP_UI_SCALE`: escala da interface (0.5 a 4, ex. `1.25`). Por padrão vem do DPI da tela,
  descontada a escala que o sistema já aplica à janela (Wayland). Útil no X11 quando o DPI informado
  está errado.
- `LEGACYRECOMP_RESOURCES`: pasta `share/legacyrecomp` alternativa (ferramentas, template, fontes).

## Fontes

Inter (interface) e JetBrains Mono (registro), licença SIL OFL 1.1, em `assets/fonts/` (origem e SHA-256
em `assets/fonts/fonts.lock`), instaladas em `share/legacyrecomp/fonts/`. Sem elas, a GUI usa a fonte
padrão do ImGui e avisa no terminal.
