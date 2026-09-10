# 14 — mik.ko 深度反汇编与内核内存读写分析（2026-09-10）

本文档记录对 mik.ko（MStar 内核驱动模块）的完整静态分析成果，
以及基于反汇编发现的内核内存读写能力与致命限制。

---

## 1. mik.ko 基本信息

| 属性 | 值 |
|---|---|
| 架构 | AArch64（ELF64，ET_REL 可重定位目标） |
| 大小 | 11,617,192 字节（11.6MB） |
| 是否 strip | **否**（4680 符号完整） |
| 各段总和 | 18,124,727 字节（17.5MB，含 .bss） |
| 加载基址 | `0xFFFFFF80023B3000`（vmallocinfo 最大块匹配确认） |
| 分析工具 | pyelftools + capstone 5.0.7（`.tools/venv`） |

### 段布局

| 段 | 大小 | 用途 |
|---|---|---|
| `.text` | 2,753,468 (2.7MB) | 代码 |
| `.rodata` | 1,148,480 (1.1MB) | 只读数据 |
| `.rodata.str1.1` | 1,150,339 (1.1MB) | 字符串 |
| `.data` | 85,584 (85KB) | 可变数据 |
| `.bss` | 12,984,512 (12.9MB) | 未初始化数据 |
| **总计** | **18,124,727 (17.5MB)** | |

---

## 2. MI_UTIL 命令系统完整逆向

### 命令处理函数

`MI_DEBUG_UTIL_ProcessDbgInfo` @ .ko offset `0x4d9e0`

这是一个命令分发器，逐条 `strcmp` 匹配命令字符串，匹配后跳转到对应 handler：

| 命令 | 跳转目标 | 功能 |
|---|---|---|
| `rbank <bank>` | 0x4db78 | 读取并打印寄存器 bank |
| `rreg <bank> <off>` | 0x4dc30 | 读取单个寄存器 |
| `wreg <bank> <off> <val>` | 0x4dcc0 | 写入单个寄存器 |
| **`rphy <PA>`** | **0x4dd80** | **读取物理地址（危险！）** |
| **`wphy <PA> <val>`** | **0x4df14** | **写入物理地址（危险！）** |
| **`rmem <VA>`** | **0x4de18** | **读取虚拟地址（危险！）** |
| **`wmem <VA> <val>`** | **0x4e058** | **写入虚拟地址（危险！）** |
| `color <0/1>` | 0x4e0c0 | 开关彩色提示 |
| `flag <val>` | 0x4e10c | 设置调试标志 |
| `dbg <module> <level>` | 0x4db78 | 设置模块调试级别 |
| `gethdmi <type>` | 0x4e190 | 获取 HDMI 信息 |
| `sethdmi <type>` | 0x4e1a8 | 设置 HDMI 参数 |

### 官方帮助文本（从 mik.ko 提取）

```
echo <cmd> <cmd_param1> <cmd_param2>... > /sys/kernel/mik/MI_UTIL

rbank [bank]: Show a registers bank
  e.g. "echo rbank 0x1015 > /sys/kernel/mik/MI_UTIL"

rreg [bank] [offset]: Read a register
  e.g. "echo rreg 0x1015 0x69 > /sys/kernel/mik/MI_UTIL"

wreg [bank] [offset] [value]: Write a register
  e.g. "echo wreg 0x1015 0x69 0x1234 > /sys/kernel/mik/MI_UTIL"

rphy [phy_addr]: Read a 32bits value from physical memory
  e.g. "echo rphy 0x400000 > /sys/kernel/mik/MI_UTIL"

wphy [phy_addr] [value]: Write a 32bits value to physical memory
  e.g. "echo wphy 0x400000 0x5678 > /sys/kernel/mik/MI_UTIL"

rmem [vir_addr]: Read a 32bits value from virtual memory
  e.g. "echo rmem 0x678934 > /sys/kernel/mik/MI_UTIL"

wmem [vir_addr] [value]: Write a 32bits value to virtual memory
  e.g. "echo wmem 0x678934 0x3322 > /sys/kernel/mik/MI_UTIL"

color [0/1]: Enable or disable color hint
  e.g. "echo color 1 > /sys/kernel/mik/MI_UTIL"

flag [value]: Set debug flag
  e.g. "echo flag 0x777 > /sys/kernel/mik/MI_UTIL"

dbg MI_[XXX] [0/0x20/0x30/0x40/0xF0]: Set debug level
  e.g. "echo dbg MI_VMON 0x30 > /sys/kernel/mik/MI_UTIL"
```

