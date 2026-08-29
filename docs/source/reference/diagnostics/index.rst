Diagnostic bridge
=================

OpenMWSE includes a diagnostic bridge that runs the built ``openmw`` executable through its normal bootstrap, save loading,
simulation, rendering, and resource paths. It captures reproducible evidence for graphics, gameplay, and performance problems
without writing to the source game profile. The bridge is the authoritative full-scene diagnostic path; focused unit and shader
tests remain the authority for their smaller ownership boundaries.

Execution modes
---------------

``openmw-diagnosticctl`` uses the same bounded command protocol in both modes:

* An enabled interactive session accepts local inspection, graphics capture, gameplay and performance traces, capsule capture,
  and shader reload commands. Interactive sessions reject deterministic stepping, world-state mutation, and remote shutdown.
* ``run`` launches ``openmw`` as a windowless child with a production offscreen OpenGL context, copied configuration, temporary
  user data, device-free input state, disabled sound, and a fixed simulation step. ``replay`` adds the validated save and world
  state from a capsule. The controller rejects a visible or foreground child window, waits for readiness and capture barriers,
  records evidence, shuts down and reaps the child, proves temporary-workspace cleanup, and verifies that the source profile did
  not change.

Set ``OPENMW_DIAGNOSTICS=1`` before starting an interactive development build to enable its default current-user endpoint. Use
an explicit endpoint with ``--diagnostic-session`` and an artifact directory with ``--diagnostic-artifacts`` when coordinating
more than one development session. Disabled builds perform no diagnostic transport or statistics collection.

Capture and replay
------------------

Run a windowless production capture from an existing save::

  openmw-diagnosticctl run --openmw <build>\openmw.exe --config <profile>\openmw.cfg \
      --save <profile>\userdata\saves\<character>\<save>.omwsave --artifacts <capture-directory> --frames 20

Replay that capture after rebuilding::

  openmw-diagnosticctl replay --openmw <build>\openmw.exe --config <profile>\openmw.cfg \
      --capsule <capture-directory>\capture.omwdiag --artifacts <replay-directory> --frames 20

Compare the real production attachments from both runs::

  python scripts\compare_diagnostic_graphics.py <capture-directory>\graphics-<frame> \
      <replay-directory>\graphics-<frame> --output <replay-directory>\graphics-parity.json

The comparison validates matching stages and dimensions, rejects non-finite values, records mean, 99.9th-percentile, and maximum
absolute error, and exits unsuccessfully when an attachment exceeds its checked-in replay-parity limit.

Attach commands
---------------

The controller accepts ``status``, ``graphics``, ``gameplay``, ``performance``, ``capsule``, and ``reload-shaders`` against an
enabled session. Supply ``--endpoint <name>`` when the session does not use the default endpoint. ``graphics`` schedules the same
production pass capture used by Shift+F12. ``capsule --output <name>.omwdiag`` writes beneath the session artifact directory.
``step`` and ``shutdown`` are child-only operations.

Providers and artifacts
-----------------------

The versioned providers expose these evidence domains:

* ``graphics@1`` captures scene color and depth, resolved atmospheric volumes and metadata, shafts, and the final composite over
  normal and diagnostic-isolation stages.
* ``gameplay@1`` records the cell, camera, weather, game and simulation clocks, and state. The replay controller checks these
  stable fields before advancing the simulation.
* ``performance@1`` records frame pacing, CPU scopes for scripts, Lua, mechanics, physics, world, and UI, resource caches and
  work queues, navigation and paging activity, scene statistics, and per-camera GPU/render timing when the driver supplies it.

A ``.omwdiag`` capsule contains a manifest, effective settings, full loadable world snapshot, stable world state, provider
versions, frame identity, and performance evidence. The manifest records the ordered content list and hashes. It does not embed
installed game or mod assets. Graphics bundles remain beside the capsule because they are evidence from the run rather than
inputs required to reproduce it. The controller also preserves the isolated engine and crash logs beneath the artifact
directory's ``logs`` folder before removing its temporary workspace.

Compatibility and failure behavior
----------------------------------

Capsules and commands carry explicit format versions. Replay fails before controlled execution when the capsule is corrupt,
unsupported, oversized, contains unsafe names, omits required state, or does not match the locally installed ordered content and
hashes. Artifact writes are bounded, validated, atomic, and refuse replacement. The controller reports structured protocol error
names and preserves engine error context.

Protocol, capsule, provider, and render-bundle versions are independent compatibility boundaries. A provider version describes
the names and semantics in one provider response. Graphics-provider bundle version 1 stores named, dimensioned float attachments;
adding a newly named optional attachment remains compatible, while changing an existing attachment's meaning, layout, numeric
representation, or coordinate convention requires a new bundle or provider version. Readers reject unsupported versions and
must not infer compatibility from the OpenMWSE release number.

The fixed-step scheduler controls simulation and rendering clocks after loading the real save. Readiness and graphics-completion
barriers prevent capture races. A disconnected controller cancels pending work, and controller destruction terminates and reaps
its child. Engine commands run at the main-thread safe point after Lua garbage collection is quiescent and before world updates.
Render capture state is restored after every diagnostic stage.

Security and privacy
--------------------

The bridge has no network listener, shell, script-evaluation, arbitrary file, native-call, or memory-access command. Windows uses
a named pipe restricted to the current user and rejects remote clients; Unix uses a current-user Unix-domain socket. Protocol
frames, queues, connections, capsule entries, attachment sizes, step counts, paths, and timeouts are bounded.

Windowless runs copy only the configuration and save state needed to bootstrap OpenMW. Installed content remains in place and is
read through the normal virtual filesystem. The engine reports its offscreen graphics and device-free input owners, while the
controller independently checks that the child owns no visible or foreground desktop window. Temporary configuration, user data,
materialized saves, and storage are removed when the run ends; engine logs are retained as diagnostic artifacts. A recursive
fingerprint proves that configuration, saves, mods, and other writable profile state are unchanged; screenshots are excluded
because the controller never writes to their directory.

Treat artifact directories as private diagnostic data. Capsules contain the captured save state, content names and hashes, while
logs and traces can contain local paths, cell names, and runtime state. Review an artifact set before sharing it.

Maintenance
-----------

Add a provider only when its owning subsystem can expose a bounded, versioned snapshot at an established safe point. Keep policy
and serialization in ``components/diagnostics`` and domain collection in the owning engine subsystem. Extend protocol and capsule
tests for malformed, boundary, cancellation, and compatibility behavior. Validate full-scene changes with capture, rebuild,
replay, and the numerical comparison; do not reproduce production rendering or simulation in a synthetic parallel harness.
Use focused graphics-context tests for individual shaders, equations, permutations, asset-loading boundaries, and edge cases.
Use the bridge for full-scene images, cross-pass evidence, production-asset interactions, and performance measurements.
