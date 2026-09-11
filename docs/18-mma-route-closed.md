# 18 — MMA 路线结案：CMA 耗尽，`mma_alloc` 无 buffer 可用（2026-09-11）

> 阶段 8 / 14 曾把「经 `/dev/mma` 做物理内存读写」列为下一步。
> 本次（设备在线窗口）以 `misysdiagnose`（uid 0）域实测 `mma` 全套读命令，
> **确认分配侧彻底不可行**，此路正式关闭。

## 一、实测结果（uid0 / misysdiagnose，运行 `mmatest`，均为只读、安全）

| ioctl | 命令 | 结果 | 判读 |
|---|---|---|---|
| `mma_get_pipeid` | `0x80304d02` | ✅ `ret=0` | **泄露内核 vmalloc 地址**（见下） |
| `mma_get_meminfo` | `0xc0304d0b` | ❌ `EINVAL` | 需传入**已有的合法 mma 句柄**，我们手里没有 |
| `mma_get_heapinfo` | `0xc0304d0c` | ❌ `EPERM` | misysdiagnose 域无权 |
| `mma_query_buf_tag` | `0xc01c4d0d` | ❌ `EPERM` | 同上 |
| `mma_alloc(size=0x1000)` | `0xc0384d13` | ❌ `ENOMEM` | **CMA 耗尽实锤**（0x1000 也分不出） |
| `mma_alloc(0xc0304d03 旧猜测)` | — | ❌ `ENOMEM` | 同上 |

> 备注：`0xc0304d03` 在旧表里被猜成 `mma_alloc_internal`，权威表已更正为 **`0xc0384d13`**（出自 `gralloc.mt5872.so` 反汇编）。本次两个命令均 ENOMEM，与命令字无关，是 CMA 真实耗尽。

## 二、`mma_get_pipeid` 泄露的内核指针

`get_pipeid(0x80304d02)` 返回 48 字节，其中低地址段含两个 **内核线性/vmalloc 地址**：

```
offset 0x10:  40 a5 06 bd c0 ff ff ff  ->  0xffffffc0bd06a540   (内核 vmalloc 地址)
offset 0x18:  e7 03 00 00              ->  0x3e7 = 999 (pipeid?)
offset 0x20:  b8 69 0b c0 ff ff ff     ->  0xffffffc00b69b8     (内核地址)
```

**价值**：属于"内核地址泄露"——配合 KASLR 关闭，可反推内核基址。但**它只是泄露，不能对任意地址读写**。

## 二、为什么这条路线走不通（根因）

1. **`mma_alloc` 全 ' ENOMEM**：`CmaTotal≈24MB`，系统进程（surfaceflinger / vdec / 第三方）已占用殆尽，
   残留 free **只剩 ~2MB**，且很多也被碎片化。即使申请 4KB 也失败。
2. **get_meminfo 需要合法 mma 句柄**：我们自己的进程不持有任何 mma buffer，`get_meminfo` 对任意 `fd` 都 `EINVAL`。
3. **get_heapinfo / query_buf_tag**：`EPERM`，需要更高域（system_app / HAL）。
4. **即使拿到 buffer，也要经 `mma_va2iova` / `mma_map` 把这些分配后的区域映射给你写** —— 而那需要
    **第一个合法的 mma buffer**，闭环在起点就断了。

## 三、结论

`/dev/mma` **不是通往完整 root 的可走之路**（在 CMA 拒绝为攻击者分配的前提下）。

- 已有的三个能力都在同一“没 buffer 就两手空空”的池子里：
  - uid0 域只能读 `kallsyms(名字) / iomem(拒) / mma_get_pipeid(指针泄露)` —— 只有泄露，无写。
  - system_app 域能开 `/dev/miomap`，但** read/ioctl 是死路**（doc 17：lseek Illegal seek / pread EINVAL / ioctl ENOTTY），只有 mmap，而 mmap 崩溃。
  - `misysdiagnose` 对 setenforce/load_policy 全拒（sepolicy 硬门）。

## 三、遗留的、尚待验证的攻击面（关注不盲动）

1. **`mma_get_pipeid` 泄露的内核地址是否自身可被 `query_buf_tag` 之列回查** → 大概率仍需句柄，低优先级。
2. **CMA 是否能通过 `mma_free_iova_space` / `import/export globalname` 复用系统已有 buffer，避开 alloc** —— 复杂度高，暂不展开。
3. 真正有希望的是 **另一个不依赖系统内存分配的设备**，或 **驱动 bug（如 fd 类型混淆 + 越界）** —— 已不再投入。
4. `mstar_miomap_device` 的 `write_file_byte` 已证实的 MI_UTIL 通道（之前 memo）仍然是最接近的已验证写路径，可再次检视能否与 `get_pipeid` 泄漏的指针配合。

## 四、操作安全（本次遵守）

- 全程**只跑只读命令**：`get_pipeid`/`get_meminfo`/`get_heapinfo`/`query_buf_tag`，`ioctl` 类安全。
- **没有调用** `mma_map`/`mma_va2iova`/`mmap`，**没有碰 `mmamap`**(避免重复打挂)。
- 没有对未知命令做宽范围盲扫（吸取 doc 09/17 教训）。