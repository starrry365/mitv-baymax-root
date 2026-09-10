# MMA 驱动接口逆向

目标：`/dev/mma` —— MStar TV SoC 的物理内存分配器（MMA = MStar Memory Agent）。
实现模块：`/vendor/lib/modules/utpa2k.ko`（14.6 MB，`/dev/mma` 字符串就在其中，
`lsmod` 显示被 `mdrv_ldm, mik, misck, mwgifker, xcker, hdcp2x, hdcp1x` 共 7 个模块依赖）。

> 注意：`utpa2k.ko` 里的 `MsOS_MMA_*` 是 16 字节间隔的 **`W`（weak）符号桩**，
> 那是给别的内核模块用的导出 API 胶水层；**用户态走的是 `/dev/mma` 的 ioctl**，
> 两条路径不要混淆。

---

## 1. ioctl 命令表（magic = `'M'` = `0x4d`）

命令码由标准 `_IOC` 宏编码：`dir(2) | size(14) | type(8) | nr(8)`。

| ioctl 码 | 方向 | nr | 结构体大小 | 对应处理函数 | 语义 |
|---|---|---|---|---|---|
| `0xc0a84d00` | RW | 0x00 | 168 | `mma_reserve_iova_space` | 预留 IOVA 空间 |
| `0x80044d00` | R | 0x00 | 4 | —（复用 nr 0，4 字节查询） | 查询已预留 IOVA |
| `0x80304d02` | R | 0x02 | 48 | `mma_get_pipeid` | 取 pipeID |
| `0xc0304d03` | RW | 0x03 | 48 | **`mma_alloc`** | 分配物理内存，返回 handle/fd |
| `0x40304d04` | W | 0x04 | 48 | `mma_buffer_authorize` | 以**物理地址**授权 buffer 给 pipe |
| `0x40284d05` | W | 0x05 | 40 | `mma_free` | 释放 |
| `0x40284d06` | W | 0x06 | 40 | `mma_map` | 映射到进程地址空间 |
| `0x40284d08` | W | 0x08 | 40 | `mma_flush` | cache flush |
| `0xc0284d09` | RW | 0x09 | 40 | `mma_export_globalname` | 导出全局名（跨进程共享） |
| `0xc0284d0a` | RW | 0x0a | 40 | `mma_import_globalname` | 按**物理地址**导入全局名 |
| `0xc0304d0b` | RW | 0x0b | 48 | `mma_get_meminfo` | **fd → 物理地址** |
| `0xc0304d0c` | RW | 0x0c | 48 | `mma_get_heapinfo` | 堆信息 |
| `0xc01c4d0d` | RW | 0x0d | 28 | `mma_query_buf_tag` | 查询 buffer tag |
| `0xc0304d17` | RW | 0x17 | 48 | `mma_va2iova` | 虚拟地址 → IOVA |
| `0x40184d0a` | W | 0x0a | 24 | `MsOS_MPool_SetWatchPT` | MPool 页表监视 |
| `0x80954d1f` | R | 0x1f | 149 | `_MHal_XC_GetFlowControlPixelRate` | 读像素时钟（非 MMA，同 magic 段） |

**置信度**：
* 命令码与 size 来自对 `utpa2k.ko` / `libutopia.so` 反汇编中立即数构造点的还原；
* **处理函数名**来自设备端 `strings` 导出的错误日志，是本表最硬的部分（见下）。

---

## 2. 权威证据：29 个 `mma_*` 处理函数（来自模块内错误字符串）

```
$ adb shell strings -n 4 /vendor/lib/modules/utpa2k.ko | grep -oE 'mma_[a-z_0-9]+' | sort -u
```

去掉面板 gamma 曲线名（`mma_0dot4`、`mma_dither` 等），得到 **29 个真实 ioctl 处理函数**：

| # | 函数 | # | 函数 |
|---|---|---|---|
| 1 | `mma_open` | 16 | `mma_free` |
| 2 | `mma_release` | 17 | `mma_free_iova_space` |
| 3 | `mma_alloc` | 18 | `mma_reserve_iova_space` |
| 4 | `mma_alloc_sec` | 19 | `mma_va2iova` |
| 5 | `mma_map` | 20 | `mma_map_iova` |
| 6 | `mma_unmap` | 21 | `mma_get_meminfo` |
| 7 | `mma_flush` | 22 | `mma_get_pipeid` |
| 8 | `mma_table` | 23 | `mma_put_pipeid` |
| 9 | `mma_buf_store` | 24 | `mma_export_globalname` |
| 10 | `mma_buf_remove` | 25 | `mma_import_globalname` |
| 11 | `mma_buf_remove_va` | 26 | `mma_import_handle` |
| 12 | `mma_query_buf_tag` | 27 | `mma_globalname_query` |
| 13 | `mma_buffer_authorize` | 28 | `mma_dmabuf_put` |
| 14 | `mma_buffer_unauthorize` | 29 | `mma_physical_buffer_authorize` |
| 15 | `mma_cma_buffer_authorize` / `mma_cma_buffer_unauthorize` | | `mma_physical_buffer_unauthorize` |

