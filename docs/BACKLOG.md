# LOMAS plugin backlog

**Source of truth** for which LOMAS plugins we build, and in what order. Moved here from the project doc `lomas/BACKLOG.md` on 11 Nov 2026 (Lars); the project doc is now a pointer to this file.

- **This file:** the list, status, order and open decisions.
- **`docs/backlog/`:** the research page – a mockup in the Seam design system, a complexity assessment and a proposed approach per plugin. `backlog-data.js` holds the rows; `index.html` renders them and holds the mockup functions. Published as the artifact «LOMAS plugin backlog»: https://claude.ai/artifact/5R1WF9wrvEeGa8MD6YAcpa (slug `23beb741-…`).
- **Workflow:** change the repo first, then republish the artifact from `docs/backlog/` (the hub chat does both). Never edit the artifact by hand. A new plugin = a row here + a row in `backlog-data.js` (+ a MOCK function in `index.html` if it gets a mockup).
- Source of the list: the plugin analysis of the Ableton project `261105 Pose måker.als` (`seam/PLUGINS.md` in the project, 9 Nov 2026), plus plugins Lars added. Hosting details (AU status, WaveShell, in-process) stay in `seam/PLUGINS.md`.
- Window size: every standalone LOMAS plugin is `plugin-h` 400 pt tall (Seam `design/tokens.json`, Lars 11 Nov 2026).

**Priority below is a proposal (hub, 11 Nov), not yet decided by Lars.** Ordered by: (1) slot in Seam's default chain, (2) how often it is used, (3) how much it reuses from Plugin-kit. When Lars decides, mark the row "decided" and move it. The research page proposes moving Iron up to right after Vari-mu.

Names are Seam module names, never trademarks. The references are internal only. Names marked *(working name)* are open.

## Status

| # | LOMAS plugin | Reference (internal) | Uses in project | Role | Status | Next |
|---|---|---|---|---|---|---|
| 1 | **Passive EQ** | Waves PuigTec EQP1A (m/s) | 3 | Default chain | ✅ 0.1.0 built, listening test passed. **Parked** (M8 + analyzer; LATER: window 340 → 400) | Move onto Plugin-kit later |
| 2 | **Pre 73** | Arturia Pre 1973 | 2 | Default chain | 🔨 P7 built, P7b/P7c accepted | P7d (Low band, window 400) → P8 |
| 3 | **Vari-mu** | Waves PuigChild 670 (s) / 660 (m) | 14 | Default chain | Face exists in Seam design system (VariMu component); no repo | P0 when Pre 73 is at P8. Builds the dynamics module in Plugin-kit |

## Proposed next (after the default chain)

| # | LOMAS plugin | Reference (internal) | Uses | Where used | Why this order | Notes |
|---|---|---|---|---|---|---|
| 4 | **FET** *(working name)* | Arturia Comp FET-76 (1176 style) | 1 | Bass | Already named as the next Seam face in `seam/PLUGINS.md`; a compressor reuses Vari-mu's detector/gain-cell work | Fast attack, ratio buttons, colour from the line amp |
| 5 | **Saturator** *(working name)* | Soundtoys Decapitator | 3 | Buses | Most-used insert after the default chain; colour pipeline (P6) reusable | Several drive styles; decide which to model |
| 6 | **Iron** *(working name)* | Kazrog True Iron | 2 | Drum bus, Main | Transformer colour – overlaps with Pre 73's transformer model | Could be a cheap spin-off of Pre 73 P6 (research: ≈ 0.5×, proposed to move up) |
| 7 | **Bass harmonics** *(working name)* | Waves MaxxBass | 2 | Kick, Main | Psychoacoustic, not a circuit; different pipeline | Lower fit to the circuit + fit recipe |
| 8 | **Multiband dynamics** *(working name)* | Waves C6-SideChain | 1 | Main | Mix/master tool, outside the recording chain focus | Largest scope; research recommends leaving it to 3rd-party hosting |

## EFX – small plugins (added by Lars, 11 Nov 2026)

Effects outside the recording chain, each as its own small plugin. RC-20's modules are split into separate plugins. Order within the group is not set.

| LOMAS plugin | Reference (internal) | In project | Complexity / effort (research) | Notes |
|---|---|---|---|---|
| **Spring** *(working name)* | Arturia Rev SPRING-636 (Grampian 636 style) | Yes | 4/5 · ≈ 1.25× | Germanium preamp (P6 pipeline) + parametric dispersive spring per tank. Strongest EFX candidate. Builds the reverb framework Space reuses |
| **Ring mod** *(working name)* | NI Guitar Rig 6 · Ring Modulator | Yes (Guitar Rig 6) | 1/5 · ≈ 0.2× | Design from spec; LOMAS "Diode" mode as character. Good first EFX build |
| **Wobble** *(working name)* | XLN RC-20 v2 · Wobble | No (requested) | 2/5 · ≈ 0.3× | Modulated delay, wow + flutter, stereo chorus. Pair with Loss; engine reusable for Tape |
| **Distort** *(working name)* | XLN RC-20 v2 · Distort | No (requested) | 2/5 · ≈ 0.4× after Saturator | Focus band + types reused from Saturator/Iron |
| **Space** *(working name)* | XLN RC-20 v2 · Space | No (requested) | 3/5 · ≈ 0.75× | FDN reverb; build after Spring |
| **Loss** *(working name)* | XLN RC-20 v2 · Loss | No (requested) | 2/5 · ≈ 0.3× | Dropouts/repeats; Codec (STFT) mode later. Pair with Wobble |

**Before the first EFX plugin:** Seam needs a decision on where EFX live (insert in the chain, or send/return), and whether they appear in the chain's "Add to chain" list.

## Parked (off in the project)

| Plugin | Reference | Note |
|---|---|---|
| Tape *(working name)* | Concept: Walters Audio T805 (hardware); data: Waves Kramer Tape, Softube Tape | **Candidate to unpark** (11 Nov): T805-style scope without transport (Gain with compensation, Bias, HF, Nominal/Overbias) → 3/5, ≈ 1×. Fits after Saturator |
| Harmonics | Softube Harmonics | Off. Research: drop, covered by Saturator + Iron |
| Transient shaper | elysia nvelope | Both instances off; AU not found. Small filler once the dynamics kit exists |

## Not LOMAS plugins

- **Seam-native, built in Seam:** Input / Output (gain, balance, VU) and EQ (replaces Ableton Utility, VU Meter, EQ Eight). Reference for EQ: FabFilter Pro-Q 3.
- **Effects, mix and analysis (outside the recording chain, hosted as 3rd party):** Little Plate, Pro-R, Ableton Reverb, Guitar Rig 6 (apart from the Ring mod), SPAN, Tuner. Rev SPRING-636 is now an EFX candidate (Spring).

## Open decisions for Lars

1. Confirm the order 3 → 8, or reorder (research proposes Iron right after Vari-mu).
2. Should Iron (6) be built as a spin-off of Pre 73's transformer model, before or after FET (4)?
3. Is Multiband dynamics (8) a LOMAS plugin at all, or left to 3rd-party hosting?
4. Re-run the plugin analysis on 1–2 more Ableton projects before committing beyond #4? One project is a small sample (several plugins have 1–2 uses).
5. Where do EFX plugins go relative to the channel-strip plugins in the queue: interleaved (for example Ring mod as a quick win) or as a block after?
6. Where do EFX live in Seam: insert or send/return?
7. Unpark Tape (T805 scope)?
