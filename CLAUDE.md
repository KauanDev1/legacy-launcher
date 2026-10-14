@AGENTS.md

## Claude-specific

- Use the superpowers skills (brainstorming, writing-plans, test-driven-development, verification-before-completion).
- Specs live in `docs/superpowers/specs`, plans in `docs/superpowers/plans`.
- Never read or dump large binary game data (ISO, XEX, extracted assets) into context; inspect it with scripts in `tools/` and read only their summarized output.
