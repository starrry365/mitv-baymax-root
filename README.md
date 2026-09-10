# mitv-baymax-root

小米电视 **ES65 2022 款**（内部代号 `baymax`，MStar **MT5872** 平台，Android 10 / SDK 29）
免拆机提权研究 —— 从 `misysdiagnose` 后门到 **uid 0 命令执行通道**，以及
MStar **MMA** 内核驱动的 ioctl 协议逆向（通往完整 root 的下一步）。

> 目标设备：Xiaomi Mi TV ES65 2022 / `MiTV_MTEQ0` / 192.168.1.164
> 前提：无需 root、无需开发者选项（`persist.adb.tcp.port=5555` 已持久化，adb 直连）

---

## 状态总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 设备识别 / 硬件巡检 | ✅ 完成 |
| 2 | 发现 `misysdiagnose` 后门（`/init.mitv.rc`） | ✅ 完成 |
| 3 | 定位无鉴权写入方 `TvService.transact(4400)` | ✅ 完成 |
| 4 | 打通 **uid 0 命令执行**（`CapEff=0x3fffffffff`） | ✅ 完成，稳定 |
| 5 | 常驻 root daemon（`rootd`） | ✅ 完成 |
| 6 | SELinux 域 `u:r:misysdiagnose:s0` 提权突破 | 🔬 进行中 |
| 7 | MStar MMA 驱动 ioctl 协议还原 | ✅ 完成（**权威版**，见下） |
| 7b | 内核地址泄露（`mma_get_pipeid`） | ✅ 完成 |
| 7c | 内核内存地图（`/proc/vmallocinfo`） | ✅ 完成 |
| 8 | 经 `/dev/mma` 做内核物理内存读写 → 改 cred / 关 SELinux | 🔬 下一步 |
| **9** | **爆破 `ITvService` AIDL → 三条无鉴权通道** | ✅ 完成 |
| **10** | **`system_app`（uid 1000）任意命令执行** | ✅ 完成，稳定 |
| **11** | **`system_app` 域执行自研原生二进制** | ✅ 打通（`system_app_data_file` 有 `execute`） |
| **12** | **`system_app` 域打 `/dev/miomap` / `/dev/malloc` 物理内存映射** | 🔬 下一步（**新首选**） |

### 第 7 阶段的关键修正

早期版本里的 MMA ioctl 表来自对立即数构造点的**猜测性还原，有多处错误**。
后来在设备上找到了真正实现这套 API 的 `gralloc.mt5872.so`（93 KB，未 strip），
**反汇编它逐条读出真实命令字**，并顺带解出完整的结构体布局与函数签名
（C++ 修饰名直接给出参数类型）。

详见 [`reverse/mma-ioctl-table.md`](reverse/mma-ioctl-table.md)。

### 意料之外的两项收获

* **`mma_get_pipeid` 泄露内核地址**：这条只读命令稳定返回两个
  `0xffffffc0xxxxxxxx` 形式的**内核线性映射地址**（每次调用值不同 ⇒ 真实内核堆对象）。
* **`/proc/vmallocinfo` 在受限域里完全可读**，直接暴露 MMA/CMA 的**物理地址区段**、
  各内核对象虚拟地址、以及内核函数符号与偏移 —— 配合 KASLR 关闭，
  相当于拿到了一张相当完整的内核内存地图。

### 当前阻塞点

`mma_alloc` 在 **744 种组合**（31 个 tag 名 × 3 尺寸 × 8 个 flags）下**全部返回 ENOMEM**。
根因指向 **CMA 耗尽**：`CmaTotal=24 MB` 而 `CmaFree` 只剩 **~2 MB**，
MMA 的物理内存正是从 CMA 分配（`/proc/vmallocinfo` 里的
`MsOS_MMA_CMA_Unauthorize+... phys=0x... ioremap [utpa2k]` 条目即为证据）。

因此下一步改为**绕开分配**，直接打「地址转换 / 句柄导入 / 授权」类命令
（`mma_map`、`mma_map_iova`、`mma_import_handle`、`mma_import_globalname`、
`mma_globalname_query`、`mma_buffer_authorize`、`mma_va2iova`），
这些命令的输入本来就是句柄或物理地址，不需要新分配。详见
[`docs/06-next-steps.md`](docs/06-next-steps.md)。

