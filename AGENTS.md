# V1Simple Engineering Contract

This standalone public repository is the product source of truth and must remain fully usable on its own. An optional private companion may provide read-only engineering context through `.codex/hooks/v1simple_context.py`; use that context to improve decisions, but never make builds, tests, or product behavior depend on the companion.

For design, bug fixes, functionality, refactors, quality work, tests, reviews, or releases, use the `v1simple-engineering` skill and consult the injected context before planning. If the bridge reports degraded context, continue from public evidence and keep the limitation visible.

- Preserve the privacy boundary: do not copy private documents, excerpts, internal-only paths, or private operational data into public source, tests, commits, logs, or release artifacts.
- Test in the layer that owns the behavior. Every bug fix needs a regression test at the lowest practical layer that reproduces the failure; add boundary or integration coverage when the defect crosses layers.
- Treat existing contracts, semantic guards, hardware constraints, and release evidence as design inputs. Do not weaken a guard merely to make a change pass.
- Keep changes scoped and inspect the final diff for accidental private context or unrelated edits.
- The public remote is main-only (`main` is the only remote branch). Never push a public development branch; development branches remain local and are preserved by the private workflow.
