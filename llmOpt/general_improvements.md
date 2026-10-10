# gengin general-improvement queue (llmOpt/)

Curated minimal chores for the llmOpt sessions — bug fixes and
testing/review-overhead reductions that do not belong to a hot-path
optimization PR.  Read the GENERAL IMPROVEMENTS fallback in
`prompts/optimize.md` before touching this file; the soft-lock rules there
apply.

This file is tracked in git and shared by every checkout — keep it small and
current.  Caps per item: <= 2 files, <= 120 changed lines, no
struct/data-layout changes, no visual change, `make_bench` not worse.

## Queue

Item format:
`- [ ] <id>: <file>:<function> — <what and why>; verify: <command>`

- (empty — the maintainer adds items here)

## Proposed (sessions: append suggestions only — never implement)

Sessions may append one-line candidates below; only the maintainer promotes
accepted ones into the queue above.

- (empty)
