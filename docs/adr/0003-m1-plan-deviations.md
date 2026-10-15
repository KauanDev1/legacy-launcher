# ADR 0003: Desvios do plano do M1

- Status: aceita
- Data: 2026-10-14

Registro de cada decisão que se afastou do plano (`docs/superpowers/plans/2026-10-14-legacyrecomp-m1.md`) ou
do spec, ou que preencheu uma lacuna dele. Formato: o quê, por quê, custo se estiver errada.

## Desvios declarados no plano (restrições globais)

### D1. SDL2 é dependência do sistema, não submódulo
- Por quê: SDL2 é empacotado em todas as distros alvo e compilá-lo junto aumentaria muito o build. Vale
  para a instalação a partir do código-fonte. O AppImage (Task 17) é a exceção: compila o SDL2 2.32.10 a
  partir do fonte fixado, **estático** (`toolchain/sdl2/`: headers, `libSDL2.a` e o `SDL2Config.cmake`), e a
  GUI e o jogo ligam o SDL2 estaticamente; nenhum `.so` do SDL é embutido nem exigido (ver R26).
- Custo se errado: quem não tem SDL2 precisa instalá-lo; trocar por submódulo é uma mudança de CMake.

### D2. Estado do pipeline em `state.toml`, não `state.json`
- Por quê: o projeto já usa tomlplusplus (do XenonRecomp) e o formato é legível e editável à mão.
- Custo: nenhum; é só o formato do arquivo.

### D3. Seleção de arquivo na GUI por arrastar e soltar + `zenity`/`kdialog`
- Por quê: SDL2 não tem diálogo de arquivo. O botão "Procurar…" só aparece se um dos dois existe; há
  também campo de texto.
- Custo: sem `zenity`/`kdialog` o usuário arrasta o arquivo ou digita o caminho.

### D4. Caminhos e hashes do jogo em `config/minecraft-tu1/game.toml` (não `hashes.toml`)
- Por quê: o mesmo arquivo descreve fontes (caminhos dentro da ISO/STFS) e hashes esperados.
- Custo: nenhum.

### D5. Memória do convidado é um único mapeamento RW `MAP_NORESERVE` (não PROT_NONE + commit manual)
- Por quê: o kernel confirma páginas sob demanda; o código recompilado acessa a memória sem verificação,
  então o mapeamento único é mais simples e rápido.
- Custo: acessos fora do que o jogo deveria tocar não geram falha imediata; a mitigação é o handler de
  falhas e, no futuro, guard pages.

### D6. O gerador de stubs de importação lê `ppc_recomp_shared.h`, não a tabela de importação do XEX
- Por quê: o XenonRecomp já resolveu os nomes (ordinais para `__imp__<Nome>`); reler o XEX duplicaria isso.
- Custo: depende do formato do cabeçalho gerado; o teste do gerador cobre mudanças.

## Rulings durante a execução

### R1. Um único branch `feat/m1` para todo o plano
- Por quê: um marco, um PR.
- Custo: nenhum (renomear o branch).

### R2. `main.cpp` do runtime não é biblioteca pré-compilada: é copiado como fonte e compilado no template do jogo
- Por quê: ele usa `PPC_IMAGE_BASE`, `PPC_CODE_SIZE` e `PPCFuncMappings`, que só existem depois do
  `recompile` e não são conhecidos quando o launcher é compilado.
- Custo: pequena mudança no template se o contrato mudar.

### R3. `extract` sem `--iso` reaproveita o fingerprint armazenado
- O quê: sem `Options.iso`, se `extract` está completo no `state.toml`, o fingerprint devolvido é o guardado
  (etapa em cache); se não está completo, erro `NotFound` ("run `legacyrecomp install --iso <file>` first").
- Por quê: permitir `legacyrecomp build` sem ISO. Custo: pequeno (toca `extract_stage`).

### R4. Precedência da pasta de dados: `--root` > `LEGACYRECOMP_ROOT` > padrão XDG
- Por quê: CLI e GUI precisam do mesmo comportamento. Custo: nenhum.

### R5. Leitura STFS mapeia bloco a bloco
- Por quê: as tabelas de hash intercalam a cada 0xAA blocos; uma janela linear corromperia arquivos com mais
  de 170 blocos. Custo se errado: extração corrompida (pego pelo hash do teste de integração).

### R6. Módulo do pipeline em `lr::pipeline`
- Por quê: restrições globais pedem esse namespace; o brief não dizia. Custo: só renomeação.

### R7. Carga de `state.toml` é tolerante
- O quê: chave ausente ou com tipo errado significa "não completo" (re-executa); só TOML ilegível ou
  `stages` que não é tabela é erro, e a mensagem nomeia a chave.
- Por quê: re-executar é sempre seguro. Custo: um estado corrompido re-executa etapas em vez de falhar.