**当前权限**：`uid=0(root)`，全部 38 个 capability（`CapEff=0x3fffffffff`），
但 SELinux 域为受限的 `u:r:misysdiagnose:s0` —— 能读 `/dev/mma`、能 binder 到
system_server，但不能改 `/system`、不能 `insmod`、不能 `setenforce`。

---

## 1. 后门链（阶段 2–4）

`/init.mitv.rc` 中存在一条极明显的后门规则：

```
on property:vendor.misysdiagnose.cmd.code=*
    exec - root root -- /vendor/bin/misysdiagnose -${vendor.misysdiagnose.cmd.code} ${vendor.misysdiagnose.cmd.arguments}
```

只要有人把 `vendor.misysdiagnose.cmd.code` 写成脚本路径参数，**init（uid 0）**
就会执行 `/vendor/bin/misysdiagnose -<code> <arguments>`。

关键点在于**谁能写这个属性**：

* 该属性被声明的合法写方是 `system_app` 域；
* 实测 `TvService`（运行于 `u:r:system_app:s0`）暴露的 binder
  `transact(4400)` **没有任何调用方鉴权** —— 它把传入的
  `(code, arguments)` 直接 `property_set()` 进上述两个属性。

于是任意 adb shell（甚至局域网内任何能连 adb 的主机）都能触发：

```bash
service call TvService 4400 s16 "s" s16 "/sdcard/cmd.sh"
```

结果：`/sdcard/cmd.sh` 以 **uid 0** 被执行。

一键执行器见 [`tools/tv_root_exec.sh`](tools/tv_root_exec.sh)：

```bash
./tools/tv_root_exec.sh "id; cat /proc/version"
# => uid=0(root) gid=0(root) groups=0(root) context=u:r:misysdiagnose:s0
```

### 为什么不是完整 root

处于 `u:r:misysdiagnose:s0` 域，DAC 层面全通（uid 0 + 全部 capability），
但 SELinux 仍拦住了：

* `setenforce` / 写 `/sys/fs/selinux/enforce` → 拒绝
* `mount`/`remount`、`insmod`/`finit_module` → 拒绝
* `chcon` 改自身上下文 → 拒绝
* 大部分 MStar 私有设备节点（`/dev/miomap`、`/dev/malloc`、`/dev/tee0` …）→ 拒绝

因此阶段 4–5 拿到的是 **"受限 root"**：足够读大量系统数据、跑自研二进制、
常驻守护进程，但不足以改系统分区或关闭 SELinux。

---

## 1.5 TvService AIDL 突破：再拿一层 `system_app`（阶段 9–11）

`service list` 显示 `TvService: [mitv.internal.ITvService]` —— 它是**标准 AIDL 服务**，
实现类 `TvServiceDefaultImpl extends ITvService.Stub`，**全部方法都没有调用方校验**。

用「`Not a data message` = 事务码不存在」这个 oracle 枚举，再用三路副作用 oracle
（属性 / 文件 / 命令）定位危险方法，得到：

| code | 方法 | 能力 |
|---|---|---|
| **1** | `systemPropertiesSet(k, v)` | 写任意系统属性 |
| **2** | `writeSystemFile(path, content)` | **任意路径写文件** |
| **3** | `runSystemCommand(cmd)` | **任意命令执行（system_app / uid 1000）** |
| 4400 | （非 AIDL）misysdiagnose 属性写 | uid 0 脚本执行（阶段 4） |

```bash
adb shell 'service call TvService 3 s16 "id"'
# uid=1000(system) ... context=u:r:system_app:s0
```

**为什么这一层比 uid 0 更有用**：`system_app` 能打开 misysdiagnose 域被拒的
**全部 MStar 私有设备** —— `/dev/miomap`(物理内存映射)、`/dev/malloc`(物理内存分配)、
`/dev/system`、`/dev/msmailbox`、`/dev/scaler`、`/dev/semutex`，以及 `/proc/utopia`
（ioctl + rw + map）和可写的 `/proc/cmdline`。

更进一步：`system_app` 的 `execute` 权限只覆盖两个类型，其中
**`system_app_data_file` 是我们自己能创建的目录**（`/data/data/com.mediatek.tv.factory/`，
新建文件自动继承该标签）⇒ **可以把自研 armv7a ELF 放进去，在 `system_app` 域执行**。

* 详见 [`docs/07-tvservice-aidl-breakout.md`](docs/07-tvservice-aidl-breakout.md)
* 详见 [`docs/08-system-app-execution.md`](docs/08-system-app-execution.md)
* 权限清单 [`policy/system-app-privileges.txt`](policy/system-app-privileges.txt)
* 执行器 [`tools/sysapp.sh`](tools/sysapp.sh) / [`tools/sarun.sh`](tools/sarun.sh)

