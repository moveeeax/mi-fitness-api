# ADR 0007 — Rejected modularity alternatives: plugins, API DSL codegen, DI container

Status: Accepted — 2026-08-22

## Context

The 2026-08-22 modularity review (four independent passes; verdict in
`docs/superpowers/specs/2026-08-22-modularity-design.md`) measured the
real pain — compile-time blast radius and sync-copy coupling — and
evaluated three "big" remedies that keep resurfacing in audits. This
ADR records their rejection so the question is not relitigated every
review.

## Decision

1. **No plugins / dlopen / separately deployable feature modules.**
   The deploy unit is one binary plus a worker, and that is a feature:
   simple charts, one image, a mirrored init/shutdown lifecycle,
   sanitizers and LTO across the whole program. C++ plugin boundaries
   would add ABI fragility and destroy those properties to solve an
   independence problem that runtime module flags (`Core::*_enabled()`
   + per-handler 404) already solve.
2. **No DSL/codegen single source for routes.** At ~65 routes, the
   scaffolder writes the three surfaces and two seconds-fast gates
   guard the drift. A DSL taxes every forker before their first
   endpoint and puts generated files in every diff. Revisit only if
   route count grows several-fold or schema-body drift (which the gates
   do not check) becomes a recurring incident class. The cheaper cut —
   registering Drogon routes FROM `Endpoints.hpp` (4 copies → 2) —
   remains open as Phase 3 work, and is not a DSL.
3. **No DI container.** The working norm is a singleton with an
   `install_for_testing` seam (ADR 0004 update). A container adds
   ceremony without adding testability the seams don't already give.

## Consequences

Modularity investment goes where the measurements point instead:
hub-splitting (Modules.hpp, per-module registration), a declared
include DAG with a gate, `app_core` STATIC de-inlining, and
single-source generation for config/release surfaces.

## Update — 2026-08-23 (Phase 3 execution)

The "cheaper cut" left open above — registering Drogon routes FROM the
`Endpoints.hpp` registry — was evaluated and is now also rejected. The
registry is deliberately metadata-only (`{method, path, description}`
strings); wiring handlers through it would require the registry to see
every controller, recreating the controller include hub that the Phase 1
hub split (#43) dismantled, or abandoning `HttpController` for lambda
registration across all 13 controllers — a fork-facing idiom change that
buys the removal of one scaffolder-written, twice-gated copy. Revisit
triggers unchanged (route count several-fold, recurring sync incidents).
The other Phase 3 items shipped instead: `release.sh` + helm-pin gating
(#50), CI self-scoping (#47), and the config-sync gate (#51).
