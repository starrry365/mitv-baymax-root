# MStar `MI_UTIL` 内核调试命令集

**来源**：`mik.ko`（设备厂商内核模块，11.6 MB，ELF64 AArch64，**未 strip，4707 个符号**）
**提取方式**：正则扫描 `.rodata` 中的帮助文本块（文件偏移 `0x3f2800`–`0x3f3a00`）
**节点**：`/sys/kernel/mik/MI_UTIL`（sysfs，**write-only**）

---

## 1. 命令表

内核提示的用法是 `echo <cmd> <params> > /sys/kernel/mik/MI_UTIL`。

| 命令 | 参数 | 语义 | 对应内核函数 |
|---|---|---|---|
| `rbank` | `[bank]` | 打印整个寄存器 bank | `_MI_DEBUG_UTIL_ShowRegBank` |
| `rreg` | `[bank] [offset]` | 读一个寄存器 | `_MI_DEBUG_UTIL_ReadReg` |
| `wreg` | `[bank] [offset] [value]` | 写一个寄存器 | `_MI_DEBUG_UTIL_WriteReg` |
| **`rphy`** | `[phy_addr]` | **读 32 位物理内存** | `_MI_DEBUG_UTIL_ShowMem`（内部做 PA→VA）|
| **`wphy`** | `[phy_addr] [value]` | **写 32 位物理内存** | `_MI_DEBUG_UTIL_WritePhy` |
| **`rmem`** | `[vir_addr]` | **读 32 位虚拟内存** | `_MI_DEBUG_UTIL_ShowBits` |
| **`wmem`** | `[vir_addr] [value]` | **写 32 位虚拟内存**（内部做 VA→PA）| `_MI_DEBUG_UTIL_WriteMem` |
| `color` | `[0/1]` | 开/关彩色提示 | `_MI_DEBUG_UTIL_EnableColorHint` |
| `flag` | `[value]` | 设置调试标志 | `_MI_DEBUG_UTIL_SetDebugFlag` |
| `dbg` | `MI_[XXX] [0/0x20/0x30/0x40/0xF0]` | 设置模块日志级别（0:NONE 0x20:ERR 0x30:WRN 0x40:INFO 0xF0:ALL）| `_MI_DEBUG_UTIL_SetDebugLevel` |
| `gethdmi` | `dmsg` / `edid` | 打印 HDMI 信息 | `_MI_DEBUG_UTIL_GetHdmiInfor` |
| `sethdmi` | `hpd` | 设置 HDMI HPD | `_MI_DEBUG_UTIL_SetHdmiInfor` |

**原始帮助文本节选**（取自 `.rodata`）：

```
6<MI3_DEBUG>echo <cmd> <cmd_param1> <cmd_param2>... > %sMI_UTIL          ( %s = /sys/kernel/mik/ )
6<MI3_DEBUG>rbank [bank]: Show a registers bank, e.g. "echo rbank 0x1015 > .../MI_UTIL"
6<MI3_DEBUG>rreg [bank] [offset]: Read a register ..., e.g. "echo rreg 0x1015 0x69 > ..."
6<MI3_DEBUG>wreg [bank] [offset] [value]: Write a register ..., e.g. "echo wreg 0x1015 0x69 0x1234 > ..."
6<MI3_DEBUG>rphy [phy_addr]: Read a 32bits value from a specified physical memory, e.g. "echo rphy 0x400000"
6<MI3_DEBUG>wphy [phy_addr] [value]: Write a 32bits value to a specified physical memory, e.g. "echo wphy 0x400000 0x5678"
6<MI3_DEBUG>rmem [vir_addr]: Read a 32bits value from a speicifed virtual memory, e.g. "echo rmem 0x678934"
6<MI3_DEBUG>wmem [vir_addr] [value]: Write a 32bits value to a specified virtual memory, e.g. "echo wmem 0x678934 0x3322"
6<MI3_DEBUG>color [0/1]: Enable or disable color hint, ...
6<MI3_DEBUG>flag [value]: Set debug flag, e.g. "echo flag 0x777 > ..."
6<MI3_DEBUG>dbg MI_[XXX] [0/0x20/0x30/0x40/0xF0]: Set debug level(...) for the specified MI_XXX module, ...
6<MI3_DEBUG>get hdmi infor e.g. "echo gethdmi dmsg > ..."   /  "echo gethdmi edid > ..."
6<MI3_DEBUG>set hdmi infor e.g. "echo sethdmi hpd > ..."
```

**输出前缀 `<MI3_DEBUG>` 等价于 `printk(KERN_INFO …)`，即写入内核 ring buffer（`dmesg`）。**

---

## 2. `dbg` 的合法模块名

从 `mik.ko` 提取出 142 个 `MI_*` 候选，其中确认属于调试模块的有：

```
MI_UTIL  MI_DEBUG  MI_VMON  MI_DISP  MI_OSD  MI_IR   MI_PM   MI_OS
MI_SYS   MI_MIU    MI_DMX   MI_AUDIO MI_AOUT MI_ACAP MI_CEC  MI_CIPHER
MI_DSC   MI_INJECT MI_PCM   MI_SYNC  MI_TUNER MI_TSIO MI_SVP  MI_VIDEO
MI_VENC  MI_GPIO   MI_IIC   MI_UART  MI_WDT  MI_FLASH
```

（另一批 `MI_*_BUF` / `MI_*_POOL` 是内存池名，不是调试模块。）

---

## 3. 地址转换语义（对提权很关键）

| 命令 | 输入地址类型 | 内核内部行为 |
|---|---|---|
| `rphy` / `wphy` | **物理地址** | 先做 **PA → VA**（`phys_to_virt` 类），失败时报 `failed to convert PA(0x%llX) to VA!` |
| `rmem` / `wmem` | **虚拟地址** | 先做 **VA → PA**（`virt_to_phys` 类），失败时报 `failed to convert VA(0x%llX) to PA!` |

* 因为 `virt_to_phys()` 只对**线性映射区**（`PAGE_OFFSET` 起，`0xffffffc000000000`）有效，
  `rmem/wmem` 只能用于**内核直接映射的地址**（`.text` / `.data` / `.bss` / 低端内存）——
  这正好覆盖 `selinux_state` 这类静态变量。
* `wphy` 的打印格式为 `ORG/NEW: (PHY: 0x%llX, MEM: 0x%llX) = 0x%x`，
  **如果输出可见，它本身就是一个 PA↔VA 换算器与内存泄露器**。

---

## 4. 当前阻塞

`/sys/kernel/mik/MI_UTIL` 是 **write-only** 节点，而 HAL 的 `write_file` 以需要读权限的模式打开，
返回 `-1`（详见 `docs/11-hal-file-primitives.md`）。因此这些原语**暂时用不上**。
被否决的假设包括：SELinux 拒绝（无 avc）、缺少换行（补 `\n` 无效）、`change_file_mode` 改权限（sysfs 返回 -1）。

---

## 5. 提取脚本

```python
import re
d = open('mik.ko','rb').read()
seg = d[0x3f2800:0x3f3a00]
for m in re.finditer(rb'[ -~]{5,}', seg):
    print(m.group().decode('ascii','ignore'))
```
