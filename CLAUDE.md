@AGENTS.md

## Claude-specific

- Use the superpowers skills (brainstorming, writing-plans, test-driven-development, verification-before-completion).
- Specs live in `docs/superpowers/specs`, plans in `docs/superpowers/plans`, SDD ledgers (copied in at milestone end) in `docs/superpowers/ledgers`.
- Never read or dump large binary game data (ISO, XEX, extracted assets) into context; inspect it with scripts in `tools/` and read only their summarized output.
- To resume work, start at `docs/handoff.md` (current state, what is left, the owner's rules and past rulings).

### Owner's working preferences

- Milestones run as subagent-driven development with a ledger; pass `effort: "high"` on every Agent dispatch (implementers, reviewers, planners).
- No question rounds: write specs and plans straight from the docs and mark your own calls; ask only what only the owner can answer (legal, the ISO, GitHub account actions).
- Talk to the owner in Portuguese, in short messages.
- Commit only as `Kauan Turcato Storto <159664484+KauanDev1@users.noreply.github.com>`; never as the KauanCentral account. Ask before every push; one consolidated branch.
- Never install system packages, WSL distros, SDKs or large downloads without asking first, even if a spec says it was pre-approved; mention the disk cost.
- Keep the docs current as work proceeds: findings and fixes update the milestone status doc and `docs/handoff.md` in the same commit.
