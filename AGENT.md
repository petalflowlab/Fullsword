# Project Notes — Fullsword
> 275 notes | Updated: 4/10/2026

## Safety Rules

- **NEVER** run `git clean -fd` or `git reset --hard` without checking `git log` and verifying commits exist.
- **NEVER** delete untracked files or folders blindly. Always backup or stash before bulk edits.

## Quick Reference
- 42 warnings → see `.agent-mem/gotchas.md`
- 80 conventions → see `.agent-mem/patterns.md`
- Codebase map → see `.agent-mem/project-brief.md`
- Active work → see `.agent-mem/active-context.md`

## Read .agent-mem/gotchas.md before ANY changes

For full memory: `.agent-mem/`
For observation details: `.agent-mem/observations/`

## Available Tools (Use ON-DEMAND only — context in .agent-mem replaces startup calls)
- `smart_save(title, content, category)` — Save + auto-detect conflicts
- `batch_save(items[])` — Save multiple in 1 call
- `query(q)` — Search memory when debugging
- `find(query)` — Full-text search for details
- `errors()` — Check compiler errors after edits
- `read_lines(path, start, end)` — Read file sections
- `grep(pattern, dir)` — Find symbols without loading full files

> Do NOT call load() or warnings() at startup — read the .agent-mem files above instead.

---
*Auto-generated*
