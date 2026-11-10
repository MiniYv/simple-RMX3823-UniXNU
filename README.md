# simple-RMX3823-UniXNU

> **Provenance**: forked from [Janit8r/uefi-rmx3823](https://github.com/Janit8r/uefi-rmx3823),
> which derives from the Project Silicium ecosystem and Project Mu (the original
> upstream README is preserved at the bottom of this file). The default branch
> is `main`; the full boot chain was verified on hardware by miniyv on
> 2026-11-10. All upstream work belongs to its original authors; this fork
> only contains board-specific adaptation and defect fixes.

UEFI port for the realme GT5 (RMX3823 / SM8550), with the goal of booting XNU
through UniXNU. See [README.zh-CN.md](README.zh-CN.md) for the Chinese version.

## Current state (verified on hardware, 2026-11-10)

Under `fastboot boot` (temporary boot — **no partitions are written**), the
complete boot chain runs:

```
fastboot → ABL (gzip) → BootShim → SEC → FV decompression (LZMA)
→ DXE (all drivers dispatched) → BDS → UniXNU menu (Apple logo, volume-key
navigation) → timeout fallback → UnableToBoot diagnostics → intentional halt
```

Not done yet: UniXNU payload wiring, UFS storage driver, boot-option
registration (see TODO).

## Key changes relative to upstream

1. **Corruption-window avoidance (the critical fix).** ABL overwrites about
   1.2 KB at a fixed offset (firmware offsets 0x349A6–0x47661) inside its own
   gunzip output buffer. In the upstream layout that region lands in the
   middle of the LZMA-compressed firmware volume, so FV decompression fails
   on every prebuilt image (the failure was masked by the stock
   screen-blanking behavior). Fix: the FDF inserts a 0x41000-byte RAW pad
   section (`rmx3823Pkg/Pad.bin`) so the compressed volume starts at
   `FD+0x50034`, past the corrupted window.
2. **RamManagerDxe removed.** It requires the Qualcomm RamPartition protocol
   that ABL provides in the stock chain; nothing installs it under
   `fastboot boot`, so the driver asserted. The memory map is already supplied
   by MemoryMapLib, so the driver is dispensable.
3. **BDS no longer powers off.** `PlatformBootManagerUnableToBoot` no longer
   blanks the screen, stalls 10 s and calls `ResetSystem`; it prints GOP
   geometry and boot-option counts, then halts via `CpuDeadLoop()`. The
   shutdown path had been masking the real failure point.
4. **SEC on-device diagnostics** (`Sec.c`): layered FVDBG / LZTEST / DTBSYNC
   / CHKSUM output of the in-memory firmware state; decompression failure
   halts instead of returning.
5. **Console parameters**: 30 ms per line, 2x font, wrap when full (keeps the
   last 115 lines).

## Build notes

```bash
# Environment: isolated venv + clean PATH (the default PATH contains an rm
# wrapper that blocks make after the 50th deletion; PYTHONPATH must be
# emptied or stuart deletes get intercepted)
export PATH=/usr/lib/llvm-22/bin:<venv>/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PYTHONPATH=
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
export RMX3823_EXTRA_PACKAGES="<UniXNU>/package:<UniXNU>/drivers/DarwinBoot"
python build_uefi.py -d rmx3823 -r DEBUG   # RELEASE currently fails to link (Sec serial constructor)

# Boot image packaging (the only parameter set ABL accepts):
#   header v0 / kernel_addr 0xA8000000 / tags 0x01E00000 / pagesize 4096
#   payload = gzip(BootShim.bin + SILICIUM_UEFI.fd) + 2041002-byte DTB blob
#   (DTB blob cut from a known-good reference image, magic d00dfeed)
```

## TODO (next phase)

- [ ] UniXNUDxe reports `image not found`: wire up the payload file GUID
- [ ] UFS storage driver (note: `USE_CUSTOM_DISPLAY_DRIVER=1` and
      `RMX3823_VENDOR_HW=0` are mutually exclusive in the DSC; enabling UFS
      requires reworking the display chain)
- [ ] Boot-option registration: the menu UI is ready, but the boot-entry list
      is still empty (Boot options: 0)
- [ ] The inner FV is 99% full; confirm the growth strategy before adding drivers
- [ ] Only one corruption window has been verified; watch for further
      corrupted regions if the compressed stream grows (CHKSUM can detect)

---

The original upstream README is preserved below:

---

# uefi-sheng

UEFI platform integration for the Xiaomi Pad 6S Pro (sheng, SM8550), using
Project Silicium and Project Mu.

The platform supplies memory reservations, board peripherals, display
handoff, and the firmware volume. UniXNU supplies the boot application and
DarwinBoot loader. UniXNU-sm8550-common supplies shared XNU platform support.

- [Firmware integration](https://unixnu.com/porting/uefi-firmware)
- [sheng device support](https://unixnu.com/devices/sheng)
- [Build instructions](Building.md)
- [Platform details](Platforms/Xiaomi/shengPkg/PORTING.md)
- [Platform status](Status.md)

## Payload firmware

Build through `./unixnu build firmware --uefi ../uefi-sheng` from UniXNU.
The DSC and FDF include `UniXNURamDiskDxe`, DiskIoDxe and FAT support so the
boot-image payload appears as a filesystem. `UniXNURamDiskDxe` is a shared
module in UniXNU's `package/`, and uses the Mu-Silicium FDT handoff.
`ShengBootMemoryDxe` supplies the board's memory expansion policy.
See [required firmware integration](https://unixnu.com/porting/uefi-firmware#required-integration).

## Credits

[Microsoft](https://github.com/microsoft/) for their [Project Mu](https://microsoft.github.io/mu/) which Powers this Project. <br>
[WOA-Project](https://github.com/WOA-Project/) for their [mu_andromeda_platforms](https://github.com/WOA-Project/mu_andromeda_platforms/) which is used as Base for this Project. <br>
[Project Aloha](https://github.com/Project-Aloha/) for their [mu_aloha_platforms](https://github.com/Project-Aloha/mu_aloha_platforms/) which is used as Reference for some SoCs. <br>
[Open Surface RT](https://openrt.gitbook.io/open-surfacert/) for their Help on Tegra Related Stuff.

[sonic011gamer](https://github.com/sonic011gamer/) and [halal-beef](https://github.com/halal-beef/) for their Amazing Work on Exynos Related Stuff.

[Janit8r](https://github.com/Janit8r/) for the rmx3823 port this fork is based on.

## License

All code except drivers in `GPLDrivers` & `GPLApplications` & `GPLLibrary` directories are licensed under BSD-2-Clause. <br />
GPL Drivers are licensed under `GPLv2` or later license.

## Device porting record

The firmware section of [sheng's porting record](https://unixnu.com/devices/sheng#firmware)
records handoff inputs, development findings, evidence and completion criteria.
Update `UniXNU-sheng/porting.toml` when firmware work changes those inputs or
results. Maintainers and agents use the [same workflow](https://unixnu.com/porting/records).

## Local build inputs

Save device paths and boot arguments in `unixnu.local.toml` in the UniXNU
checkout. See the [configuration reference](https://unixnu.com/reference/configuration#local-build-inputs).
Then use `./unixnu kernel`, `./unixnu firmware`, `./unixnu payload`, and
`./unixnu bootimg`. Argument changes require rebuilding the payload and boot
image. See [boot arguments](https://unixnu.com/xnu/boot-arguments#apply-arguments)
and [green-bar diagnosis](https://unixnu.com/guide/debugging#green-bar).