**注意**：官方示例中 `rphy` 的安全地址是 `0x400000`（4MB），
远低于用户之前尝试的 `0x43000000`（1.1GB，超出 MIU 窗口）。

---

## 3. 物理地址 ↔ 虚拟地址转换实现

### `rphy` handler（反汇编）

```asm
; 入口：x22=命令字符串指针, x19=参数1字符串指针
0x4dd80:  cbz   x19, error_exit        ; 参数检查
0x4dd98:  bl    MI_DEBUG_UTIL_DHConvert64  ; 字符串 → u64 PA
0x4dda8:  and   x0, x8, #0xfffffffffffffffc  ; 4字节对齐
0x4ddb0:  bl    MI_OS_Pa2NonCachedVa   ; PA → Uncached VA
0x4ddb4:  ldr   x1, [sp, #0x10]        ; VA
0x4ddb8:  cbnz  w0, error              ; 映射失败
0x4ddbc:  ldr   x2, [sp, #8]           ; VA
0x4ddc4:  mov   w0, #1                 ; direction=read
0x4ddc8:  bl    helper_0x4eeac         ; 执行读取并 printk
```

调用链：`rphy <PA>` → `MI_OS_Pa2NonCachedVa(PA, &VA)` → `_MI_OS_Pa2Kseg(PA, mode=2, &VA)` → `MsOS_PA2KSEG1(PA)`

### `_MI_OS_Pa2Kseg` 实现

```c
// mode=1 → MsOS_PA2KSEG0(PA)  // Cached 映射
// mode=2 → MsOS_PA2KSEG1(PA)  // Uncached 映射（rphy/rwphy 用这个）
// 返回 0 = 失败（越界），非 0 = VA
```

### `rmem` handler（反汇编）

```asm
0x4de18:  cbz   x19, error_exit
0x4de30:  bl    MI_DEBUG_UTIL_DHConvert64  ; 字符串 → u64 VA
0x4de3c:  ands  x0, x8, #-4               ; 4字节对齐 + 检查零
0x4de44:  b.eq  error_zero                ; VA=0 则退出
0x4de4c:  bl    MI_OS_Va2Pa               ; VA → PA
0x4de54:  cbnz  w0, error_va2pa           ; 转换失败则退出
0x4de64:  bl    helper_0x4eeac            ; 执行读取
```

调用链：`rmem <VA>` → `MI_OS_Va2Pa(VA, &PA)` → `MsOS_VA2PA(VA)`（返回 PA 或 -1）

---

## 4. 为什么 `rphy`/`rmem` 会崩溃

### `rphy 0x43000000` 崩溃根因

`MsOS_PA2KSEG1(0x43000000)` 是 MStar 私有的物理地址→虚拟地址映射函数。
对于超出 MIU（Memory Interface Unit）地址窗口的物理地址，
该函数**可能返回一个看似有效但实际未映射的 VA**，
后续的 `*(volatile u32*)VA` dereference 触发 **同步异常 → 内核 panic**。

MIU 窗口通常覆盖低 256MB 物理地址（`0x0`–`0x10000000`）。
官方示例 `0x400000`（4MB）在窗口内安全；
用户尝试的 `0x43000000`（1.1GB）远超窗口。

### `rmem` 即使读 vmalloc 区也危险

`rmem` 有 `MsOS_VA2PA` 保护（无效 VA 返回 -1 → 安全退出），
但对 vmalloc 区地址的转换行为不明确。
实测 `rmem 0xffffff80024009e0`（mik.ko 自己的代码段）导致 adbd 失联，
可能是 VA2PA 返回了 PA 后，后续的 PA→VA 重映射触发了问题，
或 HIDL/binder 调用超时导致 adbd 异常退出。

### 安全规则

> **MI_UTIL 的 `rphy`/`wphy`/`rmem`/`wmem` 四个命令全部禁用。**
> 只使用 `flag`/`color`/`rbank`/`rreg`/`wreg`（寄存器级操作，不涉及内存映射）。

---

## 5. 内核内存布局（从 cmdline + vmallocinfo 推导）

### 内核命令行关键参数

