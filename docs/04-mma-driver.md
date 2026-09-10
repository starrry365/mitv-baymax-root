# 04 — MStar MMA 驱动（`/dev/mma`）

> 完整的命令表、结构体布局与实测结果见
> **[`../reverse/mma-ioctl-table.md`](../reverse/mma-ioctl-table.md)**（权威版）。
> 本文只讲背景与攻击面判断。

## 为什么盯上 `/dev/mma`

跑到 uid 0 之后，穷举所有「能碰内核的入口」，只剩一个：

```
(allow misysdiagnose_29_0 mstar_mma_device (chr_file (ioctl read write open)))
```

也就是 `/dev/mma`（major 0、minor 0，SELinux 类型 `mstar_mma_device`）。
它是**当前受限域中被唯一显式放行的内核设备接口**。

> 反面对照：`devenum.c` 枚举了 40+ 个 MStar 私有设备节点
> （`/dev/miomap`、`/dev/malloc`、`/dev/cmapool`、`/dev/tee0`、`/dev/teepriv0`、
> `/dev/cli`、`/dev/cb`、`/dev/mtal`、`/dev/msmailbox`、`/dev/semutex`、
> `/dev/localdimming`、`/dev/gpiochip0` …）**全部被 SELinux 拒**，
> 只有 `/dev/mma`、`/dev/kmsg`(只读)、`/dev/ptmx`、`/dev/zero` 可用。

## `/dev/mma` 是什么

MMA = **M**Star **M**emory **A**gent，MStar TV SoC 的**物理内存分配器**，
为图形/视频流水线提供：物理连续内存分配（DMA 需要）、IOVA 映射管理、
cache flush、跨模块 buffer 共享（global name 导入导出）。

实现模块 = `/vendor/lib/modules/utpa2k.ko`（14.6 MB，`/dev/mma` 字符串在其中；
`lsmod` 显示被 `mdrv_ldm, mik, misck, mwgifker, xcker, hdcp2x, hdcp1x` 共 7 个模块依赖）。

用它的人（`strings` 匹配）：

```
/vendor/lib/egl/libGLES_mali.so      /vendor/lib/hw/hwcomposer.mt5872.so
/vendor/lib/hw/vulkan.mt5872.so      /vendor/lib/hw/gralloc.mt5872.so
/vendor/lib/libOpenCL.so             /vendor/lib/libutopia.so
/vendor/lib/modules/utpa2k.ko        /vendor/bin/cmdl_service
```

## 逆向路线（关键经验）

**不要**直接怼 25 MB 的内核模块猜结构体。正确顺序是：

1. 找**用户态实现** —— `/vendor/lib/hw/gralloc.mt5872.so`（93 KB，未 strip）
   导出了整套 `mma_*`，且 C++ 修饰名直接给出参数类型；
2. 用 `llvm-objdump -d --triple=thumbv7-none-linux-gnueabi` 反汇编（**Thumb-2**！）；
3. 用 `.rel.plt` 把 PLT 桩解析成符号名，确认哪个桩是 `ioctl@LIBC`；
4. 读 `movw/movt r1, #...` 得到真实 ioctl 命令字；
5. 读内核模块的**错误日志字符串**（`error: pMem_info == NULL` 之类）
   反推参数名与校验逻辑。

> ⚠️ 教训：早期版本的表是靠"猜立即数构造点"得来的，**多处错误**
> （把 `mma_map` 写成 `0xc0304d03`，而该值在真实表里根本不存在）。
> 一定要落到"真实实现的反汇编"这一步。

## 攻击假设

一个 DMA 内存分配器，如果：

* 允许把**用户提供的整数**直接当作 handle / 物理地址传入
  `mma_map` / `mma_import_*` / `mma_*_authorize` / `mma_va2iova`；
* 或者 handle 表不做归属校验 / 引用计数检查；

那么从用户态就能让内核把**任意物理页**映射进自己的地址空间 ⇒
**内核物理内存任意读写** ⇒ 改 `cred` / 关 SELinux。

从错误字符串看，这些「按物理地址工作」的命令（`mma_buffer_authorize` 打印
`pa = %llx`、`mma_globalname_query` 打印 `pa = 0x%llx`、`mma_import_globalname`
打印 `pa = %llx`）**只做了 NULL 检查，没看到归属校验的痕迹** —— 值得重点打。

## 实测现状（详见权威版文档）

* `mma_get_pipeid` ✅ 成功，并**泄露两个内核线性映射地址**
* `mma_alloc` ❌ ENOMEM（744 种组合全失败）→ 根因指向 **CMA 只剩 ~2 MB**
* `/proc/vmallocinfo` ✅ **可读**，暴露 MMA/CMA 的物理地址区段与内核符号偏移

⇒ 下一步：**绕开需要新分配的 `alloc`**，改打地址转换 / 句柄导入 / 授权类命令。
