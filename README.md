# mitv-baymax-root

小米电视 **ES65 2022 款**（内部代号 `baymax`，MStar **MT5872** 平台，Android 10 / SDK 29）
免拆机提权研究 —— 从 `misysdiagnose` 后门到 **uid 0 + system_app 双命令执行通道**，
再到 **CVE-2023-32830（TVAPI OOB write）的底层逆向**，最终目标是把 root 提升到
「能改系统 / 关 SELinux / 刷机」的完整状态。

> 目标设备：Xiaomi Mi TV ES65 2022 / `MiTV_MTEQ0` / 192.168.1.164
> 前提：无需 root、无需开发者选项（`persist.adb.tcp.port=5555` 已持久化，adb 直连）
> 仓库：仅存放分析文档与工具，**不重新分发**任一下述版权二进制（见「免责声明」）。

---

## 状态总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1–4 | 设备识别 + 发现 `misysdiagnose` 后门 + **uid 0 命令执行**（`CapEff=0x3fffffffff`） | ✅ 完成，稳定 |
| 5 | 常驻 root daemon（`rootd`） | ✅ 完成 |
| 7–7c | MStar **MMA** 内核驱动 ioctl 协议还原 + 内核地址泄露 + 内存地图 | ✅ 完成（权威版） |
| 8 | 经 `/dev/mma` 做内核物理内存读写 | 🔬 **CMA 耗尽，路线关闭**（见下） |
| 9–11 | 爆破 `ITvService` AIDL → `system_app` 任意命令/自研二进制执行 | ✅ 完成，稳定 |
| 12 | `system_app` 打 `/dev/miomap` 物理内存映射 | ❌ mmap 打挂设备（doc 17） |
| 13 | 内核符号：kallsyms 封 → 改从 Image 反查 | 🔬 部分，见 doc 16 |
| 14 | `/dev/miomap` read/ioctl 通道 | ❌ pread 全部 EINVAL，关闭 |
| **15** | **CVE-2023-32830（TVAPI OOB write）可行性** | 🔬 触发链路成熟，漏洞面已定位到底层 |
| **16** | **底层库逐层逆向 → 定位 root 双 OS 目标** | ✅ **架构实锤**（关键，见下） |
| **17** | **uid0 通用文件拉取通道**（`uid0_pull.sh`） | ✅ 完成，高复用 |
| **27** | **HAL 域 `write_file_byte` 打通 MI_UTIL 内核命令通道**（寄存器级写） | ✅ 完成，可用 |
| **28** | **设备存储拓扑 + eMMC env 分区块定位 + `IMtkTvFApiSystem` 的 eMMC HIDL 方法枚举** | 🔬 情报就绪（env 写路线已暂停，见红线） |
| **29** | **CVE 触发框架闭环 + `mtktvapi-service` 主二进制拉取（OOB 载体 + exchange 函数地图）** | 🔬 service 已 pull，待专项逆向 |

---

## 当前攻坚焦点（阶段 29，README 最新）

### ⛺ 阶段 28（eMMC env 持久后门）——已按安全红线暂停

> 仅保留只读情报（分区拓扑 + cmdline `ENV=EMMC off0 sz64K`）；`set_emmc_env_var` /
> `write_emmc` / `upgrade_fw` **严禁调用**（无 UART 救不回，绝不做任何持久写）。详见
> [`docs/28-emmc-env-layout.md`](docs/28-emmc-env-layout.md) 与上文红线。

🔬 详细只读数据见下方「持久后门路线」保留的历史段。

---

### 🎯 CVE-2023-32830 载体逆向：`mtktvapi-service` 主二进制（阶段 29，最新）

设备重启后（2026-09-11 15:33），CVE 差分触发框架一键闭环：
- **Step0 safe PASS**：app_process 独立子进程从 system_app 域反射 + 加载 `TVNativeWrapper`/`MtkTvFactoryService` 成功；
- **uart/uartopen/all 全零崩溃**（`outputUART` handle−1 → -1、`*_exchange_data` 有长度预检）→ **证实 JNI 层非越界点**（与 docs/25 完全吻合）；
- 关键修复：app_process32 需 `-Djava.library.path=/vendor/lib -Djava.class.path=<dex>:<dex> /system/bin 主类`（-D 必须在目录前），否则 JNI 加载失败。

