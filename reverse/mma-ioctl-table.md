# MMA 驱动接口逆向（权威版）

> **本文取代早期版本。** 早期版本的表来自反汇编中"立即数构造点"的猜测性还原，
> **有多处错误**（例如把 `mma_map` 写成 `0xc0304d03`，实际 `mma_map` 是 `0x40284d06`，
> 而 `0xc0304d03` 根本不存在）。
> 现在的方法：在设备上找到**真正实现这些函数的那个 .so**，反汇编它，逐条读出
> `movw/movt r1, ...` → `blx ioctl` 的完整命令字。

---

## 0. 关键突破：找到用户态实现

排查过程：

```bash
# 1. 谁引用 mma_alloc_internal？
adb shell 'for f in /vendor/lib/*.so /vendor/lib/hw/*.so /system/lib/*.so; do \
             grep -q -a mma_alloc_internal "$f" && echo HIT $f; done'
#   HIT: /vendor/lib/libOpenCL.so
#   HIT: /vendor/lib/libutopia.so
#   HIT: /vendor/lib/hw/gralloc.mt5872.so        <-- 93 KB，小而全
#   HIT: /vendor/lib/hw/vulkan.mt5872.so

adb pull /vendor/lib/hw/gralloc.mt5872.so
llvm-nm -D gralloc.so | grep mma
```

`gralloc.mt5872.so`（93 KB，**带完整动态符号表**）**导出了整套 MMA 用户态 API**，
而且它的 C++ 修饰名直接给出了参数类型：

```
_Z8mma_openv                       mma_open()
_Z11mma_releasev                   mma_release()
_Z9mma_allocPKcjPyPii              mma_alloc(const char*, u32, u64*, int*, int)
_Z13mma_alloc_secPKcjPyPi          mma_alloc_sec(const char*, u32, u64*, int*)
_Z8mma_freei                       mma_free(int)
_Z8mma_freei  (_Z15mma_free_handlei)  mma_free_handle(int)
_Z7mma_mapihjj                     mma_map(int, u8, u32, u32)
_Z12mma_map_iovaPyi                mma_map_iova(void*, u32, int)
_Z9mma_unmapPvj                    mma_unmap(void*, u32)
_Z9mma_flushPvj                    mma_flush(void*, u32)
_Z14mma_set_cachedih               mma_set_cached(int, u8)
_Z15mma_get_meminfoiP13mma_meminfo_t   mma_get_meminfo(int, mma_meminfo_t*)
_Z16mma_get_heapinfoPKcP14mma_heapinfo_t mma_get_heapinfo(const char*, mma_heapinfo_t*)
_Z14mma_get_pipeidPi               mma_get_pipeid(int*)
_Z20mma_buffer_authorizeii         mma_buffer_authorize(int, int)
_Z22mma_reserve_iova_spacePKcyPyiz mma_reserve_iova_space(const char*, u8, u64*, int, u32, i64)
_Z19mma_free_iova_spacePKc         mma_free_iova_space(const char*)
_Z21mma_export_globalnamei         mma_export_globalname(int)
_Z21mma_import_globalnamei         mma_import_globalname(int)
_Z17mma_import_handleiPi           mma_import_handle(int, int*)
_Z17mma_query_buf_tagPKcPjS1_S1_   mma_query_buf_tag(const char*, u32*, u32*, u32*)
```

## 1. 反汇编要点

它是 **Thumb-2** 代码（`llvm-objdump` 默认按 ARM 解会得到乱码）：

```bash
OBJDUMP=.../llvm-objdump.exe
$OBJDUMP -d --triple=thumbv7-none-linux-gnueabi gralloc.so
```

用 PLT 重定位可以确认 `blx 0x139d0` 就是 `ioctl@LIBC`：

```
$ llvm-readelf -r gralloc.so | grep 15510
00015510  00007b16 R_ARM_JUMP_SLOT  00010575  _Z9mma_allocPKcjPyPii
=> mma_alloc 的 PLT 桩 = 0x13bf0
=> blx 0x139d0 对应的 GOT 槽解析为 ioctl@LIBC
```

于是每一处 `movw r1, #0x4dXX` + `movt r1, #0xC0YY` 都是真实 ioctl 命令字。
自动提取脚本：[`decode_ioctl.py`](decode_ioctl.py)（配套 [`resolve_plt.py`](resolve_plt.py)）。

## 2. ★ 权威 ioctl 命令表

命令码按标准 `_IOC` 编码：`dir(2) | size(14) | type(8=0x4d) | nr(8)`。

