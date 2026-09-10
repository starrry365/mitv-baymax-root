# 09 — 操作风险与事故记录（**动手前必读**）

这份文档记录研究过程中**两次把电视搞挂**的经过与判据。
都是纯软件、可重启恢复，但会中断一整轮工作，值得单独成篇。

---

## 事故 1 — init 死锁（最严重）

### 现象

* `service call TvService 4400 ...` 返回正常，但**脚本永远不执行**。
* 或者脚本执行了一部分，之后**所有** `on property:` 触发器全部失效。
* 杀不掉残留进程：`kill -9 <pid>` → `Operation not permitted`
  （因为 init 是它的父进程，而 init 是不可杀的）。

### 根因

`/init.mitv.rc` 里那条后门用的是 init 的 **`exec`** 内建命令：

```
on property:vendor.misysdiagnose.cmd.code=*
    exec - root root -- /vendor/bin/misysdiagnose -s <脚本>
```

Android init 的 `exec` 是**同步阻塞**的：init 会 **fork 后等待子进程退出**，
在等待期间 **init 主循环停转** —— 于是 **所有属性触发的动作全部失效**。

只要 `misysdiagnose` 或它派生的子进程**不退出**（例如卡在一个读不出来的
`/proc` 文件上、或死循环），init 就永久卡死。

本次实测的罪魁祸首：`uprobe` 读某个 `/proc/utopia*` 文件时**永久阻塞**
（`wchan=0`，State S），父链 `misysdiagnose(18787) ← sh(18794) ← uprobe(18797)`，
PPid 全部是 1（init）。

### 判据

```bash
# 有没有卡住的 misysdiagnose 家族？（PPid=1 且 State S/D）
ps -A -o PID,PPID,STAT,NAME | grep -E 'misysdiagnose|^ *1 '
cat /proc/<pid>/wchan        # 长时间停在同一个值 = 卡住
```

### 恢复

**唯一办法：重启。** `kill -9` 无效。

好消息是 init 对 `sys.powerctl` 有**特殊旁路**，即使主循环卡死也能重启：

```bash
adb shell 'setprop sys.powerctl reboot'      # 或被阻塞时由内核 watchdog 触发
```

### 预防（**最重要的运维规则**）

> **一切经 uid 0 通道执行的命令，必须用硬超时包裹，且脚本里不允许出现
> 「可能永久阻塞」的读操作。**

[`../tools/tv_root_exec.sh`](../tools/tv_root_exec.sh) 已经内置：

```sh
timeout -s KILL 25 sh /sdcard/u_<nonce>.sh
```

即：**不给死锁任何机会**，25 秒必杀。历史版本（无超时）就是踩坑的那一版。

另外两条：

* **每次用不同的脚本文件名（nonce）**。init 的 `on property:` 只在
  **属性值发生变化**时触发；固定文件名会导致第二次调用无效（看似"没反应"）。
* 测有风险的 `/proc` 接口时，**先把超时压到 5 秒**，并优先用
  `open()` + 单次定长 `read()`，不要 `while(!feof)` 式流式读。

---

## 事故 2 — 画面卡在锁屏、回不到桌面

### 现象

用户视角：电视显示锁屏画面，解锁操作无反应 / 解锁后停在黑屏或同一帧，
**回不到桌面**。同一时刻 `adb` 也连不上（`device not found`）。

### 触发时机

发生在**用 `sarun.sh` 在 `system_app` 域跑了自研探测程序 `saprobe` 之后**。

### 最可能的根因（按可能性排序）

1. **MStar 显示/内存总线被打挂**（最可能）
   `saprobe` 会 `open()` 并（在非 safe 模式下）`ioctl`/`mmap`
   `/dev/miomap`（176,0）、`/dev/malloc`（158,0）、`/dev/mma`。
   这些是 MStar SoC 的**物理地址映射 / 内存分配 / 显示管线**接口。
   一个参数不当的 ioctl 足以让**显示控制器或总线仲裁器挂死** →
   SurfaceFlinger 提交的帧永远不被扫描输出 → **画面定格在最后一帧**。
   `adb` 断开也符合"整体 I/O 阻塞"的特征。

2. **内存耗尽导致 Launcher 被反复杀**（次可能）
   该机 RAM 仅 1.8 GB（见硬件巡检报告），且已存在
   `com.xiaomi.mitv.remotecontroller.service` 的线程风暴（700+ 线程、~120% 单核）。
   我们这一轮又跑了十几个探测进程、上百次 binder 调用、`TvService` 反复 fork/exec。
   内存压力下 LMK 把 Launcher 杀掉，解锁时起不来 → 停在锁屏。

3. **`TvService` 被打崩**（已确认发生过）
   爆破事务码时 code ≈ 82 会让 `TvService` 进程直接死掉，
   由 init 自动重启。`TvService` 属于 `android.uid.system`，
   重启期间系统级功能会短暂失效。

### 判据（可从路由器旁路检查，不依赖电视本身）

```bash
ssh router 'ping -c 2 192.168.1.164; nc -z -w 3 192.168.1.164 5555 && echo OPEN || echo CLOSED'
```

* `5555` CLOSED + ICMP 通 ⇒ 内核还在跑，但 `adbd`/框架层挂了 ⇒ 符合上面 1/2/3。
* ICMP 都不通 ⇒ 整机挂死（需断电）。

### 恢复

**重启**（长按电源，或拔电等 10 秒）。恢复后 `persist.adb.tcp.port=5555`
仍在，`adb connect` 会自动恢复。

### 预防

* **不要**在 `system_app` 域对 `/dev/miomap`、`/dev/malloc`、`/dev/system`、
  `/dev/mma`、`/dev/msmailbox`、`/dev/scaler` 做**盲扫 ioctl**。
  必须**先从 vendor 库（`libmi3.so` / `libutopia.so` / `gralloc.mt5872.so` /
  `libGLES_mali.so`）里反汇编出命令字与结构体布局**，再定向调用。
* 探测程序一律**分阶段**：`safe`（只 `open`/`read`）→ `mmap`（只 `mmap`，
  不读内容）→ `ioctl`（定向、单个命令、带超时）。
  `saprobe` 已内置 `safe|mmap|bf` 三档。
* 高风险实验**放在重启前做**，并把恢复步骤（重启）先想好。
* 跑重活前先确认内存：`cat /proc/meminfo | grep -E 'MemFree|SwapFree'`。

---

## 事故总结：三层防护

| 层 | 措施 | 落地位置 |
|---|---|---|
| 执行层 | 硬超时 `timeout -s KILL`，每次 nonce 文件名 | `tools/tv_root_exec.sh` |
| 探测层 | `safe` → `mmap` → 定向 `ioctl` 分档 | `tools/saprobe.c` |
| 认知层 | 先反汇编厂商库拿协议，再上机 | `reverse/` |

## 直接后果：`runSystemCommand` 也会阻塞

`TvService.runSystemCommand` 内部是 `Runtime.exec(cmd)`，**不读子进程输出**。
如果子进程输出超过管道缓冲区（~64 KB），**子进程会阻塞在 write 上永不退出**，
进而可能一直占着 `TvService` 的执行线程。
⇒ 用 `runSystemCommand` 跑东西时，**务必让命令自己把输出重定向到文件**
（`sh /sdcard/x.sh > /sdcard/x.out 2>&1` 这种效果，要先写成脚本），
不要让它往 stdout 狂打。