用 **uid0 通道**拉回 OOB 载体：
```
native/mtkvapi_work/mtktvapi_service.bin
  = /tools/bin/hw/vendor.mediatek.tv.mtktvapi@1.0-service  (288268 B)
```
- 确证 **import memcpy / __memcpy_chk** → OOB 在 service 主进程；
- 已定位全部 `hh_*`/`a_hidl_*exchange_data` HIDL 消费函数地图（doc29 §3.3）；
- **下一关：对 service 主二进制专项逆向，定位 memcpy 长度来自 HIDL `hidl_vec` 且未判界的写点**。

详见 [`docs/29-phaseB-service-bin-memmap.md`](docs/29-phaseB-service-bin-memmap.md)。

---

### 历史（阶段 28）持久后门路线 —— 只读细节

SELinux enforcing + AVB locked + verity enforcing（见下）堵死了所有「运行时改」的快捷 root 路，
所以把主攻方向压到 **改持久启动配置** → 内核启动时从 eMMC **环境变量区**读取 `androidboot.*`。
如果能只读拿到 env、确认含 `selinux`/`verity` 控制项，就有希望在**重启后**让设备进 permissive / 脱离 verity，
从而获得**完整 root（能改系统 / 关 SELinux）**，而非每次都要手动进 HAL 域。

**本阶段实测确立（已穷举 partition 全景 + HIDL eMMC 方法）：**

- **分区表**（`/dev/block/by-name/`，uid0 读出）共 31 分区：`MBOOT..vbmeta..boot..super..tvservice..userdata` 等，
  其中 `MBOOT`(mmcblk0p1)、`vbmeta`(mmcblk0p4)、`vbmeta_system`(mmcblk0p29) 为启动校验关键区。
  完整映射见 [`docs/28-emmc-env-layout.md`](docs/28-emmc-env-layout.md) §2。
- **cmdline**（system_app 域读全文）确认：`ENV=EMMC ENV_VAR_OFFSET=0x0 ENV_VAR_SIZE=0x10000` →
  **env 位于 eMMC 起始头部，64KB**；且当前**无** `androidboot.selinux` 显式覆盖（默认 enforcing）。
- **读取接口**：HAL 域 `IMtkTvFApiSystem` 暴露 `read_emmc`/ `get_emmc_env_var`/`set_emmc_env_var`/`write_emmc` 等
  eMMC 原始读写方法（signature 已从 mangled 符号还原，见 doc28 §4）。其中 `get_emmc_env_var` 与 `read_emmc`
  **带 `std::function` 回调**，纯 C 客户端构造该回调 ABI 是下一阶段的解锁点（本机已有一份 `libc++.so` 可反）。

**红线**：`set_emmc_env_var` / `write_emmc` / `upgrade_fw` **严禁调用**（无 UART 救不回，只读阶段）。

---

### ⚡ 决定性重构：CVE-2023-32830 的真实目标 = root 的 MStar DTV 双系统

经过 `uid 0` 通道拉出 HAL 主二进制、`libmtal.so`，并读取 `dtv_svc` 进程 maps/cmdline，
彻底搞清了这条 CVE 的真实执行往回跳栈 —— **漏洞 OOB 实现不在 Android /vendor 库，
而在一个独立的、以 root(uid 0) 运行的 MStar DTV 微系统进程（glibc 双架构）**。

```
Android (UI/binder)                      MStar Linux 微内核（root）
 App/TvService ── TVNative(Java)          ┌──────────────────────────────┐
   └→ libjni.so（长度校验）                │  /mnt/vendor/linux_rootfs/    │
       └→ HIDL stub（libmtktvapi_full.so）│    basic/dtv_svc  ← 主进程   │
           └→ cwrapper（a_scan_*_exchange│    libc-2.21.so（glibc！）    │
                dispatch vtable）          │    libapp_if_rpc.so（RPC桥）│
                    └─ ● ────────────────►│    libapi(DMX/VDEC/GFX).so   │
        root 的 dtvvc 命中 OOB          │    libmi / libdrv* / DirectFB  │
        = 直接 root，无需过 Android      └──────────────────────────────┘
          SELinux / cred toast
```

**为什么这是最干净的 root 目标**：`dtv_svc`（PID 3065，uid0/glibc）内若命中 OOB write，
直接获得 **root 代码执行**，不依赖 Android 侧任何 SELinux 策略或 cred 篡改。

