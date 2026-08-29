# AGENTS.md

## Purpose

This file defines durable engineering rules for OpenMWSE. It governs how changes are investigated, designed,
implemented, reviewed, verified, and released. It is not a roadmap, feature specification, or substitute for task-specific
acceptance criteria.

Explicit user instructions take precedence. More specific `AGENTS.md` files may add rules for a subtree, but they must not
weaken the repository-wide safety, verification, or source-authority requirements in this file.

## Project identity

OpenMWSE is an opinionated, long-lived fork of OpenMW. It may deliberately diverge from upstream product decisions while
preserving maintainability, interoperability, and practical upstream synchronization.

- Upstream acceptance is not a design requirement.
- Upstream compatibility and ease of synchronization are engineering requirements.
- Preserve upstream behavior outside the explicitly requested change boundary.
- Make deliberate fork behavior native, coherent, tested, and maintainable. Do not bolt it on through unnecessary parallel
  systems merely to minimize the number of modified upstream lines.
- Qualify every required release platform. Keep changes portable within OpenMW's existing abstractions unless a task explicitly
  requires platform-specific behavior.

## Working agreement

- Inspect relevant code, tests, configuration, history, and local instructions before proposing structural changes.
- A request to explain, diagnose, review, or report does not authorize implementation. Modify the repository only when the
  user asks for a change or the requested outcome clearly includes implementation.
- Make evidence-backed assumptions that keep work within scope. Ask before making a product decision, changing a public
  contract, or taking an action with material external consequences.
- Do not commit, push, publish, tag, or create a release unless explicitly asked.
- Preserve unrelated user changes in a dirty worktree. Never discard or rewrite them to simplify the current task.

## Repository authority and generated output

This repository is authoritative for all OpenMWSE source and released behavior.

- Derived repositories, archives, installers, generated resources, generated documentation, and other distribution artifacts
  must be reproducibly produced from canonical source in this repository.
- Never correct generated output manually. Fix its source or generator and regenerate it.
- Do not maintain multiple authored copies of the same implementation or data. Establish one owner and generate or consume all
  other representations from it.
- Generated artifacts must identify the source revision and any compatibility boundary needed to use them safely.
- Release automation must be deterministic, fail on missing or unexpected inputs, and verify the produced manifest or hashes.
- Validate release artifacts through a clean-install or clean-extraction workflow rather than only inspecting the build tree.

## Upstream and Git discipline

- Treat the official OpenMW repository as the upstream source and keep it distinct from the OpenMWSE fork remote.
- Never commit fork work to a branch intended to mirror upstream.
- Do not rebase published integration history. Integrate upstream through explicit, reviewable synchronization commits.
- Short-lived, unpublished feature branches may be rebased before integration when doing so improves clarity.
- Keep commits buildable and bisectable. Separate upstream synchronization from fork changes and unrelated cleanup.
- Resolve upstream conflicts according to the current upstream architecture, then reapply the intended OpenMWSE behavior at the
  correct ownership boundary.
- After an upstream synchronization, reassess whether fork code is still necessary. Remove obsolete implementations and
  workarounds instead of preserving them indefinitely.
- Avoid drive-by formatting, renaming, relocation, or cleanup in upstream-owned files. Minimize the coherent change surface,
  not the raw line count.

## Architecture and ownership

- Give every concern one authoritative owner.
- Put behavior in the subsystem that owns the relevant policy and lifecycle. Do not duplicate policy across engine code,
  scripts, shaders, configuration, packaging, or user interfaces.
- Keep dependencies explicit and directed toward stable abstractions.
- Prefer established OpenMW extension points and subsystem boundaries when they own the concern. Add fork-owned components only
  when they establish a clearer responsibility than extending an existing owner.
- Favor strong separation of concerns without speculative abstractions or intent-obscuring indirection. Extract a new owner when
  extending a type would materially mix responsibilities.
- Complete structural changes. Do not leave two internal paths, temporary shims, legacy aliases, or partially migrated callers.
- Preserve compatibility at genuine public boundaries, including file formats, saves, configuration, scripting APIs, mod-facing
  behavior, command-line interfaces, and distributed resources. Internal compatibility layers require explicit justification.