### R8. Ferramentas externas rodam em grupo de processos próprio; cancelar mata o grupo
- O quê: SIGTERM, depois SIGKILL após 3 s. As ferramentas não recebem mais o Ctrl+C do terminal, então o
  handler de SIGINT da CLI/GUI marca `cancelled`.
- Por quê: sem isso filhos órfãos continuariam compilando. Custo: Ctrl+C difere de um shell (documentado).
- Complemento (revisão final): a CLI trata SIGTERM e SIGHUP como SIGINT. `run_tool` publica o pgid da
  ferramenta em execução num `std::atomic<pid_t>` (`lr::pipeline::current_tool_pgid()`); no segundo sinal o
  handler faz `kill(-pgid, SIGKILL)` antes de restaurar `SIG_DFL` e re-levantar o sinal, então fechar o
  terminal ou um Ctrl+C duplo não deixa o build órfão.

### R9. `extract` rejeita caminhos STFS com `..` ou `/` inicial; `game.toml` fica ao lado do `recomp.toml`
- Por quê: evitar path traversal a partir de um pacote malicioso. (A parte "doctor sempre re-executa" foi
  substituída pelo R19.) Custo: pequeno.
- Complemento (revisão final): também rejeita `\`, NUL e bytes de controle (< 0x20); a mensagem de erro
  mostra esses bytes escapados (`\xNN`).

### R10. O fingerprint de `verify` inclui os hashes esperados
- Por quê: mudar `game.toml` precisa invalidar a verificação. Custo: nenhum.

### R11. `extract` apaga `game/` antes de extrair e valida todos os caminhos antes
- Por quê: extração parcial anterior não pode contaminar a nova.
- Custo: arquivos do usuário colocados em `game/` são apagados. `game/` pertence ao LegacyRecomp
  (documentado em `docs/pipeline.md`).

### R12. A busca de padrões de bytes dos helpers roda na imagem carregada
- O quê: novo `inspect --dump-image` (XenonUtils `Image::ParseImage`); os padrões `vmx_64` foram corrigidos
  a partir do README do XenonRecomp.
- Por quê: o `.xex` é comprimido e criptografado, então buscar nele não funciona. Custo: nenhum (ferramental).

### R13. Endereços dos helpers aceitos por evidência estrutural, sem Ghidra
- O quê: correspondências únicas da sequência completa mais contagem de referências.
- Por quê: não havia Ghidra disponível durante a execução. A confirmação no Ghidra segue como item humano
  aberto (`docs/roadmap.md`).
- Custo: um endereço errado compila o prólogo/epílogo de forma errada e quebra em tempo de execução
  (detectável no M2).

### R14. `install_function_table` devolve `Result<void>` e recebe `code_size` e `mappings`
- Por quê: validar faixa, alinhamento e que cada slot cai dentro de um mapeamento. Substitui a assinatura do
  plano. Custo: nenhum.

### R15. `main.cpp` verificado por um alvo só de testes, `lr_fake_game`
- O quê: cabeçalhos gerados falsos; `run_guest` recebe `PPCFunc*`; stubs usam `PPC_WEAK_FUNC`; `--headless`
  não inicializa SDL.
- Por quê: testar o `main.cpp` sem o jogo real. Custo: pequeno retrabalho se o contrato gerado mudar.

### R16. O runtime pré-compilado nunca toca `PPCContext`
- O quê: `run_guest(mem, body(base, user), user)` é agnóstico ao layout; o contexto e a chamada de entrada
  ficam no `main.cpp` compilado com o jogo.
- Por quê: o layout do `PPCContext` depende do `ppc_config.h` gerado. Custo: nenhum.

### R17. KPCR mínimo, verificado contra o `X_KPCR` do Xenia
- O quê: `tls_ptr` 0x00, `pcr_ptr` 0x30, base/fim da pilha 0x70/0x74, `current_thread` 0x100 apontando para
  um KTHREAD falso em 0x7FFD0000, `current_cpu` 0x10C. TLS e KTHREAD completos ficam para o M2.
- Custo: o jogo lê um campo errado do PCR e cai cedo (visível ao rodar).

### R18. Bibliotecas do runtime no pacote são uma variante sem sanitizers
- O quê: `lr_runtime_core` (+ XenonUtils) sem ASan/UBSan, independente do tipo de build do launcher; o
  template compila em Release -O2 `-fno-strict-aliasing -fexceptions` com as flags SIMD que o XenonRecomp
  exige.
- Por quê: o jogo compilado não pode herdar sanitizers do build de desenvolvimento. Custo: falhas de link,
  pegas pelo teste ponta a ponta.

### R19. O fingerprint do `doctor` é constante (`doctor-1`)
- O quê: substitui a parte "doctor sempre re-executa" do R9. Um nonce marcava todas as etapas seguintes como
  sujas e quebrava o cache.
- Custo: mudanças de ambiente não são rechecadas em execuções em cache; a perda de uma ferramenta ainda é
  pega quando uma etapa que a usa roda.

### R20. Mantida a flag `-march=sandybridge`; o `doctor` exige AVX
- O quê: um SIGILL no host gera uma dica no handler de falhas.
- Custo: CPUs sem AVX não jogam até existir um build alternativo.

### R21. `extract` sem ISO falha já no `fingerprint()` se não estiver completo com saídas presentes
- O quê: `--force extract` sem `--iso` é erro de uso.
- Por quê: `mark_started` apagaria o fingerprint guardado. Custo: nenhum.

### R22. O AppImage com toolchain embutido entra no M1 como Task 17 (decisão do usuário)
- O quê: zig c++ mirando glibc 2.31 + cmake/ninja estáticos + SDL2; plano B: LLVM com sysroot.
- Custo: se o Zig não compilar o código gerado, o spike troca para o plano B.

### R23. Redesenho da GUI (Task 15b, pedido do usuário)
- O quê: fontes Inter e JetBrains Mono (OFL) em `assets/fonts`, tema escuro neutro com acento verde,
  cabeçalho, cartões, indicador de etapas, HiDPI, captura sem o nome de usuário real.
- Custo: gosto de design; o usuário itera.

### R24. Licença do projeto muda de GPL-3.0 para MIT (decisão do usuário)
- O quê: verificado que não há código derivado de GPL no repositório e que as dependências são permissivas.
  Consequência: o UnleashedRecomp (GPL) é só referência, nunca se copia código dele. Ver ADR 0002.
- Custo: relicenciar de novo é mecânico.

### R25. Parar de empacotar `libdisasm.a` e adicionar um teste "tripwire" para `decode_insn_ppc`
- Por quê: o desassemblador GPL do XenonRecomp não é usado; ficar fora dos binários evita obrigações de GPL.
  O Task 17 precisa distribuir `THIRD-PARTY-LICENSES` e o arquivo-fonte completo com submódulos.
- Custo: nenhum. O teste não vale em binários com símbolos removidos (no AppImage, o `build.sh` procura
  também uma mensagem do `ppc-dis.c` nos launchers, que o zig deixa sem `.symtab`).

### R26. SDL2 estático no AppImage; piso de glibc 2.29 (substituída em parte pela decisão "zig em tudo")
- O quê (decisão original): o AppImage compila o SDL2 fixado como biblioteca estática PIC e embute os headers
  (o jogo liga estático, sem `.so` do SDL em tempo de execução); os launchers usariam
  `-static-libstdc++`/`-static-libgcc` e a glibc mínima seria a da máquina de build (2.44), com um build em
  contêiner antigo como trabalho futuro.
- Substituída: durante o Task 17 o projeto inteiro (launchers, ferramentas, Ninja, SDL2 e o jogo) passou a
  ser compilado pelo zig (`-target x86_64-linux-gnu.2.31`, libc++ estática). A parte do SDL2 estático
  continua valendo; a do `-static-libstdc++` e do piso 2.44 não. O piso real, conferido com `readelf`, é
  glibc 2.29 (launchers e jogo; Ninja 2.17, ferramentas 2.16; o CMake oficial exige 2.17). Ver
  `docs/packaging.md`.
- Custo se errado: o AppImage não roda em distros com glibc anterior à 2.29.

### R27. O código-fonte da libfuse vai junto de cada release do AppImage
- O quê: o runtime type2 do AppImage liga a libfuse 3.15.0 (LGPL-2.1) estaticamente. Ela entra no
  `THIRD-PARTY-LICENSES`, e o `build.sh` baixa (fixado em `toolchain.lock`) o fonte da libfuse e do runtime
  type2, com o patch do runtime, e grava `LegacyRecomp-<versão>-runtime-sources.tar.gz` ao lado do AppImage.
- Custo se errado: nenhum (só um arquivo a mais na release).

### R28. O modo embutido é detectado por `<recursos>/toolchain`, não por `$APPDIR`
- O quê: o spec (§14) dizia que o `doctor` e a etapa `build` usam o toolchain embutido quando `$APPDIR` está
  definido. A implementação (`has_bundled_toolchain` em `src/pipeline/toolchain.h`) entra no modo embutido
  quando a pasta `toolchain/` existe dentro da pasta de recursos (`share/legacyrecomp`), e aí exige todos os
  arquivos do toolchain.
- Por quê: `$APPDIR` é herdado por qualquer processo filho e pode estar definido sem o toolchain (ou faltar
  com ele, se alguém extrair o AppImage com `--appimage-extract`); a pasta de recursos é o que o launcher
  realmente usa. Também permite testar o modo embutido sem AppImage.
- Custo se errado: uma instalação a partir do fonte com uma pasta `toolchain/` sobrando nos recursos usaria
  esse toolchain; o erro seria `ToolMissing` com a dica de AppImage corrompido.