**详见 [`docs/26-cve32830-mstar-double-os.md`](docs/26-cve32830-mstar-double-os.md)** —— 含完整
反汇编证据（`a_scan_*_exchange_data` 的 vtable dispatch）、MStar 双内核架构、uid0 拉取通道记录。

### 三条「快」路已全部实证证伪（2026-09-11）

| 路线 | 结论 | 文档 |
|---|---|---|
| `/dev/mma` 物理内存读写 | **CMA 耗尽**，`mma_alloc` 全 `ENOMEM`，cur 面「重 4KB 也分不出」 | `docs/18` |
| `/dev/miomap` mmap | `mmap` 动作本身打挂设备（两次），pread 全 EINVAL | `docs/17` |
| FIFO/RPC 直连 root dtv_svc | 入口被 deep 封装（fd / net 不可读、无命名 FIFO），**非便宜捷径** | `docs/26` §6 |

**存活方向**：CVE-2023-32830 需逆向 MStar 闭源 TV 栈（数月级）。`/mnt/vendor`（linux_rootfs）
SELinux 拒读（连 uid0 也不行），`dtv_svc` 静态拉不掉 → 只能走**动态差分**（Android 侧触发 +
`/proc/<pid>/maps` / dmesg 观察）。

---

## 已打通的真相之王（可利用权限）

### 通道一：uid 0（`misysdiagnose` 域，DAC 全通）— 阶段 4

`/init.mitv.rc` 有后门：把 `vendor.misysdiagnose.cmd.code` 写脚本路径，init(uid0) 即执行。
而 `TvService.transact(4400)` **无调用方鉴权**，任意 adb shell 可触发：

```bash
service call TvService 4400 s16 "s" s16 "/sdcard/cmd.sh"   # cmd.sh 以 uid 0 执行
```

一键执行器：`./tools/tv_root_exec.sh "id"` → `uid=0(root) context=u:r:misysdiagnose:s0`
通用任意文件拉取器：`./tools/uid0_pull.sh /vendor/bin/hw/xxx-service` （能拉 shell 读不到的 vendor 文件）

**局限**：SELinux 域受限——不能 `setenforce`、`insmod`、改 `/system`、改自身上下文。

### 阶段二：`system_app`（uid 1000）— 阶段 9–11

`TvService`（AIDL `ITvService`）全部方法无校验，枚举出三条危险通道：

| code | 方法 | 能力 |
|---|---|---|
| 1 | `systemPropertiesSet` | 写任意系统属性 |
| 2 | `writeSystemFile` | **任意路径写文件** |
| 3 | `runSystemCommand` | **任意命令执行（system_app / uid 1000）** |
| 4400 | misysdiagnose 属性 | uid 0 脚本执行（阶段 4） |

`system_app` 能打开 misysdiagnose 域全拒的 MStar 私有设备（`/dev/miomap`、`/dev/malloc`、
`/proc/utopia`…），且能在自建的 `system_app_data_file` 目录里**执行自研 armv7a ELF**。

> ⚠️ 动手前必读 [`docs/09-operational-hazards.md`](docs/09-operational-hazards.md) ——
> 记录两次把电视搞挂的经过（init 死锁、画面定格锁屏）与三层防护。

---

## 彻底关闭的物理内存路线（阶段 12–14）

之前一直押注「物理内存读写 → 改 `selinux_enforcing` / `cred`」，现已系统性关闭：

- `/dev/mma`：**CMA 耗尽**（`CmaTotal=24MB` 只剩 ~2MB），744 种组合 `mma_alloc` 全 `ENOMEM`，
  且 `mmap` 需要合法命中才可，起手就断。→ [doc 18](docs/18-mma-route-closed.md)
- `/dev/miomap`：**mmap 动作本身致命**（连映射 `/proc/cmdline` 标注的 DRAM 地址也崩，两次复现）；
  `pread(物理地址)` 全 EINVAL（偏移不当物理地址）。→ [doc 17](docs/17-miomap-mmap-fatal.md)
- 内核符号：`/proc/kallsyms` 只有名无地址（`kptr_restrict=2`）、`/proc/iomem` 三域全拒、
  `/proc/modules` … → [doc 16](docs/16-kernel-symbols-blackout.md)

### 阶段 7 关键修正：MMA ioctl 权威版

