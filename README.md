# GA100 RIR Writer

[简体中文](README.zh-CN.md)

Standalone tool for reading/writing RIR (Redundancy Information Rows) on GA100 (Ampere) GPUs.
Read paths and SW Override are verified working on CMP 170HX. **RIR writes land and
persist across cold boot, but have NOT been proven to change any functional state.**

> **⚠ This project was built almost entirely through AI-assisted
> "vibe coding" — including this README. The code has been validated
> only by the measurements summarized at the bottom of this file.**
>
> **Review the source yourself, especially the write paths
> (`cmd_write_rir`), before running anything against hardware: RIR
> burns are irreversible and a bug here can cost you a slot or a
> card.**

## Build

```bash
make
# or
gcc -O2 -o ga100_rir ga100_rir.c
```

## Prerequisites

1. Linux system with the GPU installed
2. Root access (sudo)

Unbinding the NVIDIA driver is **not required for reads** (measured:
`--status` / `--dump-rir` / `--dump-fuses` all work with the driver
bound). Only unbind if a command reports the DEBUGCTRL RWL bit will
not latch, i.e. the driver is holding fuse-block access:

```bash
lspci -D | grep NVIDIA
# 0000:41:00.0 3D controller: NVIDIA Corporation GA100 ...

echo 0000:41:00.0 > /sys/bus/pci/devices/0000:41:00.0/driver/unbind
```

## Quick Start

```bash
# 1. Check current state (read-only, safe)
sudo ./ga100_rir 0000:41:00.0

# 2. Volatile unlocks (safe, lost on reboot) — these DO work:
sudo ./ga100_rir 0000:41:00.0 --sw-enable                  # needs gate already open
sudo ./ga100_rir 0000:41:00.0 --sw-write gen3 0            # Unlock PCIe Gen3/Gen4
sudo ./ga100_rir 0000:41:00.0 --sw-write nvlink 0          # Unlock NVLink
sudo ./ga100_rir 0000:41:00.0 --sw-write ecc 1             # Enable ECC

# 3. Rebind driver and test
echo 0000:41:00.0 > /sys/bus/pci/drivers/nvidia/bind
nvidia-smi
lspci -vvv -s 41:00.0 | grep -i speed
```

`--sw-enable` only works on cards whose `disable_sw_override` gate is
already open (`--status` shows `fuse : 0`). On gate-locked cards the
historical RIR route below was tried and **did not open the gate**;
it is documented, not recommended.

## All Commands

```bash
# === Read-only (safe) ===
sudo ./ga100_rir <bdf>                              # Full status + RIR state
sudo ./ga100_rir <bdf> --status                     # Feature / floorsweep info
sudo ./ga100_rir <bdf> --dump-rir                   # RIR records
sudo ./ga100_rir <bdf> --dump-fuses 128             # Raw fuse rows

# === RIR encoding (no hardware access) ===
sudo ./ga100_rir <bdf> --encode-rir 5               # Encode for bit 5 (disable_sw_override PRI)
sudo ./ga100_rir <bdf> --encode-rir 37              # Encode for bit 37 (disable_sw_override RED)

# === RIR write (IRREVERSIBLE, effect on gate UNPROVEN) ===
sudo ./ga100_rir 0000:41:00.0 --write-rir 0 0x15011401     # disable_sw_override PRI+RED records (but this did not unlock my 170hx 10g fuse at all😭)
# No external hardware is needed on 170HX (measured). The burn
# lands and survives cold boot, but never opened the gate.

# === SW Override (volatile, lost on reboot) ===
sudo ./ga100_rir <bdf> --sw-enable                  # Activate SW override
sudo ./ga100_rir <bdf> --sw-write gen3 0            # Unlock PCIe Gen3
sudo ./ga100_rir <bdf> --sw-write nvlink 0          # Unlock NVLink (0=all enabled)
sudo ./ga100_rir <bdf> --sw-write ecc 1             # Enable ECC
sudo ./ga100_rir <bdf> --sw-write gpc <mask>        # GPC disable mask
sudo ./ga100_rir <bdf> --sw-write fbp <mask>        # FBP disable mask
sudo ./ga100_rir <bdf> --sw-write fbpa <mask>       # FBPA disable mask
sudo ./ga100_rir <bdf> --sw-write fbio <mask>       # FBIO disable mask
sudo ./ga100_rir <bdf> --sw-write l2 <mask>         # ROP/L2 disable mask
sudo ./ga100_rir <bdf> --sw-write pcie <mask>       # PCIe lane disable
sudo ./ga100_rir <bdf> --sw-disable                 # Deactivate SW override
```

## Key Fuse Bits (Row 0)

```
Bit | Fuse Name               | Abs bit (PRI/RED)
----|-------------------------|-------------------
  0 | enable_fuse_program     | 0 / 32
  1 | disable_fuse_program    | 1 / 33
  5 | disable_sw_override     | 5 / 37  ← unlock target
  6 | debug_feature_sw_ovr_en | 6 / 38
```

## Safety

- **RIR writes are permanent** — only 8 rows or 16 slots, no undo
- **RIR write effect is UNPROVEN** — records persist across cold boot but the gate stayed locked on every card tested; do not burn slots expecting an unlock
- **SW Override is volatile** — safe for testing, lost on reboot
- Automatic backups + two-step confirmation on every RIR write
- Always `--dump-rir` before writing to check for empty slots

## Measurement status (summary)

- The fuse programming path works: writes land, read back correctly,
  survive a power-off cold boot.
- `DISABLE_SW_OVERRIDE_STATUS` remained 1 after cold boot on every
  burned card — bit-identical to an unwritten card.
- Two explanations remain open: (a) the record layout/encoding does
  not match what the hardware applies, or (b) GA100 does not apply
  RIR corrections to security-latch fuses at all. Neither has been
  refuted yet.
- RIR size measured: 8 rows × 32-bit (16 slots), not the 4 rows
  hardcoded in NVIDIA's Maxwell-era MODS tooling.