---

## 3. 参数语义：从错误字符串反推

模块里的校验日志**逐字泄露了每个参数的用途**，这比反汇编猜结构体快得多：

```
error: buf_info == NULL          error: pFd == NULL
error: buf_tag == NULL           error: pMem_info == NULL
error: feature == NULL           error: pPhy == NULL
error: pglb_name == NULL         error: pphyBaseAddr == NULL
error: pu8space_tag == NULL      error: u8bufTag == NULL
error: u32pipeID == NULL         error: size == 0
error: u32size <= 0              error: u64size == 0
error: vaddr == 0                error: vaddr == NULL
```

以及每个函数的失败路径都带出了它的关键参数：

```
mma_get_meminfo fail, fd = %d, pa = %llx    <-- 输入 fd，输出 pa
mma_buffer_authorize fail, pa = %llx        <-- 输入就是物理地址
mma_buffer_unauthorize fail, fd = %d
mma_cma_buffer_authorize fail, pa = %llx
mma_import_globalname fail, pa = %llx       <-- 输入物理地址
mma_globalname_query fail, pa = 0x%llx
mma_va2iova fail, va = %llx
mma_unmap fail, va:%llx pid:%x
mma_map failed! va = 0x%p
mma_export_globalname fail, ret = %d
```

### 关键推论

1. **`mma_get_meminfo(输入 fd, 输出 pa)`** —— 这是 `dma_buf` fd → **物理地址** 的转换器。
   我们完全可以先正常 `mma_alloc` 拿到自己的 buffer fd，再用它拿到 pa，
   **从而知道内核物理地址长什么样、以及分配器的地址规律**。

2. **`mma_buffer_authorize(pa, size, pipeid)` /
   `mma_physical_buffer_authorize`** —— 直接接受**用户提供的物理地址**，
   把它当作合法 DMA buffer 授权给某个硬件 pipe。
   这里没有任何「这个 pa 是不是该进程分配的」校验痕迹（错误串里只有 NULL 检查）。

3. **`mma_import_globalname(pa)` / `mma_globalname_query(pa)`** —— 同样以物理地址为输入，
   用于跨进程/跨模块共享 buffer。如果 global name 表没有做归属校验，
   就能"导入"别人的物理内存。

4. **`mma_map(va/size)` 与 `mma_map_iova`** —— 映射原语，
   配合 1、2 即可构造「把任意物理页映射进本进程」的路径。

而且模块里有一句直白的边界检查提示：

```
IOCTL command out of bounds, please check!
```

说明分发器是**按表索引**的（`mma_table` 很可能就是这张表），
这既意味着命令号是连续可枚举的，也可能意味着 `nr` 越界检查存在疏漏。

---

## 4. 攻击链假设（待上机验证）

```
[用户态] mma_alloc(size)                 -> 拿到自己的 buffer fd
         mma_get_meminfo(fd)             -> 学到 pa（验证 fd->pa 转换 + 观察物理地址分布）
         mma_map(fd, ...)                -> 确认映射语义
   --- 以上全部是「合法用法」，先打通语义 ---
[提权]   mma_physical_buffer_authorize(<目标 pa>)  -> 授权任意物理地址
         或 mma_import_globalname(<目标 pa>)       -> 导入任意物理地址
         再配合 map 把它映射进本进程
   --- 若成功 => 物理内存任意读写 ---
[收尾]   写 selinux_state.enforcing = 0，或改自身 cred->selinux_cred->sid
```

## 5. 复现脚本

* [`extract-mma-ioctl.sh`](extract-mma-ioctl.sh) —— 从设备拉 blob 并导出符号/字符串
* 交叉工具：[`../tools/mmatest.c`](../tools/mmatest.c)

## 6. 未完成 / 已知不确定

* **ioctl 与处理函数的 nr 绑定关系尚未逐条坐实**：
  表 1 的 nr 来自反汇编还原，表 2 的函数名来自 strings，
  两者的对应是按语义命名推断的，**需要在设备上用只读命令逐条校准**（`mmatest.c` 的 A 段就是干这个）。
* 结构体字段偏移（尤其 `mma_alloc` 的 in/out 布局、`mma_map` 的 va/size/handle 顺序）**尚未确认**。
* `mma_table` 的内容（真正的 nr→函数指针映射表）未提取 —— 这是下一步最值得做的一件事，
  拿到它就能一次性确认全部编号。