| 用户态函数 | ioctl 命令 | 方向 | nr | 结构体大小 |
|---|---|---|---|---|
| `mma_open()` | — | — | — | `open("/dev/mma", O_RDWR)` |
| `mma_get_pipeid(int*)` | `0x80304d02` | R | 0x02 | 48 |
| `mma_alloc(name, size, &pa, &fd, flags)` | **`0xc0384d13`** | RW | 0x13 | **56** |
| `mma_alloc_sec(name, size, &pa, &fd)` | （复用 alloc 路径） | — | — | — |
| `mma_map(fd, u8, u32, u32)` | `0x40284d06` | W | 0x06 | 40 |
| `mma_map_iova(void*, u32, int)` | `0xc0104d14` | RW | 0x14 | 16 |
| `mma_unmap(void*, u32)` | `0x40284d08` | W | 0x08 | 40 |
| `mma_free(int)` | `0x40284d05` / `0xc0284d0e` | W / RW | 0x05 / 0x0e | 40 |
| `mma_flush(void*, u32)` | `0x40284d08` | W | 0x08 | 40 |
| `mma_get_meminfo(fd, info*)` | `0xc0304d0b` | RW | 0x0b | 48 |
| `mma_get_heapinfo(name, info*)` | `0xc0304d0c` | RW | 0x0c | 48 |
| `mma_query_buf_tag(name,p,p,p)` | `0xc01c4d0d` | RW | 0x0d | 28 |
| `mma_buffer_authorize(int,int)` | `0x40304d04` | W | 0x04 | 48 |
| `mma_export_globalname(int)` | `0xc0284d09` | RW | 0x09 | 40 |
| `mma_import_globalname(int)` | `0xc0284d0a` | RW | 0x0a | 40 |
| `mma_import_handle(int,int*)` | `0xc0084d15` | RW | 0x15 | 8 |
| `mma_free_handle(int)` | `0xc0084d16` | RW | 0x16 | 8 |
| `mma_reserve_iova_space(...)` | `0xc0a84d00` | RW | 0x00 | 168 |
| `mma_free_iova_space(const char*)` | `0x40a84d01` | W | 0x01 | 168 |

（`mma_set_cached` 与 `mma_map` 共享 `0x40284d06`，可能只是对该命令的另一种参数写法。）

## 3. ★ 结构体布局（从反汇编逐字段解出）

### `mma_alloc` — 56 字节（0x38）

```
+0x00  char  name[16]        (in)   buffer tag，15 字符 + NUL
+0x10  (16 字节保留，清零)
+0x20  u32   size            (in)   请求大小
+0x24  int   fd              (out)  ← 成功时返回的 dma_buf/mma 句柄
+0x28  u64   pa              (out)  ← 成功时返回的**物理地址**
+0x30  u32   flags           (in)
+0x34  u32   (保留)
```

`flags` 位含义（从 `ion_alloc_fd` 的构造逻辑反推）：

```
bit0 = (usage >> 16) & 1
bit1 = (frame_count == 0x10)      ; 即 r4 == 0x10 时置位
bit2 = usage & 1
```

### `mma_get_meminfo` — 48 字节

```
+0x24  int   fd              (in)
+0x18  12 字节                (out) → mma_meminfo_t[0..11]
+0x28  u8                     (out) → mma_meminfo_t[13]
```
即 `mma_meminfo_t` 大致是 `{ u64 pa; u32 size; ...; u8 cached; }`。
**语义：输入一个 mma fd，输出它的物理地址。**

### `mma_get_pipeid` — 48 字节

实测（只读，安全）：

```
ioctl(fd, 0x80304d02, buf) -> 0
buf+0x00 u32 = 0x11      (可能是 pipe 数或本进程 pipe id)
buf+0x08 u32 = 3
buf+0x10 u64 = 0xffffffc02e1548c1   <== 内核线性映射地址
buf+0x18 u64 = 0x3ea
buf+0x20 u64 = 0xffffffc038cc9400   <== 内核线性映射地址（页对齐）
buf+0x28 u32 = 0x80304d02           <== 回显 ioctl 命令字
```

## 4. ★ 实测结果与当前阻塞点

完整记录见 [`../recon/mma-runtime-probe.txt`](../recon/mma-runtime-probe.txt)。

| 命令 | 结果 |
|---|---|
| `mma_open()` | ✅ 成功（DAC 层 uid=0 可打开 `/dev/mma`） |
| `mma_get_pipeid` | ✅ **成功，并泄露两个内核线性映射地址** |
| `mma_alloc` | ❌ **ENOMEM**（4096 ~ 16 MB 全部失败） |
| `mma_get_meminfo` | ❌ EINVAL（需要有效 fd） |
| `mma_query_buf_tag` / `mma_get_heapinfo` | ❌ EPERM |
| 未知 nr 的命令 | EINVAL（分发表拒绝） |

