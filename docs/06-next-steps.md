# 06 — 现状总结与下一步

## 已经拿到的

1. **免拆机、纯软件的 uid 0 命令执行通道**（后门链，稳定可用）。
2. **全部 38 个 Linux capability**（`CapEff=0x3fffffffff`）。
3. 一个**常驻 uid 0 daemon**（`rootd`），可任意时刻以 uid 0 跑命令/跑二进制。
4. **权威的 MMA ioctl 协议表**（命令字、结构体布局、函数签名全部从真实实现读出）
   → [`../reverse/mma-ioctl-table.md`](../reverse/mma-ioctl-table.md)。
5. **内核地址泄露**：`mma_get_pipeid` 返回活动内核堆对象地址。
6. **内核内存地图**：`/proc/vmallocinfo` 可读，暴露 MMA/CMA 物理区段、
   内核对象虚拟地址、内核符号 + 偏移。
7. **有利的内核环境**：KASLR 关、`/proc/kallsyms` 可读、slab 加固几乎全关。

## 还差什么

**只有一件事**：SELinux 域仍是受限的 `u:r:misysdiagnose:s0`，
因此无法 `mount`/`insmod`/`setenforce`。

一旦能改自身上下文到 `u:r:init:s0`，或把 `selinux_state->enforcing` 清零，
就是**完整 root**。

## 当前阻塞：`mma_alloc` 返回 ENOMEM

| 已验证排除的原因 | 证据 |
|---|---|
| tag 名不对 | 31 个候选名（`AMM`/`MI_MMA`/`disp`/`vdec`/`osd`/`graphic`…）全失败 |
| size 太大 | 4096 / 65536 / 1 MB / 4 MB / 16 MB 全失败 |
| flags 不对 | flags 0..7 全失败 |
| 结构体布局错 | 已从反汇编逐字段验证，且错误码是 ENOMEM（不是 EINVAL） |

| 剩余根因 | 状态 |
|---|---|
| **CMA 耗尽**（最强嫌疑） | `CmaTotal=24 MB`，`CmaFree≈2 MB`；MMA 从 CMA 分配 |
| 进程未被 MMA 驱动登记为客户端 | 待验证 |

## 下一步路线（按优先级）

### 路线 A — 绕开 `alloc`，直接打「句柄/地址」类命令（**首选**）

`alloc` 是唯一需要**新物理内存**的命令。其余命令的输入本来就是句柄或物理地址：

| 命令 | 输入 | 潜在用途 |
|---|---|---|
| `mma_map(fd, u8, u32, u32)` | fd | 把别人的 buffer 映射进自己的地址空间 |
| `mma_map_iova(void*, u32, int)` | **用户 VA** | 反向：把内核 IOVA 映射到自己进程 |
| `mma_import_handle(int, int*)` | handle | 导入他人句柄 |
| `mma_import_globalname(int)` | handle | 按 global name 导入 |
| `mma_globalname_query(pa)` | **物理地址** | 按物理地址查 global name |
| `mma_buffer_authorize` / `mma_physical_buffer_authorize` | 句柄 / **物理地址** | 授权任意物理地址给硬件 pipe |
| `mma_va2iova(void*, u32, int)` | VA | VA → IOVA |

**目标**：拿到一条「用户态 ↔ 任意物理页」的映射，然后用
`/proc/vmallocinfo` + `/proc/kallsyms`（KASLR 关闭）直接定位并改写
`selinux_state.enforcing` 或自身 `cred->selinux_cred->sid`。

**前置条件**：需要一个**合法且有效的 mma fd / handle**。

### 路线 B — 从已运行的图形进程里"借" fd（**最被低估**）

电视上 `surfaceflinger` / `gralloc` / `libGLES_mali` 天天在用 MMA，
它们手里必然有大量 mma fd。

我们已经是 `uid=0`（DAC 全通），因此可以：

```bash
ls -l /proc/<pid>/fd          # 枚举别人的 fd
cat /proc/<pid>/fdinfo/<n>    # 看 fd 信息
```

甚至用 `pidfd_open(2)` + `pidfd_getfd(2)` **直接把别人的 fd 复制到自己进程里**
（需要 `PTRACE_MODE_ATTACH_REALCREDS` 与 `CAP_SYS_PTRACE` —— 两者我们都有，
但还要确认 SELinux 是否放行 `ptrace` 与 `/proc/<pid>/fd` 的读取）。

拿到真实 mma fd 之后：
* `mma_get_meminfo(fd)` → 直接读出它的**物理地址**；
* `mma_map(fd, ...)` → 映射进自己的地址空间；
* 那这些 buffer 就在我们手里了，再顺着 `/proc/vmallocinfo` 找内核结构。

### 路线 C — 等 CMA 有空隙时再试 `alloc`

电视空闲 / 待机 / 切到主界面时 CMA 占用会下降。
可以在不同场景（播放视频 vs 待机 vs 刚开机）下各测一轮 `mma_alloc`。

### 路线 D — 备用：CVE-2021-0920（`AF_UNIX` GC UAF）

入口条件已满足（`socketpair(AF_UNIX, SOCK_STREAM)` 可用），
但 `userfaultfd` 不可用，堆风水更难做，仅作备选。

### 路线 E — 路由器侧配合

目标电视与一台已 root 的 OpenWrt 路由器（192.168.1.1）同网段。
路由器可作为持久化落脚点、文件中转/编译结果投递、DNS/流量控制。

## 快速复现步骤

```bash
# 0. 连上电视（无需开发者选项）
adb connect 192.168.1.164:5555   # 第一次若报 authenticate 失败，再连一次

# 1. 拿 uid 0 并验证
./tools/tv_root_exec.sh "id; cat /proc/version"
# => uid=0(root) ... context=u:r:misysdiagnose:s0

# 2. 部署常驻 daemon
./tools/tv_root_exec.sh "cp /sdcard/rootd /data/diagnosis/ && chmod 755 /data/diagnosis/rootd && /data/diagnosis/rootd"

# 3. 枚举可用的内核入口
#    编译 probe_open.c / devenum.c / syscap.c 推上去跑

# 4. MMA 协议验证（现代真实 ioctl 号）
#    编译 mmatest2.c 推上去跑；mmatest3.c 用于扫 tag/flags

# 5. 内核内存地图
adb shell 'grep -iE "utpa2k" /proc/vmallocinfo'
adb shell 'grep -E "CmaTotal|CmaFree" /proc/meminfo'
```

编译示例（NDK r27c，**不要加 `-static`**，动态链接可避开
静态 ELF 的 `PT_TLS` 对齐坑）：

```bash
$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/armv7a-linux-androideabi21-clang \
    -O2 -o mmatest2 mmatest2.c
```

反汇编 vendor 库时注意目标代码是 **Thumb-2**：

```bash
llvm-objdump.exe -d --triple=thumbv7-none-linux-gnueabi gralloc.so
```
