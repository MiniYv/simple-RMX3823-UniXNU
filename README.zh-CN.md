# simple-RMX3823-UniXNU（中文版）

> **来源声明**：本仓库 fork 自 [Janit8r/uefi-rmx3823](https://github.com/Janit8r/uefi-rmx3823)
> （其本身衍生自 Project Silicium 生态与 Project Mu，上游原 README 保留在
> [英文版](README.md) 文末）。本仓库主分支为 `main`，由 miniyv 于 2026-11-10
> 完成全链贯通验证。所有上游工作归属原作者，本仓库仅包含本机型适配与缺陷修复。

realme GT5（RMX3823 / SM8550）的 UEFI 移植，目标是通过 UniXNU 引导 XNU。

## 当前状态（2026-11-10 硬件实测）

在 `fastboot boot`（临时启动，**不写入任何分区**）条件下，完整引导链已贯通：

```
fastboot → ABL(gzip) → BootShim → SEC → 固件卷解压(LZMA)
→ DXE（全部驱动派发）→ BDS → UniXNU 菜单（苹果 logo、音量键可选）
→ 超时回落 → UnableToBoot 诊断 → 按计划停机
```

尚未完成：UniXNU 载荷接通、UFS 存储驱动、引导项登记（见 TODO）。

## 相对上游的关键改动

1. **损坏窗口规避（最关键的修复）**。ABL 会对其 gunzip 输出缓冲区内一个
   固定偏移（固件偏移 0x349A6–0x47661）覆写约 1.2KB 数据。在上游布局中，
   该区域恰好位于 LZMA 压缩固件卷的中段，导致所有预构建镜像的固件卷
   解压均告失败（该故障长期被原版的盖屏关机行为所掩盖）。修复方式：
   FDF 在 SEC 之后插入 0x41000 字节的 RAW 填充段（`rmx3823Pkg/Pad.bin`），
   使压缩卷从 `FD+0x50034` 开始，整体避开损坏窗口。
2. **移除 RamManagerDxe**。该驱动依赖原厂链条中由 ABL 提供的高通
   RamPartition 协议；`fastboot boot` 链条中无人安装该协议，驱动因此
   断言。内存映射已由 MemoryMapLib 提供，该驱动并非必需。
3. **BDS 不再关机**。`PlatformBootManagerUnableToBoot` 移除了盖屏、
   等待 10 秒与 `ResetSystem` 关机逻辑，改为打印 GOP 几何与引导项数量，
   随后 `CpuDeadLoop()` 停机。原关机路径一直在掩盖真实故障点。
4. **SEC 现场诊断**（`Sec.c`）：以 FVDBG / LZTEST / DTBSYNC / CHKSUM
   分层打印内存中的固件实况；解压失败时停机而非返回。
5. **控制台参数**：每行 30ms、字体 2 倍、写满回卷（保留最后 115 行）。

## 构建要点

```bash
# 环境：隔离 venv + 干净 PATH（默认 PATH 中的 rm 包装器会在第 50 次删除时
# 阻断 make；PYTHONPATH 必须清空，否则 stuart 的删除操作会被拦截）
export PATH=/usr/lib/llvm-22/bin:<venv>/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PYTHONPATH=
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
export RMX3823_EXTRA_PACKAGES="<UniXNU>/package:<UniXNU>/drivers/DarwinBoot"
python build_uefi.py -d rmx3823 -r DEBUG   # RELEASE 目前链接失败（Sec 的串口构造函数）

# 启动镜像打包（ABL 仅接受这一组参数）：
#   header v0 / kernel_addr 0xA8000000 / tags 0x01E00000 / pagesize 4096
#   载荷 = gzip(BootShim.bin + SILICIUM_UEFI.fd) + 2041002 字节 DTB 包
#   （DTB 包自可正常显示的参考镜像尾部切出，magic d00dfeed）
```

## TODO（下一阶段）

- [ ] UniXNUDxe 报 `image not found`：接通载荷文件 GUID
- [ ] UFS 存储驱动（注意：dsc 中 `USE_CUSTOM_DISPLAY_DRIVER=1` 与
      `RMX3823_VENDOR_HW=0` 互斥，启用 UFS 需重新搭配显示链）
- [ ] 引导项登记：菜单界面已就绪，但启动项列表仍为空（Boot options: 0）
- [ ] 内层 FV 已占用 99%，新增驱动前需先确认扩容策略
- [ ] 目前仅验证了一处损坏窗口；若压缩流增长，需留意其他未验证的
      损坏区域（CHKSUM 可检测）