`mma_alloc` 的 ENOMEM 已排除以下原因（**31 个 tag 名 × 3 个尺寸 × 8 个 flags
= 744 次尝试全部 ENOMEM**）：

* ❌ tag 名不对
* ❌ size 太大
* ❌ flags 不对

剩下的两个根因（都在证据里）：

1. **CMA 几近耗尽**（最强嫌疑）：
   ```
   CmaTotal:  24576 kB     <- 只有 24 MB
   CmaFree:    2108 kB     <- 仅剩 ~2 MB
   ```
   MMA 的物理内存来自 CMA（`/proc/vmallocinfo` 里全是
   `MsOS_MMA_CMA_Unauthorize+... phys=0x... ioremap [utpa2k]` 条目）。
   24 MB 的 CMA 被电视的显示/视频流水线长期占满且高度碎片化，
   任何尺寸的 `mma_alloc` 都拿不到**连续**物理块。
2. 调用进程（由 init 的 `exec` 拉起，域 `u:r:misysdiagnose:s0`）
   可能未被 MMA 驱动登记为合法客户端。

## 5. ★ 意外收获：`/proc/vmallocinfo` 完全可读

受限域里 `/proc/vmallocinfo` **可读**（`/proc/iomem`、`/proc/modules` 则被拒），
它直接暴露了 MMA 的**物理地址布局**：

```
0xFFFFFF800D700000-0xFFFFFF800D711000  69632  MsOS_MMA_CMA_Unauthorize+0x1ac/0xc68 [utpa2k] phys=0x0000000045370000 ioremap
0xFFFFFF800D7C0000-0xFFFFFF800D801000 266240  ... phys=0x0000000045321000 ioremap
0xFFFFFF8012000000-0xFFFFFF8013D12000 30482432 ... phys=0x0000000043210000 ioremap
0xFFFFFF8014000000-0xFFFFFF8015FEC000 33472512 ... phys=0x00000000454b0000 ioremap
...
```

再加上 `MsOS_MMA_map+0x1f4/0x518`、`MsOS_MMA_putfd+0x128/0x500`、
`MsOS_SHM_GetId+0x224/0x28c` 等条目，可以完整重建：

* MMA/CMA 的**物理内存区段**
* 每个内核对象的**虚拟地址**（配合 KASLR 关闭 ⇒ 地址固定）
* 内核函数符号 + 偏移（`+0x1ac/0xc68`）

即：**在完全没有内核代码执行的情况下，我们已经有了一幅相当完整的内核内存地图。**

## 6. 下一步（按可行性排序）

1. **绕开 `alloc`，直接打地址转换/导入类命令**
   —— 这些命令的输入本来就是句柄或物理地址，不依赖新分配：
   * `mma_map` / `mma_map_iova` / `mma_unmap`
   * `mma_import_handle` / `mma_import_globalname` / `mma_globalname_query`
   * `mma_buffer_authorize` / `mma_physical_buffer_authorize`
   * `mma_va2iova`
   需要先拿到一个**其他进程已分配的合法 mma fd/handle**（电视上显示流水线天天在用）。

2. **从已运行的图形进程里取 fd**
   `gralloc`/`surfaceflinger`/`libGLES_mali` 手里一定有大量 mma fd。
   我们已经是 uid 0（DAC 全通），可以
   `/proc/<pid>/fd/` 列目录、甚至 `pidfd_getfd(2)` 拿别人的 fd
   —— 这是**当前最被低估的一条路**（需要确认 SELinux 是否放行
   `/proc/<pid>/fd` 的读取与 ptrace 类操作）。

3. **等 CMA 有空隙时再试 alloc**
   TV 空闲/待机时 CMA 占用会下降，届时 `mma_alloc` 可能直接成功。

4. **路线 B 备用**：CVE-2021-0920（`AF_UNIX` GC UAF）。入口条件已满足
   （`socketpair(AF_UNIX, SOCK_STREAM)` 可用），但 `userfaultfd` 不可用，堆风水更难做。

## 7. 复现命令

```bash
# 反汇编（注意必须指定 thumbv7）
NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe \
    -d --triple=thumbv7-none-linux-gnueabi gralloc.so > gralloc.asm

# 提取 ioctl 命令
python decode_ioctl.py gralloc.asm

# 解析 PLT 桩 → 导入符号
python resolve_plt.py gralloc.so 0x139d0 0x13bf0

# 上机
armv7a-linux-androideabi21-clang -O2 -o mmatest2 mmatest2.c   # 不要 -static
adb push mmatest2 /sdcard/mmatest2
./tv_root_exec.sh "/data/diagnosis/mmatest2"
```

原始反汇编：[`disasm-gralloc-mma.txt`](disasm-gralloc-mma.txt)
