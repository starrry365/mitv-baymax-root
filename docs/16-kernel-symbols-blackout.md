# 16 — 内核符号黑盒：kallsyms 被封，改从内核镜像里取符号（2026-09-11）

## 结论先行

在这台设备上，**任何域都拿不到内核符号地址**：

| 通道 | shell(uid2000) | misysdiagnose(uid0) | system_app(uid1000) |
|---|---|---|---|
| `/proc/kallsyms` | Permission denied | 可读，**地址列全 0** | 可读，**地址列全 0** |
| `/proc/iomem` | Permission denied | Permission denied | Permission denied |
| `/proc/kcore` | 不存在 | 不存在 | 不存在 |
| `/proc/vmallocinfo` | 可读 | 可读 | Permission denied |

`kptr_restrict` 本身读不出来（三个域都 Permission denied），从表现反推是 **`kptr_restrict=2`**：
连持有 `CAP_SYSLOG`、uid 0 的 `misysdiagnose` 都只能看到 `0000000000000000`。

> ⚠️ **这直接作废了此前所有依赖 kallsyms 地址的方案**，包括 `solve.py` 那条
> 「kallsyms 取 vaddr → 减 PAGE_OFFSET → 加 PHYS_OFFSET 得物理地址」的链路。

## 替代路线：从内核 Image 里反查符号

既然 proc 不给地址，就回到内核镜像本身。ARM64 内核的导出符号表结构：

```c
/* include/linux/export.h —— CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y 时 */
struct kernel_symbol {
        int value_offset;       /* 相对 value_offset 字段自身的地址 */
        int name_offset;        /* 相对 name_offset 字段自身的地址 */
        int namespace_offset;
};
```

它的语义是 **PREL32 相对寻址**：

```
符号真实地址 = (uintptr_t)&entry->value_offset + entry->value_offset
名字字符串   = (char *)&entry->name_offset     + entry->name_offset
```

于是可以**反过来用**：先在 `__ksymtab_strings` 里找到 `"selinux_enforcing\0"` 的位置 `S`，
再在整个 Image 范围内搜索满足下面等式的 4 字节槽位 `p`：

```
(char *)p + *(int32_t *)p == S
```

命中位置的**前一个 word** 就是 `value_offset`，代入上式即得符号地址。
这个方法不需要任何符号表入口变化credentials，只要能读到 Image 内容。

PREL32 还有一个好处：**它是相对量**。
在线性映射（虚拟地址 ↔ 物理地址相差一个常数 delta）下，
无论我们拿着的是虚拟视角还是物理视角的数据，相对关系都成立 —— 可以直接在物理内存里算。

## VA ↔ PA 的自我标定

不需要预先知道 DRAM 基址。找到 Image 头后：

```
头文件 +0x38  = magic "ARMd"            → 得到 Image 物理基址 X
头文件 +0x08  = text_offset             → _text 的物理地址 PA(_text) = X + text_offset
PREL32 反查 "_text"                     → 得到 VA(_text)

DELTA = VA(_text) - PA(_text)
任意符号物理地址 = 符号虚拟地址 - DELTA
```

这套标定是自洽的、零假设的，不依赖 `PAGE_OFFSET` 或 `LX_MEM` 的猜测。

## 目标符号优先级

```
selinux_enforcing   (4.19 之前的老布局)
selinux_state       (较新布局：struct selinux_state { bool enforcing; ... })
selinux_enabled
```

注意核对：4.19 里 enforcement 可能已经是 `selinux_state.enforcing`（`bool`，1 字节）
而不再是一个独立的 `int`。写之前**必须先读出当前值确认为 1**，再决定是否写 0
—— 这条闸门能挡掉绝大多数"地址算错导致乱写"的事故。

## 还剩的下游问题

反查成功只解决了一半：**怎么把值写进物理内存**。这条路今天撞了墙，
见 [`docs/17-miomap-mmap-fatal.md`](17-miomap-mmap-fatal.md)。
