> **SLOPWARE.** Written largely by AI agents (Claude Code + Codex) from reverse-engineered
> Qualcomm firmware, tested on exactly one laptop. It pokes raw SoC registers. It works here,
> it may hard-reset yours. No warranty, not upstream quality, read before you run.

# X1E ADSP/CDSP at EL2 without qebspil and without TZ PAS

Linux running at EL2 (slbounce, no Gunyah/QHEE) on X1E80100 starts and stops the ADSP and
CDSP with the stock `qcom_q6v5_adsp` remoteproc driver. Neither qebspil nor TZ
authentication is needed.

Tested on: ASUS Vivobook S15 S5507QA, `linux-msm/laptops-kernel` branch `qcom-laptops` at
`65f4ae7320b8` (7.3.0-rc7), ASUS 2026 firmware set (CDSP c1-00079), 2026-10-16. The 5
patches apply cleanly on that commit, in order.

Verified with this driver: both DSPs come up, the battery (battmgr on the ADSP) and the
speakers work, and llama.cpp runs on the NPU (CDSP, FastRPC). An s2idle cycle
(`pm_test=platform`) resumes with both DSPs running; a real overnight sleep is not tested yet.

## How it works

Like `qcom_q6v5_adsp` already does for SDM845/SC7280, Linux copies the image into the
carveout and releases the core through the QDSP6SS boot FSM (`RST_EVB`, `BOOT_CORE_START`,
`BOOT_CMD`). There is no PAS `auth_and_reset` and no signature check. X1E needs a few extra
things that the hypervisor (QHEE) would otherwise do:

- **Second image.** Each DSP also loads its DTB image into its own carveout. For the ADSP,
  the DTB descriptor at qdsp6ss+0x60..0x74 is rewritten, because a reset clears it.
- **EVB.** The boot address is the ELF entry point, and the other register bits are kept.
- **Clock/PLL bring-up and stop sequences.** These were recovered from QHEE (.367) and are
  replayed as raw MMIO, along with the CDSP IOMMU apertures QHEE grants.
- **The ADSP image UEFI leaves running.** It is stopped over SMP2P and PAS shutdown. This is
  the only TZ call, and it also releases that image's carveout protection.

## Patches (`patches/`)

| # | What |
|---|---|
| 0001 | tzmem: `qcom,shm-bridge-vmid` self-owner for EL2 (from the jglathe tree) |
| 0002 | x1-el2 DT: 40-bit DMA ranges, Iris firmware SID, SHM bridge owner (from the jglathe tree) |
| 0003 | pmic_glink: defer the rpmsg probe until the pmic_glink device exists (battery after DSP start) |
| 0004 | **remoteproc: qcom_q6v5_adsp: add X1E80100 ADSP/CDSP for Linux at EL2** |
| 0005 | x1-el2 DT: ADSP/CDSP on `qcom,x1e80100-{adsp,cdsp}-pil` (ADSP streams in bypass, CDSP SID 0x0c00) |

0001/0002 are general EL2 prerequisites (Iris and SCM need them too), not DSP-specific.

`src/qcom_q6v5_adsp.c` is the resulting driver, for reading. It grows from 850 to about 1950
lines; nearly all of the addition is the X1E register sequences and the aperture table.

### What is actually needed (bisected)

Each group of QHEE steps was switched off one at a time. A group is "needed" if the DSP then
fails to start. Groups that were not needed were also dropped together and re-tested with a
cold start plus a stop/start of both DSPs, so they don't just rely on clocks UEFI left on.

| DSP | Needed | Dropped (not needed) |
|---|---|---|
| CDSP | the 103 IOMMU apertures QHEE grants, SMEM + cmd-db in its IOMMU domain, PLL | branch-clock setup, calibration, QHEE private channel page |
| ADSP | 10 registers at 0x0726a000 + 0x07250018/0x0726a300, PLL, DTB descriptor | GCC/LPASS branch clocks (0x00147160, 0x06e21008/34, 0x00147010, 0x06848020), optional boot-config block, 0x0725b000 clear |

The stop sequences were not bisected.

### CDSP apertures

The apertures were also bisected one by one. The CDSP needs 19 of the 103 to boot: QUPv3
wrapper clock pages, IPC_CORE, NSP CX CPR, PRNG, the TCSR mutex and registers, RPMh PDC/BCM/CPRF,
AOP message RAM, QFPROM, the CDSP BWMON and an LLCC broadcast register. With only those 19, the
CDSP boots and restarts, but it crashes as soon as an HTP (NPU) session opens: it touches the
LLCC registers, and from then on even its restarts fault there. So the driver keeps the whole
table. Each entry is named in the source, and [`cdsp-apertures.csv`](cdsp-apertures.csv) lists
the name and bisection result for every one. The names come from the hardware region directory
inside the CDSP firmware, not from guesses.

