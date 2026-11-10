/**
  Copyright (c) 2011-2017, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/PrePiLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/HobLib.h>
#include <Library/CpuLib.h>
#include <Library/ExtractGuidedSectionLib.h>
#include <Library/MemoryAllocationLib.h>

#include <Pi/PiFirmwareFile.h>
#include <Pi/PiFirmwareVolume.h>
#include <Pi/PiHob.h>

#include "Sec.h"

// First argument (x0) of the previous stage, saved by ModuleEntryPoint.S
extern UINT64  gSecEntryArg0;

// The library constructors ProcessLibraryConstructorList () runs for this module, in its generated order, called one by one so the screen
// shows which one a stall is in (an entry here that the generated list lacks fails to link, which is the check).
// Helper.S: clean and invalidate every data cache level by set/way (see there).
extern VOID SecCleanInvalidateDataCacheAll (VOID);

extern RETURN_STATUS EFIAPI BaseDebugLibSerialPortConstructor (VOID);
extern RETURN_STATUS EFIAPI ExtractGuidedSectionLibConstructor (VOID);
extern RETURN_STATUS EFIAPI ArmMmuBaseLibConstructor (VOID);
extern RETURN_STATUS EFIAPI LzmaDecompressLibConstructor (VOID);

// Walks the HOB list with a bound, so a zero-length or runaway entry is reported instead of looping forever in the constructors and in
// the FV decompression that follow (both search the list).
STATIC
VOID
CheckHobList (VOID)
{
  EFI_PEI_HOB_POINTERS  Hob;
  UINTN                 Count = 0;

  Hob.Raw = GetHobList ();
  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobLength < sizeof (EFI_HOB_GENERIC_HEADER) || Count >= 16384) {
      DEBUG ((EFI_D_ERROR, "SEC: HOB list broken at entry %u: type 0x%x length %u\n", (UINT32)Count, (UINT32)Hob.Header->HobType, (UINT32)Hob.Header->HobLength));
      return;
    }

    Count++;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  DEBUG ((EFI_D_WARN, "SEC: HOB list ok, %u entries\n", (UINT32)Count));
}

//
// 步骤打点：日志现在能完整显示，屏幕底部那条像素墓碑已经撤掉。
// 每一句 "[SEC nn]" 就是一步，哪一句没出现，就是死在那一步。
//

VOID
SecTombstone (
  IN UINT32  Step
  )
{
  DEBUG ((EFI_D_WARN, "[SEC %02d]\n", Step));
}

//
// FV 现场诊断。DecompressFirstFv 只告诉我们"找没找到"，不告诉我们"看见了什么"。
// 这里把 SEC 眼里的 FV 头、前几个 FFS 条目、FV HOB、以及两次查找的返回码全部打出来：
//   - 如果 head 与固件文件不一致 -> 内存里的 FD 根本不是我们打包的那份（拷贝/地址问题）
//   - 如果 ffs[] 里 type=0x0B 存在但 find FV_IMAGE 失败 -> 校验和或状态位问题
//   - 如果 FV_IMAGE 找到了却解不开 -> GUIDED/LZMA 解压器没注册
//
STATIC
VOID
DumpFvState (VOID)
{
  EFI_STATUS           Status;
  EFI_PEI_FV_HANDLE    Volume   = NULL;
  EFI_PEI_FILE_HANDLE  File     = NULL;
  UINT8               *Base     = (UINT8 *)(UINTN)FixedPcdGet64 (PcdFvBaseAddress);
  UINT64               FvLength;
  UINT16               HdrLength;
  UINT16               Checksum;
  UINT32               Attributes;
  UINTN                Off;
  UINTN                FvHobs   = 0;
  EFI_PEI_HOB_POINTERS Hob;

  CopyMem (&FvLength,   Base + 0x20, sizeof (FvLength));
  CopyMem (&Attributes, Base + 0x2C, sizeof (Attributes));
  CopyMem (&HdrLength,  Base + 0x30, sizeof (HdrLength));
  CopyMem (&Checksum,   Base + 0x32, sizeof (Checksum));

  DEBUG ((EFI_D_WARN, "FVDBG base=0x%lx size=0x%x\n", (UINT64)(UINTN)Base, FixedPcdGet32 (PcdFvSize)));
  DEBUG ((EFI_D_WARN, "FVDBG head=%02x%02x%02x%02x %02x%02x%02x%02x\n",
          Base[0], Base[1], Base[2], Base[3], Base[4], Base[5], Base[6], Base[7]));
  DEBUG ((EFI_D_WARN, "FVDBG sig=%02x%02x%02x%02x fvlen=0x%lx attr=0x%x hdr=0x%x cks=0x%x\n",
          Base[0x28], Base[0x29], Base[0x2A], Base[0x2B], FvLength, Attributes, HdrLength, Checksum));

  // 前 5 个 FFS 头，按内存里的原样解析
  Off = HdrLength;
  for (UINTN i = 0; i < 5; i++) {
    UINT8  State;
    UINT8  Type;
    UINT32 Size;
    UINT8  Sum;

    if (Off + 24 > FvLength) {
      break;
    }

    State = Base[Off + 23];
    Type  = Base[Off + 18];
    Size  = Base[Off + 20] | (Base[Off + 21] << 8) | (Base[Off + 22] << 16);
    Sum   = 0;
    for (UINTN k = 0; k < 24; k++) {
      Sum = (UINT8)(Sum + Base[Off + k]);
    }

    Sum = (UINT8)(Sum - State - Base[Off + 17]);
    DEBUG ((EFI_D_WARN, "FVDBG ffs[%u] off=0x%x type=0x%x size=0x%x state=0x%x chk=0x%x\n",
            (UINT32)i, (UINT32)Off, (UINT32)Type, Size, (UINT32)State, (UINT32)Sum));
    if ((Type == 0xFF) || (Size == 0)) {
      break;
    }

    Off = (Off + Size + 7) & ~7;
  }

  // FV HOB 有几个、分别指向哪
  Hob.Raw = GetHobList ();
  if (Hob.Raw != NULL) {
    Hob.Raw = GetNextHob (EFI_HOB_TYPE_FV, Hob.Raw);
    while (Hob.Raw != NULL) {
      DEBUG ((EFI_D_WARN, "FVDBG hob[%u] base=0x%lx len=0x%lx %a\n",
              (UINT32)FvHobs,
              (UINT64)Hob.FirmwareVolume->BaseAddress,
              (UINT64)Hob.FirmwareVolume->Length,
              (Hob.FirmwareVolume->BaseAddress == (UINT64)(UINTN)Base) ? "MATCH" : "OTHER"));
      FvHobs++;
      Hob.Raw = GetNextHob (EFI_HOB_TYPE_FV, GET_NEXT_HOB (Hob));
    }
  }

  DEBUG ((EFI_D_WARN, "FVDBG fv hob count=%u\n", (UINT32)FvHobs));

  // 把两步查找分开报，才能区分"没有卷"和"有卷没文件"
  Status = FfsFindNextVolume (0, &Volume);
  DEBUG ((EFI_D_WARN, "FVDBG FvFindNextVolume(0)=%r vol=0x%lx\n", Status, (UINT64)(UINTN)Volume));
  if (!EFI_ERROR (Status)) {
    Status = FfsFindNextFile (EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE, Volume, &File);
    DEBUG ((EFI_D_WARN, "FVDBG find FV_IMAGE=%r file=0x%lx\n", Status, (UINT64)(UINTN)File));
    if (!EFI_ERROR (Status) && (File != NULL)) {
      // 第一个 section 是 GUIDED/LZMA。段头 24 字节 + 压缩数据开头。
      // 磁盘上的正确值：GUID 段头 4 字节 = f7 65 02 98 开头？直接对照下方两组值。
      UINT8  *Sec  = (UINT8 *)File + 24;
      UINT16  DO;

      CopyMem (&DO, Sec + 20, sizeof (DO));
      DEBUG ((EFI_D_WARN, "FVDBG guided sec: size=0x%x type=0x%x dataoff=0x%x\n",
              (UINT32)(Sec[0] | (Sec[1] << 8) | (Sec[2] << 16)), (UINT32)Sec[3], (UINT32)DO));
      DEBUG ((EFI_D_WARN, "FVDBG sec payload=%02x%02x%02x%02x %02x%02x%02x%02x (flat=04a03917+00)\n",
              Sec[DO], Sec[DO + 1], Sec[DO + 2], Sec[DO + 3],
              Sec[DO + 4], Sec[DO + 5], Sec[DO + 6], Sec[DO + 7]));

      //
      // DTBSYNC + CHKSUM：查传输踩踏。ABL 先 gunzip 再摆 DTB，如果 DTB
      // 被放进了 gunzip 输出的内部，拷贝过来的 FD 里就会出现 FDT 魔数
      // （d00dfeed）。同时把压缩流分 8 段求和，与磁盘基准逐段比对。
      //
      {
        UINTN  FdSize = FixedPcdGet32 (PcdFdSize);
        UINTN  Found  = 0;

        for (UINTN o = 0; (o + 4 <= FdSize) && (Found < 3); o++) {
          if ((Base[o] == 0xD0) && (Base[o + 1] == 0x0D) && (Base[o + 2] == 0xFE) && (Base[o + 3] == 0xED)) {
            DEBUG ((EFI_D_WARN, "DTBSYNC magic at FD+0x%lx\n", (UINT64)o));
            Found++;
            o += 3;
          }
        }

        if (Found == 0) {
          DEBUG ((EFI_D_WARN, "DTBSYNC none inside FD\n"));
        }

        {
          UINTN  LzStart = (UINTN)((UINT8 *)Sec + DO - Base);
          UINTN  SSize   = (UINTN)(Sec[0] | (Sec[1] << 8) | (Sec[2] << 16));
          UINTN  LzLen   = SSize - DO;
          UINTN  Chunk   = LzLen / 8;

          for (UINTN c = 0; c < 8; c++) {
            UINT32 Sum = 0;
            UINTN  End = (c == 7) ? LzLen : (c + 1) * Chunk;

            for (UINTN o = c * Chunk; o < End; o++) {
              Sum += Base[LzStart + o];
            }

            DEBUG ((EFI_D_WARN, "CHKSUM[%u] 0x%lx+0x%lx = 0x%08x\n",
                    (UINT32)c, (UINT64)(LzStart + c * Chunk), (UINT64)(End - c * Chunk), Sum));
          }
        }
      }
      {
        RETURN_STATUS  Ts;
        UINT32         OutSize     = 0;
        UINT32         ScratchSize = 0;
        UINT32         Auth        = 0;
        UINT16         Attr        = 0;
        VOID          *Scratch     = NULL;
        VOID          *Dest        = NULL;

        Ts = ExtractGuidedSectionGetInfo (Sec, &OutSize, &ScratchSize, &Attr);
        DEBUG ((EFI_D_WARN, "LZTEST GetInfo=%r out=0x%x scratch=0x%x attr=0x%x\n", Ts, OutSize, ScratchSize, (UINT32)Attr));
        if (!EFI_ERROR (Ts) && (OutSize != 0)) {
          Scratch = AllocatePages (EFI_SIZE_TO_PAGES (ScratchSize));
          Dest    = AllocatePages (EFI_SIZE_TO_PAGES (OutSize) + 1);
          DEBUG ((EFI_D_WARN, "LZTEST scratch=0x%lx dest=0x%lx\n",
                  (UINT64)(UINTN)Scratch, (UINT64)(UINTN)Dest));
          if ((Scratch != NULL) && (Dest != NULL)) {
            EFI_PEI_HOB_POINTERS  H;

            Dest = (UINT8 *)Dest + EFI_PAGE_SIZE - 4;
            Ts   = ExtractGuidedSectionDecode (Sec, &Dest, Scratch, &Auth);
            DEBUG ((EFI_D_WARN, "LZTEST Decode=%r auth=0x%x dest=0x%lx\n", Ts, Auth, (UINT64)(UINTN)Dest));
            DEBUG ((EFI_D_WARN, "LZTEST dest[0..7]=%02x%02x%02x%02x %02x%02x%02x%02x (want 04000019 04a03917)\n",
                    ((UINT8 *)Dest)[0], ((UINT8 *)Dest)[1], ((UINT8 *)Dest)[2], ((UINT8 *)Dest)[3],
                    ((UINT8 *)Dest)[4], ((UINT8 *)Dest)[5], ((UINT8 *)Dest)[6], ((UINT8 *)Dest)[7]));

            H.Raw = GetHobList ();
            DEBUG ((EFI_D_WARN, "LZTEST hob freeTop=0x%lx freeBottom=0x%lx\n",
                    (UINT64)H.HandoffInformationTable->EfiFreeMemoryTop,
                    (UINT64)H.HandoffInformationTable->EfiFreeMemoryBottom));
          }
        }
      }
    }

    File   = NULL;
    Status = FfsFindNextFile (EFI_FV_FILETYPE_DXE_CORE, Volume, &File);
    DEBUG ((EFI_D_WARN, "FVDBG find DXE_CORE=%r file=0x%lx\n", Status, (UINT64)(UINTN)File));
  }

  // GUIDED 段解压器注册表。FV_IMAGE 里那一段用的是 LZMA（EE4E5898-...），
  // 如果这里没有它，解压必然返回 Not Found / Invalid Parameter。
  {
    GUID  *GuidList = NULL;
    UINTN  Count    = ExtractGuidedSectionGetGuidList (&GuidList);

    DEBUG ((EFI_D_WARN, "FVDBG guided handlers=%u\n", (UINT32)Count));
    for (UINTN i = 0; i < Count && i < 4; i++) {
      DEBUG ((EFI_D_WARN, "FVDBG handler[%u] d1=0x%x d4=%02x%02x%02x%02x\n",
              (UINT32)i, GuidList[i].Data1,
              GuidList[i].Data4[0], GuidList[i].Data4[1], GuidList[i].Data4[2], GuidList[i].Data4[3]));
    }
  }
}

#ifndef MDEPKG_NDEBUG
STATIC
VOID
ClearFrameBuffer (VOID)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion;

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);

  // Clear Memory Region
  if (FrameBufferRegion.Address != 0 && FrameBufferRegion.Length != 0) {
    ZeroMem ((VOID *)FrameBufferRegion.Address, FrameBufferRegion.Length);
  }
}

STATIC
VOID
PrintFirmwareVersion (VOID)
{
  // Clear Frame Buffer
  ClearFrameBuffer ();

  // Print UEFI Version Message
  DEBUG ((EFI_D_WARN, "\n"));
  DEBUG ((EFI_D_WARN, "Project Silicium for %a %a\n",            FixedPcdGetPtr (PcdSmbiosSystemManufacturer), FixedPcdGetPtr (PcdSmbiosSystemModel)));
  DEBUG ((EFI_D_WARN, "Firmware Version %s Build at %a on %a\n", PcdGetPtr (PcdFirmwareVersionString), __TIME__, __DATE__));
  DEBUG ((EFI_D_WARN, "\n"));
}
#endif

STATIC
EFI_STATUS
InitializeMemory (VOID)
{
  EFI_STATUS                    Status;
  EFI_MEMORY_REGION_DESCRIPTOR  UefiMemoryRegion;
  EFI_HOB_HANDOFF_INFO_TABLE   *HobList;

  // Locate "DXE Heap" Memory Region
  LocateMemoryRegionByName ("DXE Heap", &UefiMemoryRegion);
  LocateMemoryRegionByName ("DXE_Heap", &UefiMemoryRegion);

  // Save UEFI Memory Details
  UINTN UefiMemoryBase = UefiMemoryRegion.Address;
  UINTN UefiMemorySize = UefiMemoryRegion.Length;

  // Verify UEFI Memory Base
  if (UefiMemoryBase == 0 && UefiMemorySize == 0) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"DXE Heap\" Memory Region!\n"));
    return EFI_NOT_FOUND;
  }

  // Initialize HOB List
  HobList = HobConstructor ((VOID *)UefiMemoryBase, UefiMemorySize, (VOID *)UefiMemoryBase, (VOID *)(UefiMemoryBase + UefiMemorySize));
  PrePeiSetHobList (HobList);

  // Initialize MMU & Memory HOBs
  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Build Stack HOB
  BuildStackHob (FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));

  return EFI_SUCCESS;
}

STATIC
VOID
SecMain (VOID)
{
  EFI_STATUS Status;

  // STEPS 01-09：进入 SEC 到打印出固件版本。停在 01 说明连 PlatformInitialize 都没回来。
  SecTombstone (1);

  // Do Platform Specific Initialization
  PlatformInitialize ();
  SecTombstone (2);

#ifndef MDEPKG_NDEBUG
  // Initialize Serial Port
  Status = SerialPortInitialize ();
  if (EFI_ERROR (Status)) {
    SecTombstone (3);
    return;
  }
  SecTombstone (4);

  // Print Firmware Version
  PrintFirmwareVersion ();
  SecTombstone (5);

  // Initialize Debug Agent
  InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  SaveAndSetDebugTimerInterrupt (TRUE);
  SecTombstone (6);
#endif

  // Initialize Memory
  Status = InitializeMemory ();
  if (EFI_ERROR (Status)) {
    SecTombstone (7);
    return;
  }
  SecTombstone (8);

  DEBUG ((EFI_D_WARN, "SEC: memory HOBs done\n"));

  // Build CPU HOB
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));
  SecTombstone (9);

  // Set the Boot Mode
  SetBootMode (BOOT_WITH_DEFAULT_SETTINGS);
  SecTombstone (10);

  // Initialize Platform HOBs
  Status = PlatformPeim ();
  DEBUG ((EFI_D_WARN, "SEC: platform HOBs done (%r)\n", Status));
  if (EFI_ERROR (Status)) {
    SecTombstone (11);
    return;
  }
  SecTombstone (12);

  // Pass on what the previous stage handed over in x0 (a DT address for a
  // Linux-style boot loader), so a DXE driver can use it.
  {
    UINT64  EntryArg = gSecEntryArg0;

    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));
  }
  SecTombstone (13);

  CheckHobList ();
  SecTombstone (14);

  // Run Library Cconstructors
  DEBUG ((EFI_D_WARN, "SEC: constructor debug\n"));
  BaseDebugLibSerialPortConstructor ();
  SecTombstone (15);
  DEBUG ((EFI_D_WARN, "SEC: constructor guided-section\n"));
  ExtractGuidedSectionLibConstructor ();
  SecTombstone (16);
  DEBUG ((EFI_D_WARN, "SEC: constructor mmu\n"));
  ArmMmuBaseLibConstructor ();
  SecTombstone (17);
  DEBUG ((EFI_D_WARN, "SEC: constructor lzma\n"));
  LzmaDecompressLibConstructor ();
  SecTombstone (18);
  DEBUG ((EFI_D_WARN, "SEC: constructors done\n"));
  CheckHobList ();
  SecTombstone (19);

  // Decompress all FVs
  DumpFvState ();
  Status = DecompressFirstFv ();
  DEBUG ((EFI_D_WARN, "SEC: FV decompressed (%r)\n", Status));
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Decompress FV! Status = %r\n", Status));
    SecTombstone (20);
    // 停在这里而不是 return：return 会把控制权交回一片没人接管的地址，
    // 屏幕内容会被冲掉，反而看不到上面这些诊断信息。
    CpuDeadLoop ();
  }
  SecTombstone (21);

  // Load DXE Core
  DEBUG ((EFI_D_WARN, "SEC: loading DXE core\n"));
  Status = LoadDxeCoreFromFv (NULL, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Load DXE Core! Status = %r\n", Status));
    SecTombstone (22);
  }
  SecTombstone (23);
}

VOID
SecEntry (VOID)
{
  // The previous stage ran with its caches on. Clean and invalidate them before anything else: the UEFI writes its HOB tables and heap with
  // the MMU and caches off, and lines the previous stage left would hide those writes (or be written back over them) once the caches are on.
  SecCleanInvalidateDataCacheAll ();

  // Disable Data Cache
  ArmDisableDataCache ();

  // Invalidate Instruction Cache
  ArmInvalidateInstructionCache ();

  // Enable Instruction Cache
  ArmEnableInstructionCache ();

  // Write back and invalidate Stack & UEFI FD D-Cache. A plain invalidate would discard what the entry code just stored there
  // (live stack frames, gSecEntryArg0) if the previous stage left the data cache on.
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdFdBaseAddress),     FixedPcdGet32 (PcdFdSize));

  // Verify Exception Vector Table
  ASSERT (((UINTN)SecVectorTable & ARM_VECTOR_TABLE_ALIGNMENT) == 0);

  // Enable new Exception Vector Table
  ArmWriteVBar ((UINTN)SecVectorTable);

  // Enter Main SEC Function
  SecMain ();
}
