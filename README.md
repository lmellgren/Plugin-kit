# LOMAS Plugin-kit

Shared code for LOMAS plugins: the realtime DSP engine, the black-box measurement rig, the circuit and
fitting tools, the JUCE wrapper base and the Seam GUI components. Extracted (copied) from LOMAS Passive EQ
at M7b (`PassiveEQ@ec407da`); Passive EQ itself is unchanged and migrates to the kit in its own milestone.

Plugins use the kit as a git submodule at `kit/`. Plugin-specific code (netlist, parameters, colour curves,
editor layout) stays in the plugin repo. Improve the kit instead of forking it, and keep `./check.sh` green.

## Contents

| Area | Files | What |
|---|---|---|
| DSP engine | `core/include/lkit/Foh.h`, `LinAlg.h` | Exact-pole state-space filter with physical states: first-order-hold discretisation (one augmented `expm`), short least-squares output FIR, stereo `d2` tick. Generic in the number of states. |
| Modal engine | `core/include/lkit/Modal.h`, `LinAlg.h` | The same FOH network in modal form: 2 x 2 blocks (invariant subspaces of eigenvalue pairs, close eigenvalues kept together), one-input recursion, output map + 4 input taps fitted to the analog response times an integer delay, coefficient changes converted through the physical basis. `LinAlg.h` adds real eigenvalues (balance + Hessenberg + QR) and null spaces. |
| Resampling | `Halfband.h/.cpp` | Linear-phase Kaiser half-band stages for 2x up/down (sample and block APIs, short compile-time kernels), cascaded `Oversampler`. |
| Plumbing | `Simd.h`, `Biquad.h`, `Smooth.h`, `TrueBypass.h` | 2-lane vectors, bilinear biquad, smoothing that snaps, peak meter, true bypass (bit-exact, latency-compensated, 10 ms crossfade). |
| Bench | `Bench.h` | Warm-up, best-of-N, per-stage CSV rows in "% of one core per stereo instance" (QA R5). |
| Golden files | `Golden.h` | Compare renders with committed `.lkg` files; `LKIT_UPDATE_GOLDEN=1` rewrites (on purpose only). |
| Offline render | `RenderJob.h` | Driver for a plugin's core render tool: reads the text job from `lkit.jobs`, same protocol as the AU host. |
| Wrapper base | `juce/…/ProcessorBase.h` | `juce::AudioProcessor` shell: true bypass on the On parameter, static Auto Gain (message thread), Output trim, meters, mono/stereo, chunking, versioned state + migration. |
| Seam UI | `juce/…/Seam.h`, `juce/gen_tokens.py`, `cmake/Lkit.cmake` | Encoder (unipolar/bipolar/stepped), Segmented, Toggle, Trim, Section, Meter, Header, ValueTip. `lkit_seam_tokens(<target>)` generates `Tokens.h` from `~/Apps/Seam/design/tokens.json` at build time; no design file is copied. |
| AU host | `measure/lkit_measure.mm` | `list`, `params` (parameter map, presets, channel configs, latency), `probe`, `render` (settle, reset, pre-roll, read-back checks). Effects `aufx` and music effects `aumf`. |
| VST3/AU render | `tools/vst3render/` | Headless render of our own VST3/AU builds from the same host job (VST3 == AU checks). |
| Measurement | `python/lkit/sweep.py`, `jobs.py`, `host.py` | Synchronised exponential sweep (H1 + harmonics), jobs (settings x levels -> `.npz` + MANIFEST), parameter maps. |
| Circuit | `python/lkit/mna.py`, `spice.py` | Netlist (R, C, L, K, E): AC response, state space with physical states (for `Foh.h`), sympy H(s), ngspice deck + runner. |
| Fit | `python/lkit/fit.py` | Staged, bounded least squares in physical parameters; refuses holdout settings. |
| Reports | `python/lkit/plots.py` | Plot style and family-of-curves helpers. |
| Seam reference | `tools/seam_reference.py` | Screenshot of a seamgen plugin window (read-only import of the design tools). |
| Safety net | `check.sh`, `templates/check.sh` | Kit: build + all tests. Template for plugins: build, ctest, auval, pluginval. |

## Build and test

```sh
./check.sh                       # builds core + JUCE parts, runs C++ (core, JUCE) and Python tests
```

Needs JUCE 8.0.6 (`JUCE_DIR`, default `~/Documents/ChatGPT/Plugins/.build-deps/JUCE-8.0.6`), Python 3.13 for
the venv (`.venv`, created by `check.sh`), and ngspice for the spice cross-check test (skipped if missing).

## Using it from a plugin

```cmake
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
set(LKIT_BUILD_JUCE ON CACHE BOOL "" FORCE)
add_subdirectory(kit)
juce_add_plugin(MyPlugin ...)
target_link_libraries(MyPlugin PRIVATE lkit::juce lkit::seam ...)
lkit_seam_tokens(MyPlugin)
```

Python: `sys.path.insert(0, "kit/python")`, then `from lkit import host, jobs, mna, spice, fit`.

## Not in the kit (yet)

- Passive EQ's network, colour curve, response curve/analyzer component and editor layout: plugin-specific.
- The oversampled colour section of Passive EQ (`PassiveEq.cpp`): its structure (2x network, xN colour,
  short half-bands, silence skip) is the template, but the stage list depends on the plugin's model.
  It moves into the kit as a generic section when Pre 73 P6 needs it.
