# 05 — 内核环境与加固现状

> **重要修正**：本机内核是 **arm64 / 64 位**（`CONFIG_ARM64=y`, `CONFIG_64BIT=y`），
> 用户态才是 32 位 ARM（armeabi-v7a）。
> ⇒ 内核指针与 `task_struct` 布局都是 **64 位**，写 exploit 时不能按 arm32 处理。

## 基本参数

| 项 | 值 |
|---|---|
| 架构 | arm64（aarch64），64-bit 内核 |
| 页大小 | 4 KB（`ARM64_4K_PAGES`） |
| 虚拟地址位宽 | VA_BITS = **39**（用户态 39 位） |
| 物理地址位宽 | PA_BITS = 48 |
| 用户态 ABI | 32-bit ARM（armeabi-v7a），NDK `armv7a-linux-androideabi21-clang` |
| 内核符号 | `/proc/kallsyms` **可读**（`CONFIG_KALLSYMS=y`） |

## 加固开关一览（取自设备上的 `/proc/config.gz`）

### ❌ 已关闭 —— 对我们有利

| 配置 | 状态 | 意义 |
|---|---|---|
| `CONFIG_RANDOMIZE_BASE` | **not set** | **KASLR 关闭** ⇒ 内核 `.text`/符号地址固定 |
| `CONFIG_SLAB_FREELIST_RANDOM` | **not set** | slab 空闲链表不随机化 ⇒ 堆风水可预测 |
| `CONFIG_SLAB_FREELIST_HARDENED` | **not set** | freelist 指针无校验 ⇒ UAF/溢出更好用 |
| `CONFIG_FORTIFY_SOURCE` | **not set** | 无编译期 memcpy/strcpy 长度检查 |
| `CONFIG_MODULE_SIG` | **not set** | 内核模块无需签名（若拿到 insmod 权限即可加载任意 ko） |
| `CONFIG_INIT_ON_ALLOC_DEFAULT_ON` | not set | 分配内存不自动清零 ⇒ 可读残留数据 |
| `CONFIG_INIT_ON_FREE_DEFAULT_ON` | not set | 释放内存不清零 |
| `CONFIG_DEBUG_LIST` | not set | 链表完整性检查关闭 |
| `CONFIG_BUG_ON_DATA_CORRUPTION` | not set | 数据损坏不 panic |
| `CONFIG_KASAN` / `CONFIG_KCOV` | not set | 无内存/覆盖率检测（且是 release 内核，非 eng） |
| `CONFIG_PANIC_ON_OOPS` | not set | **oops 不会 panic** ⇒ 试错成本低，失败可重试 |
| `CONFIG_DEVMEM` / `CONFIG_PROC_KCORE` | not set | `/dev/mem`、`/proc/kcore` 不存在（这条是坏消息，见下） |
| `CONFIG_SECURITY_SELINUX_DISABLE` | not set | 运行时无法通过 selinuxfs 关 SELinux |

### ✅ 已开启 —— 需要绕过

| 配置 | 状态 | 影响 |
|---|---|---|
| `CONFIG_ARM64_PAN` + `CONFIG_ARM64_SW_TTBR0_PAN` | **y** | 内核态访问用户页会 fault ⇒ 不能靠「传用户指针当内核指针」 |
| `CONFIG_ARM64_UAO` | y | 同上（用户访问覆盖） |
| `CONFIG_VMAP_STACK` | y | 内核栈在 vmalloc 区，栈溢出打不到相邻结构 |
| `CONFIG_STRICT_KERNEL_RWX` | y | 内核代码段只读、不可执行数据 |
| `CONFIG_STACKPROTECTOR_STRONG` | y | 栈 canary |
| `CONFIG_HARDENED_USERCOPY` | y | `copy_to/from_user` 校验目标是否在合法 slab/栈内 |
| `CONFIG_SLAB_MERGE_DEFAULT` | y | 相同大小 slab 缓存合并（对 heap spray 是双刃剑） |
| `CONFIG_SECCOMP` / `SECCOMP_FILTER` | y | 只影响已有沙箱进程 |
| `CONFIG_ARM64_TAGGED_ADDR_ABI` | y | 用户态可用 TBI（对提权基本无关） |
| `CONFIG_SECURITY_SELINUX_DEVELOP` | y | 允许 `selinux=0` 启动参数生效（需要改 cmdline，暂时用不上） |

## 可读的内核信息源（信息泄露面）

| 路径 | 权限 | 价值 |
|---|---|---|
| `/proc/kallsyms` | **可读** | 全符号表 + 地址（KASLR 关闭 ⇒ 直接可用） |
| `/proc/vmallocinfo` | **可读** | 内核 vmalloc 区布局、各模块基址 |
| `/dev/kmsg` | 只读 | 内核日志（含 `__pm_relax` 等 debug 打印） |
| `/proc/devices` | 可读 | 全部字符/块设备 major 号 |
| `/proc/self/maps` | 可读 | 自身映射（可回推 ASLR 与映射策略） |

被拒的：`/proc/iomem`、`/proc/modules`、`/sys/fs/selinux/*`、
`/proc/sys/kernel/*`（除 `perf_event_paranoid`、`unprivileged_bpf_disabled`、`panic_on_oom` 可读）。

## 系统调用普查结论

只有在 `AF_UNIX` 上能建 socket（且**只能 socketpair / abstract bind**，
路径 bind 被拒）。`AF_INET` / `AF_INET6` / `AF_NETLINK` / `AF_PACKET` 全拒。

可用：`memfd_create`、`process_vm_readv`、`mmap(MAP_FIXED)` 到低地址、`socketpair(AF_UNIX)`。

被拒/未实现：`perf_event_open`(拒)、`bpf`(拒)、`userfaultfd`(未实现)、
`kexec_load`(未实现)、`io_uring_setup`(未实现)、`finit_module`(拒)、
`add_key`(拒)、`prctl(PR_SET_MM)`(拒)。

> 完整原始输出见 [`../recon/syscap.txt`](../recon/syscap.txt)、
> [`../recon/dev1.txt`](../recon/dev1.txt)、[`../recon/final1.txt`](../recon/final1.txt)。

## 结论：为什么选 `/dev/mma` 而不是经典内核漏洞

本机 `/dev/mem`、`/proc/kcore` 都不存在，且无 `AF_INET`/`AF_PACKET`
（很多网络栈漏洞的路子直接封死），`userfaultfd`/`io_uring`/`bpf` 也不可用
（堵掉了一大批现成 exploit 的技术路线）。

而 **`/dev/mma` 是唯一被 SELinux 显式放行、且直接操作物理内存的接口**，
加上 **KASLR 关闭 + `/proc/kallsyms` 可读**，使得「拿到一个物理内存读写原语」
就能一步到位。这条路径的信息条件近乎理想，值得优先投入。