- Keep ownership and lifetime explicit. Avoid hidden global state, implicit cross-subsystem mutation, and order-dependent setup.
- Prefer immutable data and pure transformations where practical. Encode units, coordinate spaces, color spaces, time bases, and
  valid ranges in types and names where possible.

## Source readability and documentation comments

- Write self-documenting code with expressive, concise names for modules, types, classes, functions, methods, parameters, and
  variables.
- Names communicate mechanics and domain meaning. Documentation comments communicate rationale, constraints, intent, contracts,
  and other context that is not evident from the declaration and implementation alone.
- Documentation comments are mandatory for all new and changed modules, types, classes, functions, and methods. Update existing
  documentation comments whenever the associated contract, constraints, or behavior change.
- Use concise one-sentence documentation comments for simple declarations and imperative phrasing for callable behavior.
- Use Doxygen-compatible structured comments for complex C++ APIs. Use the established repository-native documentation-comment
  conventions for Lua, shaders, build logic, and other source languages.
- Documentation comments must be accurate and meaningful. Do not restate signatures, control flow, or other obvious mechanics.
- Inline comments are allowed only for non-obvious behavior, invariants, edge cases, mathematical assumptions, compatibility
  requirements, ownership, lifecycle, or external constraints.
- Keep documentation changes within the coherent change boundary. Do not add comments mechanically to otherwise untouched
  upstream declarations.

## Behavior and compatibility

Before changing behavior, establish:

- the current upstream behavior;
- the intended OpenMWSE behavior;
- the affected public, persisted, mod-facing, rendering, scripting, configuration, and release boundaries;
- failure and fallback behavior; and
- how the result will be verified.

Also:

- Characterize behavior before restructuring behavior-sensitive code.
- Treat original-engine quirks and existing OpenMW behavior as compatibility constraints unless the requested change explicitly
  supersedes them.
- Make intentional divergence visible in tests and appropriate user-facing documentation.
- Version persistent formats and public interfaces deliberately, with migrations when compatibility requires them.
- State what compatibility implementations support and fail clearly for unsupported operations. Do not use silent no-ops to
  create the appearance of support.
- Defaults are product behavior. Change them only as part of the requested outcome and test both fresh and existing
  configurations when relevant.

## C++ standards

- Follow the repository's C++ standard, `.clang-format`, `.clang-tidy`, naming, and include-order conventions.
- Use the formatter and analyzer versions pinned by repository automation. Do not substitute a different local version when its
  output or diagnostics may differ.
- Format changed C++ files or lines only. Do not reformat unrelated upstream code.
- Use RAII and explicit ownership. Prefer values, references, and standard smart pointers over manual lifetime management.
- Prefer expressive domain types and enums over unrelated primitive values or boolean parameter collections.
- Keep headers focused and dependencies minimal. Avoid unchecked narrowing, undefined behavior, lifetime hazards, and reliance
  on unspecified evaluation or initialization order.
- Preserve useful error context. Do not swallow exceptions, ignore error states, or silently replace invalid values.
- Use the project's logging facilities for runtime diagnostics. Messages should identify the failed operation and relevant
  resource or subsystem without exposing sensitive data.
- New warnings or static-analysis findings introduced by changed code are failures. Do not suppress diagnostics globally to make
  a local change pass.

## Rendering and shader standards

- Give shared rendering equations and policy one implementation. Reuse common shader includes or an appropriately owned CPU-side
  model rather than copying logic between passes.
- Make coordinate space, distance units, depth convention, color space, transfer function, exposure assumptions, and valid ranges
  explicit.
- Centralize shared tuning data. Do not scatter duplicate coefficients or independently tuned copies across engine code, scripts,
  and shaders.
- Keep CPU-side state, uniforms, shader declarations, and fallback paths synchronized through a tested interface.
- Consider every supported rendering path and meaningful compile-time permutation affected by a change.
- Shader preprocessing success is not compilation success. Compile representative generated shaders in a real graphics context.
- Treat compilation failures, link failures, invalid uniforms, NaNs, discontinuities, undefined sampling, and accidental fallback
  paths as defects.
- Use deterministic scenes and representative checks of affected passes, materials, lighting, transparency, reflections,
  shadows, and environment conditions. Evaluate visual as well as numerical correctness.
- Measure CPU cost, GPU cost, memory effects, compilation stalls, and cache behavior when a rendering change can materially affect
  performance.

