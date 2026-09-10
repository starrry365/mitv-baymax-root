# 04 — MStar MMA 驱动与 ioctl 协议还原

## 为什么盯上 `/dev/mma`

跑到 uid 0 之后，穷举所有「能碰内核的入口」，只剩一个：

```
/selinux: (allow misysdiagnose_29_0 mstar_mma_device (chr_file (ioctl read write open)))
```

也就是 `/dev/mma`（major 0、minor 0，SELinux 类型 `mstar_mma_device`）。
它是**当前受限域中被唯一显式放行的内核设备接口**，因此是突破 SELinux 的
主攻方向。

> 反面对照：`devenum.c` 枚举了 40+ 个 MStar 私有设备节点
> （`/dev/miomap`、`/dev/malloc`、`/dev/cmapool`、`/dev/tee0`、`/dev/teepriv0`、
> `/dev/cli`、`/dev/cb`、`/dev/mtal`、`/dev/msmailbox`、`/dev/semutex`、
> `/dev/localdimming`、`/dev/gpiochip0` …）**全部被 SELinux 拒**，
> 只有 `/dev/mma`、`/dev/kmsg`(只读)、`/dev/ptmx`、`/dev/zero` 可用。

## `/dev/mma` 是什么

MMA = **M**Star **M**emory **A**gent，是 MStar TV SoC 的**物理内存分配器**。
它为图形/视频流水线提供：

* 连续的**物理内存**分配（DMA 需要物理连续）
* **IOVA**（I/O 虚拟地址）映射管理
* cache flush / 一致性维护
* buffer 在多个驱动间共享（global name 导入导出）

使用它的模块（从设备上 `strings` 匹配得到）：

```
/vendor/lib/egl/libGLES_mali.so
/vendor/lib/hw/hwcomposer.mt5872.so
/vendor/lib/hw/vulkan.mt5872.so
/vendor/lib/hw/gralloc.mt5872.so
/vendor/lib/libOpenCL.so
/vendor/lib/libutopia.so
/vendor/lib/modules/utpa2k.ko
/vendor/bin/cmdl_service
```

## 逆向方法

三个输入：

| 文件 | 大小 | 价值 |
|---|---|---|
| `/vendor/lib/modules/utpa2k.ko` | 25 MB | **未剥离符号**，含 `MsOS_MMA_*`、`UtopiaIoctl`、大量 `*Ioctl` |
| `/vendor/lib/libutopia.so` | 19 MB | ARM32 用户态库，含 `MsOS_MMA_*` 封装与 `'M'`-magic ioctl 调用点 |
| `/vendor/bin/cmdl_service` | 28 KB | 小型客户端，ioctl 调用序列清晰 |

**提取流程**：在 `libutopia.so` / `utpa2k.ko` 中定位所有
`mov r0/r1, #0x4d / <nr>` + `mov r2, #<size>` 的 `_IOC` 常量构造点
（以及 `_IOR/_IOW/_IOWR` 宏展开后的立即数），反推出完整命令表。
magic 已确认 = `'M'` = **`0x4d`**。

> ⚠️ 最初的探测（用 `ioctl(fd, 0x0001, ...)`）全部返回 `EACCES` —— 那是因为
> 用了**错误的命令码**：MMA 走的是标准 `_IOC` 编码，不是裸 1/2。
> 完整表见 [`../reverse/mma-ioctl-table.md`](../reverse/mma-ioctl-table.md)（29 条）。

## 攻击假设

一个设计粗糙的 DMA 内存分配器，如果：

* 允许把**用户提供的整数**直接当作 handle / 物理地址传入 `mma_map` / `mma_va2iova`；
* 或者 handle 表没有做范围校验 / 引用计数检查；

那么从用户态就能让内核把**任意物理页**映射进自己的地址空间 ⇒
**内核物理内存任意读写** ⇒

1. 读出内核 `init_cred` / 自身 `task_struct->cred`，把 `selinux_cred` 的
   `sid` 改成 `u:r:init:s0` 或 `kernel`；
2. 或直接改 `selinux_state.enforcing = 0`；
3. 或覆写 `cred->uid/gid/cap_*` + hook `security_ops`。

这比 CVE-2021-0920（AF_UNIX GC UAF）路径更**直接**、更**可控**，
因为不需要控制内核堆布局，只需要一个「物理地址 ↔ 用户映射」的转换原语。

## 待验证程序

[`../tools/mmatest.c`](../tools/mmatest.c) 已写好，策略是**先只读后写**：

* **A 段（只读查询类）**：`get_meminfo` / `get_heapinfo` / `get_pipeid` /
  `query_buf_tag` / dcache flush —— 先确认命令码与结构体大小对不对；
* **B 段（分配类）**：用 `size=4096` 调 `mma_alloc_internal(0xc0304d03)`，
  看能否拿到 handle，再回头用 handle 调 `get_meminfo` 验证 handle 的语义；
* **C 段**：`mma_map` / `mma_free` / `reserve_iova` / `va2iova` **列出但暂不调用**
  （它们会改内核状态，需先确认结构体布局）。

判定标准：A 段能否**稳定返回非 EINVAL 的错误码或成功的结构体回填**。
若 A 段全 `EINVAL`，说明结构体布局猜错了，需要回头从 `utpa2k.ko` 的
`unlocked_ioctl` 分发函数里逐字段反解参数偏移。
