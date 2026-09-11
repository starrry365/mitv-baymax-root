# 17 — `/dev/miomap` 的 mmap 会把设备打挂（当日两次复现）

> 与 [`docs/09-operational-hazards.md`](09-operational-hazards.md) 记录的风险**不同**：
> 09 讲的是"某些具体地址危险"，本篇讲的是 —— **mmap 这个动作本身就是致命的**，
> 哪怕映射的是 `/proc/cmdline` 里白纸黑字标注的内存（recovery 帧缓冲）。

## 事故时间线（2026-09-11）

| 时间 | 做的事 | 结果 |
|---|---|---|
| 08:57 | `mio_probe`：`open("/dev/miomap")` + 在候选基址上以 64KB 步长**扫描** ARM64 Image 头（每步只读 0x100 字节） | 运行后 adbd 立刻失联 |
| 09:11 | `ioread`：**只读 proc 文件**，完全不碰 `/dev/miomap`、不做任何 mmap | ✅ 平安无事，跑通两次 |
| 09:16 | 安装 uid0 常驻守护（adbd 若消失就 `setprop ctl.start adbd`） | 装上了，但设备失联后**没能救回来** |
| 09:20 | `mioprobe2`：**只读 11 个物理页**（3 个 cmdline 确认存在的 DRAM 页 + 8 个低地址抽查） | 运行后 adbd 立刻失联 |

两次 mmap 相关的操作，都在几秒内让 5555 关闭；而同样走 system_app 通道、
但不碰设备的 `ioread` 一点事没有。

**失联特征**：ping 通、RTT 2–3ms、ARP 表里有 MAC（系统还活着），但 5555 关闭。
后续确认 `androidboot.bootreason=kernel_panic`。

## 结论

```
/dev/miomap 的 open 可以过，mmap 不能过。
之前"mmap 审计稽查部 = devmem 式任意物理读写"的整套设想到此为止。
```

## 机理推测（两条，均无法在用户态规避）

1. **ARM64 内存属性冲突**：把内核已经用 Normal 属性映射的 DRAM，
   再以 Device / 非缓存属性映射一次 ⇒ stage-1 属性不一致 ⇒ `SError` / 内核 panic。
2. **驱动改写硬件映射**：MStar 这套 MMIO 设备的 `.mmap` 可能会重新配置 MIU / MMU，
   破坏正常的总线与显示输出。

注意 cmdline 里有 `MIU_HIT_PANIC=OFF`，但**并没有**让设备在坏地址访问时幸免。

## 应对：改走 read() / ioctl，并且"分步骤 + 每步落盘"

新的探测程序是 `tools/miostep.c`，设计要点：

* **按危险性递增分级**：`open → lseek+read → pread(物理地址) → 极少量只读 ioctl → mmap(off=0) → mmap(指定 phys)`
* **每一步都先把结果 `fsync` 到 `/sdcard/miostep.log`（双备份）**，再进入下一步
  ⇒ 万一第 N 步把机器打挂，重启后读日志就能精确知道死在哪一步

这里的重点是 **`pread(fd, buf, n, phys)`**：
若这个驱动把文件偏移当物理地址用，它由**内核态**完成拷贝，
天然绕开用户态 mmap 的属性冲突，可能是既安全又省事的物理内存读写通道 —— 尚待验证。

## 附带的硬性操作约束

* 物理内存读写**不要再做 64KB 步长的区域扫描**（第一次事故的诱因之一）
* 每次探测优先打 `/proc/cmdline` 里确认存在的 DRAM 地址
  （`recovery_fbaddr=0x27800000`、`VOC_MEM=0x29600000`、`CMA0 st=0x2A400000`）
* 每次操作前假设 **窗口只有几分钟**（见 [`docs/15-adbd-stability-issue.md`](15-adbd-stability-issue.md)），
  把要做的事压进尽量少的一次触发里