早期 MMA 表是猜测还原的，后从设备拉到 **`gralloc.mt5872.so`（未 strip）** 反汇编逐条读出
真实命令字与完整 struct 布局（magic `'M'`=0x4d）。详见 [`reverse/mma-ioctl-table.md`](reverse/mma-ioctl-table.md)。

---

## ⚡ 当前执行点：CVE-2023-32830 一键差分触发（待设备上线）

设备因既往盲扫 ioctl 理论触发击穿**曾离线**，现需**物理断电重启**才能恢复 adb。重启后，
`cve_dispatch/run_cve.sh` 已就绪，一条命令即可推进（严格独立子进程，零砖风险）：

```bash
# Step 0 （先做）——最小可行性验证：safe mode，仅反射 + 类加载，零风险
./cve_dispatch/run_cve.sh 0 safe

# Step 2 —— 差分触发：all 渐变长度扫描全部越界面（独立 app_process32 崩溃 = 可观测的 hit）
./cve_dispatch/run_cve.sh 1 all      # 或 1 uart / 1 esc 单独扫某一面
```

- **通道**：`sa3.sh`（`system_app` 域 + `runSystemCommand` 单窗口 exec）→ `app_process32` 独立子进程
- **classpath**：双裸 dex 冒号分隔 `/sdcard/cve_tv.dex:/sdcard/cve_tr.dex`，规避 multi-dex jar 的 PathClassLoader 只读 classes.dex 限制；`cve_tv.dex`=TvService 全类（目标 `TVNativeWrapper`），`cve_tr.dex`=`TvgTrigger` 主类
- **观测点**：`/sdcard/trig_log.txt` + `logcat -b crash` + tombstone；`TvgTrigger` 对每种长度 try/catch，越界即 SIGSEGV 可复现
- ⚠️ **红线**：只在独立 app_process32 子进程触发，绝不碰 TvService 主进程；不写 eMMC/env、不做 ioctl

（脚本：[`tools/run_cve.sh`](tools/run_cve.sh)。dex 触发资产因含版权类不留仓，仅存于本机
`cve_dispatch/`，脚本内已写死 host 绝对路径；dest 侧由脚本推送 `/sdcard/cve_tv.dex:cve_tr.dex`。）

---

## 目录结构

```
docs/    共 29 篇研究文档（设备 01 → CVE 底层定位 26 → HAL write_file 27 → eMMC env 布局 28 → CVE 载体逆向 29，推荐按编号顺序走）
tools/    自研工具（C/shell/python）
          —— tv_root_exec.sh(uid0执行) / uid0_pull.sh(uid0任意文件拉取)
             sysapp.sh / sarun.sh(system_app执行) / reconnect.sh(重启恢复)
             run_cve.sh(CVE-2023-32830 独立子进程差分触发，见下文「当前执行点」)
             mioprobe* / miostep(分级探测) / miroot2(PREL32查符号) / mma* / ...
reverse/  MMA ioctl 权威表 + 从设备抽取 blob 的脚本
policy/   关键 SELinux 规则摘录（misysdiagnose / system_app 权限画像）
recon/    原始侦察输出（设备节点、AIDL 事务码、cmdline 全文、proc 可读性矩阵…）
```

**推荐阅读路径**：`docs/01 → 02 → 03 → 07 → 08 → 09 → 1x → 19(全展望) → 20(CVE 计划) →
21/22/23(触发) → 24(路由判定) → 26(CVE 双 OS 实锤) → 27(HAL write_file 打通 MI_UTIL) →
28(eMMC env 布局) → 29(CVE 载体逆向)`。

---

## 一键恢复 / 环境

```bash
./tools/reconnect.sh    # 循环重连 adb（最长 120s）→ 校验 uid0 + system_app 两通道
```

- `persist.adb.tcp.port=5555` 已持久化；开机头 ~20–40s `connect` 报 10061 属正常，重试即可。
- 交叉编译：NDK r27c `armv7a-linux-androideabi21-clang`（动态链接更省事，无需 patch PT_TLS）
- 设备侧执行细节：`docs/03`

---

## 免责声明

本研究仅针对**本人自有设备**，用于维护、备份与本地化改造。所有二进制 blob（`utpa2k.ko`、
`libutopia.so`、APK、`dtv_svc` 等）均属于小米 / MStar 版权物，本仓库**不重新分发**，只提供
从设备自行抽取的脚本。请勿用于未授权设备。