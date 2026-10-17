> **SLOPWARE.** Written largely by AI agents (Claude Code + Codex) from reverse-engineered
> Qualcomm firmware, tested on exactly one laptop. It works here, it may hard-reset yours.
> No warranty, read before you run.

# X1E ADSP/CDSP at EL2: clock-provider variant

The same series as the top-level folder of this repo, plus five patches (13–17) that
move the Q6 core PLL and core RCG out of the remoteproc driver into a clock provider in
`drivers/clk/qcom`. Which of the two goes upstream is for the maintainers to decide; this
folder exists so both are ready.

Patches 1–12 are identical to `../patches`. Read that README for how the
DSP start works, the bisection results and the register names.

## Patches 13–17

| # | What | Lines |
|---|---|---|
| 0013 | dt-bindings: clock: `qcom,x1e80100-{adsp,cdsp}-q6sscc` + ID header | +61 |
| 0014 | dt-bindings: remoteproc: `core` clock for both DSPs | +6 −2 |
| 0015 | clk: qcom: `q6sscc-x1e80100.c`, the Q6 core clock controller | +284 |
| 0016 | remoteproc: qcom_q6v5_adsp: take the Q6 core clock from q6sscc | +29 −70 |
| 0017 | DT: the two clock controllers (disabled in hamoa, enabled in x1-el2) | +32 |

The whole series: 17 patches, about +1864 −12 lines.

Kernel config: `CONFIG_CLK_X1E80100_Q6SSCC=y` (or `=m`), plus `CONFIG_RESET_QCOM_PDC`
and `CONFIG_QCOM_Q6V5_ADSP` as for the main folder.

## Why the provider is unusual

The PLL and RCG sit inside the QDSP6 subsystem:

- they lose their state on every DSP reset;
- on the CDSP they can only be touched after the boot FSM powered the core up.

So `q6sscc` never reads the hardware on its own:

- `.prepare` reprograms the PLL every time (QHEE's sequence, which `clk_lucid_ole_pll_configure()`
  does not match);
- `.recalc_rate` reports the programmed rate, `.is_enabled` the software state;
- `.disable` only does bookkeeping: the subsystem reset turns the PLL off;
- both clocks are `CLK_IGNORE_UNUSED`, so the clock core's unused-clock pass leaves them alone.

The remoteproc driver calls `clk_prepare_enable(core)` at the point where it used to program
the PLL: before the DTB descriptor on the ADSP, after the boot FSM on the CDSP.

The parent is `bi_tcxo_ao_div2`: the PLL configuration assumes a 19.2 MHz reference
(L = 0x32 for 960 MHz, which is what the clock framework now reports), and the active-only
vote does not keep XO on while the CPUs sleep.

## Tested

Same laptop, same base, 2026-10-17:

- both DSPs from cold boot;
- two CDSP restarts and two ADSP restarts (audio stack not loaded);
- llama.cpp on the NPU (8.2 tok/s);
- battery and sound.

The enable counts in `clk_summary` stay balanced across restarts.

`dt_binding_check`, `dtbs_check` (new nodes) and `checkpatch --strict` are clean, apart from
the MAINTAINERS reminder for the new files.

The patches carry `Assisted-by:` and no `Signed-off-by:` (see the main README); whoever submits
adds their own.

`src/` has the resulting `qcom_q6v5_adsp.c` and `q6sscc-x1e80100.c`.
