// SPDX-License-Identifier: MIT
#include "pipeline/game_exit.h"

#include "common/paths.h"

namespace lr::pipeline {

std::filesystem::path game_trace_file(const std::filesystem::path& root) {
  return DataPaths::from_root(root).logs / "game-trace.log";
}

std::string describe_game_exit(int code, const std::filesystem::path& trace_file, ExitTextLanguage language) {
  const bool en = language == ExitTextLanguage::English;
  const std::string n = std::to_string(code);
  const std::string trace =
      en ? " To see the kernel calls before it, run `legacyrecomp run -- --trace-imports` and read " +
               trace_file.string() + "."
         : " Para ver as chamadas ao kernel antes disso, rode `legacyrecomp run -- --trace-imports` e leia " +
               trace_file.string() + ".";
  switch (code) {
    case 0:
      return {};
    case 2:
      return en ? "game exited with code 2: usage or load error (bad arguments, image missing or not matching the "
                  "build, memory setup); the game's error message is above."
                : "O jogo terminou com código 2: erro de uso ou de carga (argumentos, imagem ausente ou diferente da "
                  "compilada, mapeamento de memória); a mensagem do jogo está na saída de erro (stderr).";
    case 3:
      return (en ? "game exited with code 3: it called a kernel import that is not implemented yet; the report above "
                   "names it (see docs/kernel-coverage.md)."
                 : "O jogo terminou com código 3: chamou uma importação do kernel ainda não implementada; o relatório "
                   "na saída de erro (stderr) diz qual (veja docs/kernel-coverage.md).") +
             trace;
    case 4:
      return (en ? "game exited with code 4: execution fault (SIGSEGV, SIGILL, ...) caught by the fault handler; the "
                   "report above has the faulting address."
                 : "O jogo terminou com código 4: falha de execução (SIGSEGV, SIGILL etc.) tratada pelo handler; o "
                   "relatório na saída de erro (stderr) tem o endereço.") +
             trace;
    case 5:
      return (en ? "game exited with code 5: fatal guest error (the game reached a state the kernel cannot continue "
                   "from); the report above says why."
                 : "O jogo terminou com código 5: erro fatal do convidado (o jogo chegou a um estado de onde o kernel "
                   "não consegue seguir); o relatório na saída de erro (stderr) diz o motivo.") +
             trace;
    case 6:
      return (en ? "game exited with code 6: deadlock (every guest thread stayed blocked); the report above lists "
                   "what each thread waits for."
                 : "O jogo terminou com código 6: deadlock (todas as threads do jogo ficaram bloqueadas); o relatório "
                   "na saída de erro (stderr) lista o que cada thread espera.") +
             trace;
    default:
      break;
  }
  if (code > 128 && code < 128 + 65) {
    const std::string sig = std::to_string(code - 128);
    return en ? "game was killed by signal " + sig + " (exit code " + n + ")."
              : "O jogo foi encerrado pelo sinal " + sig + " (código " + n + ").";
  }
  return en ? "game exited with code " + n + "." : "O jogo terminou com código " + n + ".";
}

}  // namespace lr::pipeline
