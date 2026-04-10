# Fullsword — Project Memory

> Auto-synced | 276 observations

**Stack:** Unknown stack

## 🏛️ CORE ARCHITECTURE

> **CRITICAL:** The following rules represent strict architectural boundaries defined by the user. NEVER violate them in your generated code or explanations.

# Intellectual Property & Architecture Rules
Write your strict architectural boundaries here. 
BrainSync will automatically enforce these rules across all agents (Cursor, Windsurf, Cline) 
and inject them into the memory context.

Example:
- NEVER use TailwindCSS. Only use vanilla CSS.
- NEVER write class components. Only use functional React components.

## 🛡️ GLOBAL SAFETY RULES

- **NEVER** run `git clean -fd` or `git reset --hard` without checking `git log` and verifying commits exist.
- **NEVER** delete untracked files or folders blindly. Always backup or stash before bulk edits.

## 🧭 ACTIVE CONTEXT

> Always read `.cursor/active-context.md` for exact instructions on the specific file you are currently editing. It updates dynamically.

## 🔴 STOP — READ THESE FIRST

- **Agent: follow existing project patterns — don't introduce a different style** — Agent: follow existing project patterns — don't introduce a different style
- **Agent: don't generate code with "any" type — define proper TypeScript types** — Agent: don't generate code with "any" type — define proper TypeScript types
- **Agent: always handle loading/error states — don't just render data** — Agent: always handle loading/error states — don't just render data
- **Agent: don't use deprecated APIs — check library version, use current API** — Agent: don't use deprecated APIs — check library version, use current API
- **Agent: check existing code before creating utility functions — avoid duplicates** — Agent: check existing code before creating utility functions — avoid duplicates

## 📐 Conventions

- Version your API from day 1 (/api/v1/)
- Use consistent response format across all endpoints
- Implement soft delete for important data — don't hard delete without confirmation
- Handle timezone correctly — store UTC, display in user's timezone
- Make layouts responsive from the start — mobile-first approach
- Disable submit button during form submission — prevent double-submit
- Always add empty states ("No items yet" with call-to-action)
- Always add error states with retry button — not just blank screen

## ⚡ Available Tools (ON-DEMAND only)
- `save(title, content, category)` — Save a note + auto-detect conflicts
- `batch_save(items[])` — Save multiple notes in 1 call
- `query(text)` — Search memory for architecture, past fixes, decisions
- `search(text)` — Full-text search for details
- `check_errors()` — Check compiler errors after edits

> ℹ️ DO NOT call get_context() or get_gotchas() at startup — context above IS your context.

---
*Auto-synced | 2026-04-10*
