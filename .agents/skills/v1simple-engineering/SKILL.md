---
name: v1simple-engineering
description: Apply V1Simple's repository contracts and optional private engineering context to design work, bug fixes, refactors, quality or functionality improvements, tests, hardware or protocol changes, code reviews, and releases. Use for any task that plans, changes, validates, or evaluates V1Simple product behavior.
---

# V1Simple Engineering

## Workflow

1. Read the root `AGENTS.md`. Before planning, consult the engineering context injected by `.codex/hooks/v1simple_context.py`. If it is absent, execute the configured bridge command from `.codex/hooks.json` once with the current lifecycle event input. Treat a degraded-context message as an explicit limitation and continue from public evidence.
2. Inspect the relevant public code, tests, contracts, semantic guards, and current diff. Use private context as design input, never as a substitute for public evidence.
3. Plan the smallest coherent change that preserves architecture, hardware constraints, public interfaces, and established invariants. State any unresolved risk before editing.
4. Implement without weakening safety or quality checks. Keep the public repository fully buildable and testable without the private companion.
5. Test in the owning layer. For a bug fix, add a regression test at the lowest practical layer that fails for the defect; add boundary or integration coverage when behavior crosses layers. Run the relevant existing guard plus proportionate broader checks.
6. Review the final diff for behavioral regressions, unrelated changes, and private leakage. Never copy private documents, excerpts, internal-only paths, or operational data into public artifacts.
7. For releases, require the repository's release evidence and readiness gates. Never push a public development branch; the public remote remains `main`-only.

Report the context status, design constraints applied, tests run, and any remaining risk in the handoff.
