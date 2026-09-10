# 12 — 跨块设备读取与镜像抽取

**日期**：2026-09-10
**状态**：✅ 已从 HAL 域读取出 3 个分区镜像（MBOOT / MPOOL / mmcblk0boot0）

---

## 1. 为什么块设备是目标

`hal_tv_tvfactory_default` 域在策略里被授予：

```
(allow hal_tv_tvfactory_default mstar_block_device     (blk_file (ioctl read getattr lock map open)))
(allow hal_tv_tvfactory_default mstar_boot_block_device(blk_file (ioctl read getattr lock map open)))
(allow hal_tv_tvfactory_default mstar_mboot_block_device(blk_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_mpool_block_device(blk_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default block_device_29_0 (dir (search)))
```

`misysdiagnose` 域对 `/dev/block/` 连目录都 `search` 不了（`EACCES`），
**只有 HAL 域能摸到块设备**。这给出一条**从内核外部拿固件/内核镜像**的通路。

---

## 2. 分区布局

`/proc/partitions`（uid0 域可读）：

```
 179        0   30535680 mmcblk0      (≈29.1 GiB)
 179        1       5120 mmcblk0p1
 179        2       3072 mmcblk0p2
 179        3       5120 mmcblk0p3
 179        4       1024 mmcblk0p4
 179        5       1024 mmcblk0p5
 179        6      65536 mmcblk0p6
 179        7       1024 mmcblk0p7
 179        8     131072 mmcblk0p8
 179        9       3072 mmcblk0p9
 179       10       1024 mmcblk0p10
 ...
 254        0     512000 zram0
```

by-name 软链接目录：**`/dev/block/platform/mstar_mci.0/by-name/`**

---

## 3. 标签 → 路径映射（节选自 `vendor_file_contexts`）

| 分区名 | SELinux 类型 | HAL 可读 |
|---|---|---|
| `MBOOT` | `mstar_mboot_block_device` | ✅（且**可写**）|
| `MPOOL` | `mstar_mpool_block_device` | ✅（且**可写**）|
| `boot` | `boot_block_device` | ❌ |
| `recovery` | `recovery_block_device` | ❌ |
| `system` / `vendor` / `super` | `*_block_device` | ❌ |
| `tvconfig` | `mstar_tvconfig_block_device` | ❌ |
| `cusdata` | `mstar_cusdata_block_device` | ❌ |
| `project_id` | `mstar_project_id_block_device` | ❌ |
| — | `mstar_block_device`（**整盘**）| ✅ |
| `/dev/block/mmcblk0boot0` `/boot1` | `mstar_boot_block_device` | ✅（只读）|

---

## 4. 枚举结果（`tvhidl` mode 60）

49 条候选路径中，**6 条可被 HAL 以 `O_RDONLY` 打开**：

```
[01] rv=0   /dev/block/platform/mstar_mci.0/by-name/MBOOT
[02] rv=0   /dev/block/platform/mstar_mci.0/by-name/MPOOL
[42] rv=0   /dev/block/mmcblk0                  ← 整盘，含所有分区数据
[43] rv=0   /dev/block/mmcblk0boot0
[44] rv=0   /dev/block/mmcblk0boot1
[49] rv=0   /sys/kernel/mm/ksm/run
```

其余（`boot`/`system`/`vendor`/`recovery`/`super`/`tvconfig`/`dtb`/`optee`/… 共 41 条）全部 `rv=-1`，
并在 `dmesg` 中**没有任何 avc** ⇒ 拒绝发生在 SELinux 之后（`block_device_29_0` 只有 `dir search`，
具体分区的类型没有对应的 `allow`）。

**关键**：**整盘 `/dev/block/mmcblk0` 可打开**。
理论上只要能用 `read_file(path, offset, size, cb)`（支持 offset/size）就能读到 `boot` 分区的内核镜像 ——
**但这需要实现 HIDL callback**（见 `10-hidl-tvfactory-breakout.md` §6）。

---

## 5. 已抽取的镜像

抽取流程（全部免拆机、免重启）：

```
HAL copy_file(<blockdev>, /dev/TVIDL_XXX)     # 流式 1KB 缓冲
uid0: cat /dev/TVIDL_XXX > /sdcard/xxx.img     # uid0 域对 /sdcard 可写
adb pull /sdcard/xxx.img
```

| 镜像 | 大小 | MD5 | 内容判定 |
|---|---|---|---|
| `mboot.img` | 5,242,880 B | `0316af7b6bba5c29c64ae88676e26f4b` | MStar **mini-bootloader**（`MBOT-1106.0.10`、`Board=MT5872-H1V1-B2-S_MT5872`、`MBoot_IN=MMC_FLASH`、`Kernel=64BIT`、`Security=OPTEE SECURITY_BOOT`）。含 10042 条 `MDrv_*`/`HAL_*`/`_MApi_*` 符号。**不含 Linux 内核**。|
| `mpool.img` | 3,145,728 B | — | MStar memory-pool 分区 |
| `boot0.img` | 4,194,304 B | — | eMMC boot 区；无 ARM64 magic / gzip / `Linux version` ⇒ **不含内核** |

**判定方法**：在镜像里搜索 `ARM\x64`（ARM64 Image magic）、`\x1f\x8b\x08`（gzip）、
`\xfd7zXZ\x00`（xz）、`Linux version`、`uImage` magic。

**结论**：Linux 内核不在 HAL 可读的任何分区中 —— 它在 `boot` 分区（`boot_block_device`，HAL 无权限）。

---

## 6. 下一步

1. **实现 HIDL callback** ⇒ 用 `read_file("/dev/block/mmcblk0", boot_offset, len, cb)`
   按偏移读整盘拿内核镜像（`boot` 分区偏移可从 GPT 解析，或用 `mmcblk0` 头部扫描）。
2. 或在 `mboot.img` 里找 **GPT/分区表**，直接从整盘定位 `boot` 偏移。
3. `MPOOL` 与 `MBOOT` HAL **可写** —— 但改写可能影响启动，**风险极高，暂不动手**。

---

## 7. 复现

```bash
# 枚举
./tools/halrun.sh 60 /data/data/com.mediatek.tv.factory/probe-paths.txt X

# 抽 MBOOT
./tools/halrun.sh 2 /dev/block/platform/mstar_mci.0/by-name/MBOOT /dev/TVIDL_MBOOT
./tools/tv_root_exec.sh "cat /dev/TVIDL_MBOOT > /sdcard/mboot.img"
/d/Tools/platform-tools/adb.exe pull /sdcard/mboot.img ./mboot.img
```