### What the registers are

The ADSP bring-up writes are standard Qualcomm clock-controller registers:

| Write | Register |
|---|---|
| `0x0726a000 + 4*n`, n = 0..9 | TCM block branch clocks (CBCRs) |
| `0x0726a300` | `AXIS_HS_CBCR` |
| `0x07250018` | `RSC_WAIT_EVENT_OVRD_MASK` |
| `0x06840000..0x06840038` | QDSP6SS Lucid OLE PLL (the CDSP has the same PLL type at `0x32340000`) |
| `0x06848000/04/20` | Q6 core RCG (command/config) and core branch clock |

The descriptor at qdsp6ss+0x60..0x74 holds the DTB entry address, chip id, chip version,
platform info and image size. EVB writes keep bits 31:28 and 3:0.

The stop paths follow QHEE in two places that an earlier version got wrong: the NSP NoC halt is
requested only when the NoC is up and not idle yet, and a failed QUP/GPI cleanup SMC
(`0xc2000405`, SiP service 4 command 5) no longer aborts the ADSP stop halfway.

Kernel config: `CONFIG_QCOM_Q6V5_ADSP=m` (it now selects `QCOM_PAS`). Boot the `-el2.dtb`
with slbounce as usual, and **remove qebspil from the ESP**.

Firmware is loaded from `/lib/firmware` via the DT `firmware-name` entries
(`qcom/x1e80100/<vendor>/<device>/qcadsp8380.mbn`, `adsp_dtbs.elf`, `qccdsp8380.mbn`,
`cdsp_dtbs.elf`, plus the `*.jsn` files for pd-mapper).

### Board-specific values

- **ADSP DTB descriptor values** are module parameters with this laptop's defaults
  (`adsp_chip_ids=0x0088022b`, `adsp_chip_version=0x201`, `adsp_platform_info=0x28000200`).
  They were read on this Vivobook before reset. On another X1E device, read your own first.
- **SoC id check.** The ADSP bring-up refuses to run unless the SoC id register reads
  exactly `0xa0090201`.

## Not done / honest notes

- **Partly minimal.** The bring-up was bisected at the level of step groups (see above). Single
  registers inside the needed groups and the stop sequences were not.
- **Raw clock-controller writes.** The sequences poke GCC/LPASS/NSP registers behind the
  clock framework's back. For upstream, the PLLs and branch clocks belong in a QDSP6SS clock
  provider (`clk_alpha_pll`, `clk_rcg2`, `clk_branch`). The generic Lucid OLE configure helper
  writes the calibration field differently from QHEE, so it can't be reused as is.
- **Raw SMC.** The ADSP stop issues a raw SMC (`0xc2000405`, QUP/GPI channel cleanup) outside
  `qcom_scm`.
- **Ignored timeouts.** Some timeouts are ignored on purpose, because QHEE ignores them too.
- **Missing QHEE step.** The CDSP private channel clear that QHEE does is not reproduced.
- **No DT binding.** There is no binding update for the new compatibles yet.

## Restart (`optional-restart/`)

`echo stop/start > /sys/class/remoteproc/remoteprocN/state` works for the **CDSP**.

The ADSP itself also stops and starts fine when the audio stack is not loaded (tested with
`module_blacklist`). With audio loaded, ADSP restart is **not finished**. The audio stack has to be detached first (that is what
`restart-adsp73.py` does). These patches (a separate 3-patch series, applied on top of
`patches/`) fix what broke along the way:

- `0001` (Oleg Keri): SoundWire waits for the IRQ thread before gating the clock;
- `0002`: LPASS macros, SoundWire and battmgr survive the ADSP going away;
- `0003`: the reset-gpio device is re-created after its GPIO controller rebinds (WSA884x speakers).

Still open: `wcd938x` creates an IRQ domain that it never removes. After `rmmod`, the next
IRQ lookup oopses and leaves the irqdomain mutex held, so the ADSP restart hangs in GLINK
start. The ADSP itself stops and starts fine.

## License

GPL-2.0, same as the Linux kernel files these patches touch.

## History

The first version was a separate 2900-line `qcom_x1_el2_pas` driver that still went through
TZ PAS. TZ turned out not to be needed, and the stock driver covers most of it, so that
driver is gone. Thanks to travmurav and valpackett for the hint (Valve's loader does the
same three QDSP6SS writes).
