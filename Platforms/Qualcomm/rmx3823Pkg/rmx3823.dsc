##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# realme GT5 240W (RMX3823) - SM8550 / kalama
#
# Derived from Platforms/Qualcomm/qrd8550Pkg (the SM8550 reference design),
# not from the Xiaomi nuwaPkg/shengPkg packages, because RMX3823 is a
# non-Xiaomi board and its ABL descends from the same Qualcomm boot_images
# baseline as QRD8550.
#
# Reused verbatim from QRD8550 on purpose:
#   - Binaries/qrd8550/* prebuilt Qualcomm DXE blobs (no rmx3823 set exists)
#   - Silicium-ACPI/Platforms/Qualcomm/qrd8550/AcpiTables.inf (no Realme
#     SM8550 ACPI package exists; the tables are SoC-generic)
#   - The DDR / register memory map in Library/MemoryMapLib/MemoryMapLib.c
#     (see the note in that file about what still needs device verification)
#
################################################################################

[Defines]
  PLATFORM_NAME                  = rmx3823
  PLATFORM_GUID                  = 7C4E1B92-3A5D-4F68-9E07-B2D1A8C53F14
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/rmx3823Pkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = rmx3823Pkg/rmx3823.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = Mu drivers only. ABL's framebuffer keeps scanning out and SimpleFbDxe
  #     publishes a GOP over it, so the UniXNU menu is visible without a single
  #     vendor hardware driver.
  # 1 = also load the stock vendor DXE blobs from Binaries/qrd8550 (SMEM, clocks,
  #     RPMh, UFS, USB, charger, ...). This is the QRD8550 driver set, not an
  #     RMX3823 one - it has never been run on this board.
  #
  # Start at 0. On sheng the vendor chain blanked the panel even with the board's
  # own panel data present, and rmx3823 has no panel data at all yet.
  #
  RMX3823_VENDOR_HW              = 0

  #
  # 0 = SM8550-AB
  # 1 = SM8550-AC
  #
  SOC_TYPE                       = 0

!include KailuaPkg/KailuaPkg.dsc.inc

!if ($(USE_CUSTOM_DISPLAY_DRIVER) == 1) && ($(RMX3823_VENDOR_HW) == 0)
  !error The stock DisplayDxe chain needs RMX3823_VENDOR_HW = 1
!endif

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"realme"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"RMX3823"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"RMX3823"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"RMX3823"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"RMX3823"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1240
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2772
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # UNIXNU BRING-UP TUNING -- readability of the on-panel log
  #
  # Delay: microseconds waited after every newline.
  # 30000 (30 ms/line) is a compromise: fast enough that the whole SEC+DXE
  # log fits in the watchdog window, slow enough to read.
  #
  # FontScale: 0 means "derive from the short edge". Fixed at 2 here:
  # 1240/((5+1)*2) = 103 chars per line, 2772/((16-4)*2) = 115 lines per
  # screen. That is more lines than SEC ever writes, so nothing SEC prints is
  # scrolled off before the failure point is reached.
  #
  # FreezeWhenFull FALSE: DXE dispatch prints several hundred lines, far more
  # than 115 screen lines. Freezing (the 2026-11-10 PAD-build setting) hid the
  # BDS end state below a full screen. Scroll instead: the panel keeps the LAST
  # 115 lines, which is what we want now that the failure point moved to the
  # end of the boot.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|30000
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferFontScale|2
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferFreezeWhenFull|FALSE

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"
!if $(RMX3823_VENDOR_HW) == 0
  # PlatformPeim otherwise walks the stock UEFI's leftover FD window and calls
  # into its scheduler protocol. With no vendor drivers in the volume nothing
  # consumes those HOBs, and on sheng that produced an intermittent stall right
  # after "MMU: enabled".
  gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|FALSE
!endif

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|rmx3823Pkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|rmx3823Pkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # UniXNU Libraries
  #
  # UniXNULib is shared by the menu application, the boot option registrar and
  # the RAM disk driver. FdtLib is the MdePkg base implementation.
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

[Components]
  #
  # ACPI Tables
  #
  # The QRD8550 tables are SoC-generic and Realme has no SM8550 ACPI package
  # upstream. XNU does not consume ACPI, so these only matter if a Linux
  # payload is ever booted on this board.
  #
  qrd8550/AcpiTables.inf

  #
  # UniXNU
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