> ⚠️ **动手前请先读 [`docs/09-operational-hazards.md`](docs/09-operational-hazards.md)** ——
> 记录了两次把电视搞挂的经过（init 死锁、画面定格在锁屏），以及对应的三层防护。

---

## 2. 常驻通道（阶段 5）

[`tools/rootd.c`](tools/rootd.c) 是一个以 uid 0 常驻的用户态 daemon：
轮询 `/sdcard/.rcmd`，把命令结果写 `/sdcard/.rout`。
避免每次都走一遍 binder 触发（避开残留进程卡死 init exec 队列的问题）。

```
[pid=7914 uid=0] rootd v2 up, polling /sdcard/.rcmd
```

---

## 3. MMA 驱动逆向（阶段 7）

`/dev/mma`（SELinux 类型 `mstar_mma_device`）是 MStar TV SoC 的**物理内存分配器**
（Memory Management Agent）。它被策略显式授权给 misysdiagnose 域：

```
(allow misysdiagnose_29_0 mstar_mma_device (chr_file (ioctl read write open)))
```

这是**已到手 uid 0 中唯一可用的、直接操作内核内存的接口**，因此成为突破 SELinux 的
主要攻击面。

从设备上拉取的厂商模块 `utpa2k.ko`（未剥离符号，含 `MsOS_MMA_*`）与
`/vendor/lib/libutopia.so` 中还原出 **29 条 ioctl 命令**（magic = `'M'` = `0x4d`），
完整表见 [`reverse/mma-ioctl-table.md`](reverse/mma-ioctl-table.md)。

核心几条：

| ioctl | 名称 | 语义 |
|---|---|---|
| `0xc0304d03` | `mma_alloc_internal` | 分配物理内存，返回 handle |
| `0x40284d06` | `mma_map` | 把 handle 映射进进程地址空间 |
| `0xc0304d17` | `mma_va2iova` | 虚拟地址 → 物理地址 |
| `0xc0a84d00` | `mma_reserve_iova` | 预留 IOVA 区间 |
| `0xc0304d0b/0c` | `get_meminfo` / `get_heapinfo` | 读取堆信息 |
| `0x40284d08` | `mma_flush` | cache flush |

一个设计粗糙的 TV 内存分配器，如果允许把**任意物理地址**当作 handle 交给
`mma_map` / `mma_va2iova`，就是一条从用户态直达内核物理内存的读写通道 ——
这正是阶段 8 要验证的。

[`tools/mmatest.c`](tools/mmatest.c) 是上机验证程序（先用只读查询类命令探协议，
危险命令暂不调用）。

---

## 4. 目录结构

```
docs/     研究文档（设备、后门、uid0 通道、MMA、环境限制、下一步、
          TvService AIDL 突破、system_app 执行、操作风险）
tools/    自研工具源码（C / shell / python）
reverse/  MMA ioctl 逆向成果（表 + 从设备抽取 blob 的脚本）
policy/   关键 SELinux 规则摘录（misysdiagnose / system_app 权限画像）
recon/    原始侦察输出（设备节点、syscall 普查、进程、AIDL 事务码勘定等）
```

## 4.5 设备重启后的一键恢复

```bash
./tools/reconnect.sh          # 循环重连 adb（最长 120s）→ 校验 uid0 + system_app 两条通道
```

`persist.adb.tcp.port=5555` 已持久化，重启后 adbd 会自动起；
但开机头 ~20–40 秒内 `connect` 会报 `Connection refused (10061)`，属正常，重试即可。
首次 `connect` 若报 `failed to authenticate`，再连一次即 `device`。

## 5. 环境 / 编译

* 交叉编译器：Android NDK r27c `armv7a-linux-androideabi21-clang`
* 静态链接需把 ELF 的 `PT_TLS` 对齐 patch 成 32（Bionic 要求），
  或改用**动态链接**直接绕开（推荐）
* 设备侧执行：见 `docs/03-uid0-channel.md`

## 6. 免责声明

本研究仅针对**本人自有设备**，用于设备维护、备份与本地化改造。
所有二进制 blob（`utpa2k.ko`、`libutopia.so`、APK 等）**均为小米 / MStar 版权物**，
本仓库**不重新分发**，只提供从设备自行抽取的脚本。
请勿将本仓库内容用于未授权设备。
