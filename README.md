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
| Modal engine | `core/include/lkit/Modal.h`, `LinAlg.h` | The same FOH network in modal form: 2 x 2 blocks (invariant subspaces of eigenvalue pairs, close eigenvalues kept together), one-input recursion, output map + input taps fitted to the analog response times an integer delay (regularised towards the circuit's own output map), blocks rotated so one row takes the input, coefficient changes converted through the physical basis, a coarse fit for designs while a knob moves. `LinAlg.h` adds real eigenvalues (balance + Hessenberg + QR) and null spaces. |
| Resampling | `Halfband.h/.cpp` | Linear-phase Kaiser half-band stages for 2x up/down (sample and block APIs, compile-time kernels for the tap counts in use: 4 / 5 / 6 / 8 / 16 / 19 / 20 / 31 / 32 folded taps; other lengths run the generic kernel, about 25 % slower), cascaded `Oversampler`. |
| Dynamics | `Dynamics.h` | A tube limiter's control-loop parts (Vari-mu P5): `CubicTable<N, K>` (K smooth functions on one uniform grid, Catmull-Rom; for a gain cell's law built from the plugin's own model), `Detector` (full-wave rectifier, dead zone, softplus knee, source resistance, soft current limit: the charge current at vc; `voltage()` and `currentFrom()` separately for a caller that reads the rectifier between samples, Vari-mu P6), `RcNetwork` (main R ‖ C plus up to three series-RC branches, switched per Time constant, explicit Euler in the offline model's order). The gain cell and the loop's topology stay in the plugin. |
| Plumbing | `Simd.h`, `Biquad.h`, `Smooth.h`, `TrueBypass.h` | 2-lane vectors, bilinear biquad, smoothing that snaps, peak meter, true bypass (bit-exact, latency-compensated, 10 ms crossfade). |
| Loudness | `Loudness.h` | K-weighted (BS.1770) pink-noise loudness of a response, for a static Auto Gain that compensates an EQ curve (from Passive EQ). |
| Bench | `Bench.h` | Warm-up, best-of-N, per-stage CSV rows in "% of one core per stereo instance" (QA R5). |
| Golden files | `Golden.h` | Compare renders with committed `.lkg` files; `LKIT_UPDATE_GOLDEN=1` rewrites (on purpose only). |
| Offline render | `RenderJob.h` | Driver for a plugin's core render tool: reads the text job from `lkit.jobs`, same protocol as the AU host. |
| Wrapper base | `juce/…/ProcessorBase.h` | `juce::AudioProcessor` shell: true bypass on the On parameter, static Auto Gain (an expensive part on the message thread, a cheap part per block on the audio thread so it follows automation, also with Auto Gain off), Output trim, meters, mono/stereo, chunking, versioned state + migration. |
| Seam UI | `juce/…/Seam.h`, `juce/gen_tokens.py`, `cmake/Lkit.cmake` | Encoder (44 pt: unipolar/bipolar/stepped; options: knob height (90, or 84 in the Vari-mu rows) and a labelled stepped scale; arrow keys), GrMeter (needle, 20 → 0 dB, value text; Vari-mu P7), Section (the heading can change), the 126 pt encoder cells of the chain modules (ScaleKnob: stepped with dense scales, bipolar, unipolar; DualKnob: frequency ring + gain knob, two hit zones and focus parts; EQ glyphs; label/dot click, keyboard, accessibility), Segmented, Toggle, Trim, Section, Meter, Header (centred or plugin-window style), ValueTip. `lkit_seam_tokens(<target>)` generates `Tokens.h` from `~/Apps/Seam/design/tokens.json` at build time; no design file is copied. |
| AU host | `measure/lkit_measure.mm` | `list`, `params` (parameter map, presets, channel configs, latency), `probe`, `render` (settle, reset, pre-roll, read-back checks; `stimulus_channels: 2` for an interleaved stereo stimulus; `watch`: output parameters such as meters read once per block; a mid-render `change` renders in real time with the main run loop running and re-sends until it reads back, for plugins that apply parameters on a main-thread timer). Effects `aufx` and music effects `aumf`. |
| VST3/AU render | `tools/vst3render/` | Headless render of our own VST3/AU builds from the same host job (VST3 == AU checks). An AU is found through the system registry: install the build first. |
| Measurement | `python/lkit/sweep.py`, `jobs.py`, `host.py` | Synchronised exponential sweep (H1 + harmonics), jobs (settings x levels -> `.npz` + MANIFEST), parameter maps. |
| Circuit | `python/lkit/mna.py`, `spice.py` | Netlist (R, C, L, K, E): AC response, state space with physical states (for `Foh.h`), sympy H(s), ngspice deck + runner. |
| Fit | `python/lkit/fit.py` | Staged, bounded least squares in physical parameters; refuses holdout settings. |
| Reports | `python/lkit/plots.py` | Plot style and family-of-curves helpers. |
| Analysis | `python/lkit/analysis.py` | Peak frequency, shelf midpoint, −3 dB corner, coherent harmonics, THD+N, harmonic tracks (Blackman-Harris), worst alias of a sine render (from Pre 73). |
| Dynamics | `python/lkit/dynamics.py` | Compressor/limiter measurement: sine stimuli whose level changes only at whole cycles (staircase for the static curve in one render, piecewise bursts), steady-state stats per segment (level, gain, H2…Hk, THD+N), instantaneous gain trace (analytic envelopes; `hp_hz` high-passes both signals so a thump does not bend the envelope), attack time, release fit (single and two-slope, for program-dependent release), GR read-back from the output (Vari-mu P2). |
| Model values | `python/lkit/params_header.py` | `write_params_header(values, out, namespace, source)`: the circuit model's component values (nominal + fitted) → a generated C++ header of `constexpr` values; component values only, never response tables (from Pre 73). |
| Host frame | `juce/…/HostFrameFollower.h`, `juce/src/HostFrameFollower.mm` (`lkit::hostframe`) | AUv2: lets an editor follow the frame the host gives the plugin view (chain layout: the host sets the height, the editor picks its layout). From Pre 73 P7e, in the kit since Vari-mu P7; its own target, so a plugin that still carries its own copy does not get two. Guard it with an `auview` host check in the plugin's `check.sh`. |
| Snapshot | `juce/include/lkit/juce/Snapshot.h`, `templates/snapshot/` | Plugin window → PNG without a host (`setPlain`, `feedTone`, `writeEditorSnapshot`), for the face check against the Seam preview; the template is the console app to copy (from Pre 73). Needs `JUCE_MODAL_LOOPS_PERMITTED=1`. |
| Seam reference | `tools/seam_reference.py` | Screenshot of a seamgen plugin window (read-only import of the design tools). |
| Safety net | `check.sh`, `templates/check.sh` | Kit: build + all tests. Template for plugins: build, install the AU, ctest (host tests need the installed AU), auval, pluginval. |

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

Python: `sys.path.insert(0, "kit/python")`, then `from lkit import host, jobs, mna, spice, fit, analysis, dynamics, params_header`.

## Not in the kit (yet)

- Passive EQ's network, colour curve, response curve/analyzer component and editor layout: plugin-specific.
- The oversampled colour section of Passive EQ (`PassiveEq.cpp`): its structure (2x network, xN colour,
  short half-bands, silence skip) is the template, but the stage list depends on the plugin's model.
  It moves into the kit as a generic section when Pre 73 P6 needs it.