```
LX_MEM=0x22D00000                  # 主内存物理基址（574MB）
LX_MEM2=0x4A400000,0x53E00000      # 第二内存区（1.4GB）
sysrq_always_enabled=1             # SysRq 编译时启用
MIU_HIT_PANIC=OFF                  # MIU 命中不触发 panic
security=selinux                   # SELinux 启用
```

### 虚拟地址空间

- **VA_BITS = 39**（内核空间基址 `0xFFFFFF8000000000`）
- **内核模块区**：`0xFFFFFF8000DA0000` – `0xFFFFFF8038E8000`（vmalloc）
- **mik.ko 基址**：`0xFFFFFF80023B3000`（vmallocinfo 最大块 17.5MB 匹配）
- **KASLR 关闭**（`PAGE_OFFSET=0xFFFFFFC000000000` 固定）

### 物理内存布局

```
0x00000000 – 0x22D00000  : SoC 保留区（RIU/寄存器/固件）
0x22D00000 – 0x4A400000  : LX_MEM 主内存（574MB）
0x4A400000 – 0x9E200000  : LX_MEM2 第二内存区（1.4GB）
```

---

## 6. mik.ko 注册的 proc 接口

mik.ko 通过 `proc_create()` 注册了 3 个 proc 接口（均为 mode 0600）：

| proc 名称 | 用途 |
|---|---|
| `mi_dev_sys_time` | 设备系统时间 |
| `mi_sys_time` | 系统时间 |
| `mi_sys_cfg_time` | 系统配置时间 |

这些都是时间相关接口，**不是** MI_UTIL 或 utopia。
MI_UTIL 通过 `kobject_init_and_add` + `sysfs_create_bin_file` 注册为 sysfs 节点。

---

## 7. 已确认的其他 proc 接口（闪电脚本收集）

设备 `/proc/` 下存在以下 MStar 相关接口：

| 接口 | 权限 | 说明 |
|---|---|---|
| `/proc/utopia` | `-rw-rw-rw-`（全局读写！） | MStar Utopia 框架接口，读报 I/O error（write-triggered） |
| `/proc/utopia2k` | ? | Utopia2K 接口 |
| `/proc/utopia_mdb` | ? | Utopia MDB 接口 |
| `/proc/Mstar-utopia2k-str` | ? | Utopia2K 字符串接口 |
| `/proc/mstar_dvfs` | ? | MStar DVFS（动态电压频率调节） |
| `/proc/tz2_mstar` | ? | TrustZone 接口 |

### SELinux 授权（HAL 域 `hal_tv_tvfactory_default`）

```
(allow hal_tv_tvfactory_default proc_utopia (dir (ioctl read getattr lock search open)))
(allow hal_tv_tvfactory_default proc_utopia (file (ioctl read write getattr lock append map open)))
(allowx hal_tv_tvfactory_default proc_utopia (ioctl file (0x4d03)))
(allowx hal_tv_tv_factory_default proc_utopia (ioctl file (0x5501 0x5503)))
```

**`/proc/utopia` 是一个未被探索的内核命令执行通道。**
HAL 域对其有完整的 read+write+ioctl 授权，且包含特殊 ioctl 命令字。
读操作返回 I/O error 说明它是**写触发型**接口——需要写入命令字符串触发执行。

---

## 8. 下一步策略（不再使用 rmem/rphy）

### 路线 A：`/proc/utopia` 写触发接口

- 从设备拉取 `/proc/utopia` 的所有者模块（`cat /proc/modules | grep utopia`）
- 反汇编该模块理解写命令格式
- 或直接尝试写入已知命令格式（参照 MI_UTIL 的 `rbank`/`rreg` 等）

### 路线 B：`mstar_miomap_device` ioctl + mmap

HAL 域对 `/dev/miomap`（MStar IO 映射设备）有完整 chr_file 权限（ioctl+rw+map）。
通过 ioctl 可以请求映射特定物理地址段到用户空间，
绕过内核的 PA2KSEG 机制，避免内核态 fault。

需要反汇编 `libmi3.so` 获取 ioctl 命令字和结构体布局。

### 路线 C：SysRq 利用

cmdline 有 `sysrq_always_enabled=1`，但 `misysdiagnose`/`system_app` 域
写 `/proc/sysrq-trigger` 被 SELinux 拒绝。
需要找到一个能写 sysrq-trigger 的域，或者用 MI_UTIL 的 `wreg` 写硬件级 SysRq 寄存器。