## Lua and scripting standards

- Target the Lua language and runtime behavior supported by the current OpenMW revision.
- Follow existing script lifecycle, sandbox, serialization, event, and API-version conventions.
- Prefer events and explicit state transitions over per-frame polling.
- Keep script state ownership and save behavior explicit. Avoid hidden module state that depends on load order.
- Validate external data and public API arguments at the boundary. Produce actionable errors that identify the script, API, and
  rejected operation.
- Do not claim compatibility through silent stubs or ignored operations.
- Respect engine resource limits and determinism. Avoid unbounded work and accidental reliance on host filesystem, process,
  network, locale, or clock state.
- Keep generated declarations and documentation synchronized with public API changes.
- Format and lint fork-authored scripts with repository-pinned tools when available. Do not reformat unrelated upstream scripts.

## Build, configuration, and dependencies

- CMake is the authoritative build description. Keep targets, installation, tests, and generated resources expressed through the
  existing build architecture.
- Do not hardcode developer-machine paths, drive letters, SDK locations, or build-directory names in committed source or build
  logic.
- Avoid duplicating build configuration in ad hoc scripts. Repository helper scripts should orchestrate authoritative CMake
  targets rather than reimplementing them.
- Reuse configured SDKs, runtimes, and toolchains. Do not install, vendor, or select a duplicate toolchain unless explicitly
  requested or required by a pinned build configuration.
- Ask before adding or upgrading a production dependency.
- Prefer upstream-compatible dependencies and existing project facilities before introducing a new library.
- Pin repository tooling used for reproducible checks. Keep exact versions in automation or dependency manifests rather than this
  instruction file.
- Keep downloaded tools and dependency caches outside the source tree unless they are intentionally vendored.
- Do not edit vendored code without a clear ownership and maintenance decision.

## Licensing and provenance

- Verify licensing and redistribution rights before copying or adapting third-party code, shaders, scripts, equations, tables,
  data, documentation, or assets.
- Record the source, version or revision, license, and nature of local modifications for incorporated third-party material.
- Preserve required copyright notices, license texts, attribution, and corresponding-source obligations in source and released
  artifacts.
- When provenance or permission is uncertain, treat observed behavior as a reference and produce an independent implementation.
  Do not copy first and resolve licensing later.
- Keep provenance records close to authoritative source. Do not rely on commit history or personal knowledge as the only record.

## Security and robustness

- Treat content files, mods, saves, scripts, configuration, archives, paths, and network-derived data as untrusted inputs.
- Validate inputs at subsystem boundaries and fail with useful context.
- Preserve sandbox and capability boundaries. New access to the filesystem, processes, native code, or network requires an
  explicit product and security decision.
- Bound parsing, allocation, recursion, queues, and asynchronous work for content-controlled input. Avoid unsafe path
  construction, traversal, unchecked archive extraction, and destructive operations on unresolved paths.
- Do not commit secrets, credentials, private keys, tokens, machine-local configuration, personal paths, or private user data.

## Testing standards

- Add or update tests with every behavior change and bug fix.
- Test observable behavior at the subsystem that owns it. Avoid tests coupled only to private helpers, incidental call order, or
  implementation trivia.
- Prefer the lightest real component that proves the behavior. Fake external boundaries, not the behavior under test.
- Cover success, failure, boundary, regression, and compatibility cases as applicable.
- Tests must be deterministic, isolated, order-independent, and parallel-safe.
- Control clocks, randomness, environment, locale, filesystem, process state, graphics settings, and external services when they
  affect the result.
- Do not use arbitrary sleeps, retries, test ordering, or global serialization to hide races or flakes.
- A flaky test is a defect. Reproduce and fix it; do not normalize it.
- Performance-sensitive changes require a representative baseline and an after measurement. Do not infer performance from code
  shape alone.
- Rendering changes require both automated validation and controlled visual verification.
- Build and test the configurations affected by compile-time options or platform-specific paths.
- Run focused checks continuously, broader affected suites before handoff, and full required gates before release. Never weaken
  meaningful coverage merely to make a change pass.

## Diagnostic bridge

- Use `openmw-diagnosticctl` as the authoritative full-scene path for graphical, gameplay, and performance diagnosis. Retain
  focused tests for equations, shaders, permutations, subsystem contracts, and edge cases.
