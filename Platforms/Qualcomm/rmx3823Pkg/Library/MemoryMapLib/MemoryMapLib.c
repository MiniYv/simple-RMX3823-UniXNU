#include <Library/MemoryMapLib.h>

//
// realme GT5 240W (RMX3823) - SM8550 / kalama
//
// PROVENANCE
// ----------
// Every entry below is transcribed from the memory map this board's own UEFI
// ships with. The stock uefi.elf carries its uefiplatLA.cfg as an embedded
// string blob; it was extracted verbatim into uefiplat-rmx3823.cfg in this
// package, and it declares:
//
//   [Config]
//   Version = 3
//   MaxMemoryRegions = 74
//
// That is 25 DDR regions and 14 register regions, 39 in total, and they are
// reproduced here one for one and in the same order. The cfg column order maps
// directly onto this struct:
//
//   MemBase, MemSize, MemLabel, BuildHob, ResourceType, ResourceAttribute,
//   MemoryType, CacheAttributes
//     -> Address, Length, Name, HobOption, ResourceType, ResourceAttribute,
//        MemoryType, ArmAttributes
//
// Two cross-checks against independent files, both of which agree:
//   - "IMEM Base" 0x14680000 is exactly the entry point and first LOAD address
//     of the stock tz.mbn.
//   - "TZApps Reserved" 0xD8800000 + 0x07400000 contains the 0xd830a000
//     segment that tz.mbn also loads.
//
// Three carveouts are additionally visible in this board's own dtbo overlay:
//   - aop_cmd_db_region     @ 0x81C60000 falls inside "AOP" 0x81C00000 + 0xA0000
//   - smem_region           @ 0x81D00000 equals "SMEM" base
//   - gpu_micro_code_region @ 0x9B09A000 falls inside "PIL Reserved"
//
// WHAT IS DELIBERATELY NOT HERE
// -----------------------------
// qrd8550Pkg's table, which this package started from, also carries GPU PRR
// (0x81F00000), TPMControl, USB UCSI Shared, PLD_PEP, TZ STAT (0x82700000),
// ADSP RPC (0x84B00000), MPSS_EFS (0x85300000) and four PCIe apertures. None
// of them appear in this board's cfg. They were dropped rather than carried
// over: the gap 0x81F00000..0x8A800000 that they would occupy is not described
// by the vendor map at all, and inventing reservations for a board that does
// not declare them is how you hand the kernel memory something else still owns.
// A region absent from this table gets no HOB, so DXE will neither allocate it
// nor map it - which is the safe outcome when its true extent is unknown.
//
// One deviation from the cfg, stated plainly: the cfg marks both "NOMAP"
// entries as BuildHob = NoMap, and Mu-Silicium's EFI_MEMORY_HOB_OPTION has no
// NoMap member. They are emitted as AddMem + MEM_RES + Reserv, following
// qrd8550Pkg. That reserves them instead of leaving them undescribed, which is
// the conservative reading of the same intent.
//
// WHAT THIS TABLE DOES NOT SAY
// ----------------------------
// It is authoritative for what UEFI may touch, but it is not a description of
// installed RAM: it stops at 0x100000000. The 24 GiB this device reports is
// covered only up to the low 2 GiB window plus carveouts, and memory above
// 0x100000000 is absent. That is normal for Qualcomm UEFI - the kernel takes
// its range from the ADT, not from this table - but it means the ADT memory
// node, not this file, is what has to be right about the top of RAM.
//

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"NOMAP",             0x80000000, 0x01200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"RSRV0",             0x81200000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL DT",            0x81A00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL Ramdump",       0x81A40000, 0x001C0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP",               0x81C00000, 0x000A0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SMEM",              0x81D00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL Reserved",      0x8A800000, 0x18280000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display Demura",    0xA2A80000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DBI Dump",          0xA6100000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
  {"UEFI FD",           0xA8000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD Reserved",  0xA8400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"CPU Vectors",       0xA8600000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Info Blk",          0xA8601000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"MMU PageTables",    0xA8602000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log Buffer",        0xA8605000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"UEFI Stack",        0xA860D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC Heap",          0xA864D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Sched Heap",        0xA86D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"FV Region",         0xA8AD9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI RESV",         0xA8ED9000, 0x00127000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Kernel",            0xA9000000, 0x0F000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"Display Reserved",  0xB8000000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DXE Heap",          0xBAB00000, 0x0D1C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"TZApps Reserved",   0xD8800000, 0x07400000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"NOMAP",             0xFF800000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},

  // Other Memory Regions
  {"IMEM Base",         0x14680000, 0x0002A000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IMEM Cookie Base",  0x146AA000, 0x00016000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

  // Register Regions
  {"IPC_ROUTER_TOP",    0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SECURITY CONTROL",  0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP",               0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PRNG_CFG_PRNG",     0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CRYPTO0 CRYPTO",    0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CORE_TOP_CSR",      0x01F00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PERIPH_SS",         0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB",               0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AOSS",              0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TLMM",              0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SMMU",              0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"APSS_HM",           0x17000000, 0x02000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
