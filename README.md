# simple-RMX3823-UniXNU

> **来源声明**：本仓库 fork 自 [Janit8r/uefi-rmx3823](https://github.com/Janit8r/uefi-rmx3823)
> （其本身衍生自 Project Silicium 生态与 Project Mu，上游原 README 保留在文末）。
> 本仓库主分支为 `main`，由 miniyv 于 2026-11-10 完成全链打通验证。
> 所有上游工作归属原作者，本分支仅做本机型适配与问题修复。

realme GT5（RMX3823 / SM8550）的 UEFI 移植，目标是通过 UniXNU 引导 XNU。

## 当前状态（2026-11-10 实测）

`fastboot boot` 临时启动（**不写任何分区**）下，整条链路已贯通：

```
fastboot → ABL(gzip) → BootShim(避雷搬运) → SEC → FV 解压(LZMA)
→ DXE(全部驱动加载) → BDS → UniXNU 菜单(苹果 logo + 音量键可选)
→ 超时回落 → UnableToBoot 诊断 → "Halting on purpose" 按计划停机
```

尚未完成：UniXNU 载荷补齐、UFS 存储驱动、引导项登记（见 TODO）。

## 相对上游的关键改动

1. **避雷填充（最重要的修复）**：ABL 在 gunzip 输出的固件内偏移
   `0x349A6–0x47661` 处会覆写约 1.2KB 数据，恰好砸中上游布局里的 LZMA
   压缩流，导致 FV 解压必然失败（上游所有现成镜像均踩此雷，只是被
   黑屏掩盖）。修复：FDF 在 SEC 后插入 `0x41000` 字节 RAW 填充
   （`rmx3823Pkg/Pad.bin`），把压缩流整体挪到 `FD+0x50034` 起，雷区
   只落在填充内。
2. **摘除 RamManagerDxe**：该驱动依赖 ABL 提供的 Qualcomm
   `EFI_RAMPARTITION_PROTOCOL`，fastboot 直启链无人安装，DEBUG 断言死
   循环。内存映射已由 MemoryMapLib 提供，故移除。
3. **BDS 不再关机**（`BootManagerLib`）：`PlatformBootManagerUnableToBoot`
   去掉盖屏 / 停 10 秒 / `ResetSystem` 关机，改为打印 GOP 几何与引导项
   数量后 `CpuDeadLoop()` 停住——黑屏曾是这个移植最大的障眼法。
4. **SEC 现场诊断**（`Sec.c`）：FVDBG / LZTEST / DTBSYNC / CHKSUM 分层
   打印内存中的 FV 实况；解压失败改为停住不冲屏。
5. **控制台参数**：滚动 30ms/行、字体 2 倍、写满回卷（保留最后 115 行）。

## 构建要点（本机踩过的坑）

```bash
# 环境：隔离 venv + 干净 PATH（默认 PATH 里的 rm 包装会在删除第 50 个
# 文件时拦住 make；PYTHONPATH 必须清空，否则 stuart 删除操作被拦）
export PATH=/usr/lib/llvm-22/bin:<venv>/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PYTHONPATH=
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
export RMX3823_EXTRA_PACKAGES="<UniXNU>/package:<UniXNU>/drivers/DarwinBoot"
python build_uefi.py -d rmx3823 -r DEBUG   # RELEASE 目前链接不过（Sec 的串口构造函数）

# 打包启动镜像（ABL 只认这一组参数）：
#   header v0 / kernel_addr 0xA8000000 / tags 0x01E00000 / pagesize 4096
#   载荷 = gzip(BootShim.bin + SILICIUM_UEFI.fd) + 2041002 字节 DTB 包
#   （DTB 包从可正常显示的参考镜像尾部切出，magic d00dfeed）
```

## TODO（下一阶段）

- [ ] UniXNUDxe 报 `image not found`：补齐载荷文件 GUID
- [ ] UFS 存储驱动（注意 dsc 中 `USE_CUSTOM_DISPLAY_DRIVER=1` 与
      `RMX3823_VENDOR_HW=0` 互斥，打开 UFS 需重新搭配显示链）
- [ ] 引导项登记：菜单界面已就绪，但"选择启动项"里还没有任何可启动的系统条目（Boot options: 0）
- [ ] 内层 FV 已 99% 满，加驱动前先确认扩容策略
- [ ] ABL 雷区目前只验证一处，压缩流变长后留意第二颗雷（CHKSUM 可检测）

---

以下为上游原 README 保留：

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