- Reproduce development issues with an isolated `run` capture or validated `.omwdiag` capsule and `replay`. Preserve the capsule,
  provider evidence, engine logs, performance trace, and graphics-parity report needed to support conclusions.
- Never automate, mutate, or autonomously launch the user's normal game or profile. Hidden runs use temporary writable state and
  treat configuration, saves, mods, content, and screenshots as read-only. Live attachment permits inspection and explicitly safe
  development commands only.
- Extend the versioned protocol or owning diagnostics provider when new evidence is required. Do not add environment-gated
  screenshot generators, synthetic full-scene emulation, manual cross-harness capture formats, or parallel performance harnesses.
- Treat capsules, logs, and traces as private diagnostic data because they can contain save state, content names and hashes, local
  paths, cell names, and runtime state. See `docs/source/reference/diagnostics/index.rst` for commands and artifact contracts.

## Quality gates and verification

- Use repository-owned quality commands and CI configuration as the authority for exact tool versions, options, and required
  checks.
- At minimum, run the applicable formatter check, static analysis, focused tests, affected test suites, and build targets.
- Verify generated files are current and reproducible when their sources or generators change.
- Exercise runtime changes in a representative environment. For release changes, verify a clean artifact and its installation or
  extraction behavior.
- Distinguish observed results from inference. Report commands or checks that actually ran, their outcomes, and anything deferred.
- A failing required gate blocks completion unless the failure is proven unrelated and reported explicitly.
- Do not claim success based solely on compilation when the requested behavior can be exercised directly.

## Documentation and observability

- Document public behavior, settings, interfaces, compatibility guarantees, installation requirements, and migration steps when
  they change.
- Keep engineering policy out of feature documentation and feature specifications out of this file.
- Describe the product as it exists now. Do not document removed alternatives, imagined implementations, or historical process
  unless that history is necessary to explain a compatibility constraint.
- Avoid creating broad documentation unrelated to the requested change.
- Keep examples, paths, commands, defaults, and version claims synchronized with the implementation.
- Keep diagnostics actionable and located at the owning boundary. Preserve the original cause when translating errors.

## Code review rules

Review changes for:

- unintended behavior outside the requested boundary;
- duplicated ownership, incomplete migrations, or unnecessary upstream conflict surface;
- public, persisted, content-facing, scripting, rendering, build, and release compatibility;
- lifetime, threading, determinism, numerical stability, error handling, security, licensing, and provenance;
- missing, inaccurate, mechanical, or format-inappropriate documentation comments and unclear source names;
- missing boundary, integration, visual, performance, generation, or release verification; and
- claims not supported by executed verification.

Keep findings concrete and actionable: identify the behavior, failure mode, evidence, and safest correction. Do not report
preferences already enforced mechanically unless the toolchain cannot express the issue.

## Definition of done

A change is complete when:

- the requested behavior is implemented at the correct owner;
- the change has no known partial migration, duplicate path, or unnecessary compatibility shim;
- relevant tests were added or updated and pass;
- applicable formatting, analysis, build, runtime, visual, performance, generation, and release checks pass;
- required documentation comments are present, accurate, meaningful, and written in the appropriate source-language format;
- affected public behavior is documented and generated artifacts are reproducible from canonical source;
- unrelated user work remains intact;
- the diff contains no accidental formatting, debug code, temporary files, machine-local paths, or secrets; and
- the handoff states what changed, what was verified, and any remaining risk or deferred coverage.

## Commit policy

- Commit only when explicitly requested.
- Each commit must have one clear objective and leave the repository buildable.
- Use an outcome-oriented subject in the form `type(scope): result` when practical.
- Use `feat`, `fix`, and `perf` for product-facing outcomes; use `refactor`, `test`, `docs`, `build`, `ci`, and `chore` for internal
  changes.
- Avoid WIP, checkpoint, miscellaneous, formatting-only, and mechanical-diary commits in published history.
- Do not mix an upstream synchronization with independent feature work.
- Do not amend, reorder, squash, or rewrite commits belonging to the user unless explicitly asked.

## Exceptions

The user may explicitly override these rules for a task. State the exception, keep it as narrow as possible, and preserve all
unaffected safety and verification requirements. Do not create permanent waiver machinery for a one-off decision.
