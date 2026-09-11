# 24 — 免硬件 root 路线最终判定（2026-09-11 实测闭环）

> 目标：在「免拆机 UART、快速、完整 root」约束下，确认还有哪条路是**已实测可走**的。
> 结论先行：**所有"快"的免硬件捷径均已实测证伪；唯一剩余的可继续深挖路径是 CVE-2023-32830 越界（需数周逆向，非当天）**。

---

## 1. 已系统性证伪的路线（勿再试）

| 路线 | 实测结论 | 依据 |
|---|---|---|
| `/dev/miomap` mmap 写 enforcing | **mmap 必打挂设备**（2x kernel_panic） | docs/17 |
| `/dev/miomap` pread 物理读 | **全部返回 EINVAL**，驱动不把文件偏移当物理地址；设备未崩但无通道 | docs/24 §本轮实测 |
| misysdiagnose(uid0) **网络 socket** | **TCP 双向全被 SELinux 拒**（`socket: Permission denied`）→ 无法起网络 root shell；`setsid nc -L /system/bin/sh` 亦失败 | docs/24 本轮实测 |
| `outputUARTSerial` CVE 越界触发 | **openUARTSerial 返回 -1（UART 硬件/上下文未初始化）→ output 全部 ret=-1 提前返回，无法进入原生 memcpy 越界写** | docs/24 本轮实测 (app_process32 已成功触发，但 handle 无效) |
| 改 /system 装 su | 分区 `ext4 ro` + dm-verity，无解 | docs/24 本轮实测 |
| setenforce | 无任何 user 域有 setenforce；enforce 写被 security 钩子拒 | docs/16 |
| 网络 UART/外挂 shell | misconfig 无 socket 权限 | 本轮 |

## 2. 已建立并**固化可用**的攻坚能力（高价值，供 CVE 研究使用）

- **system_app 域任意命令执行**（TvService code 3 + `sysapp.sh`）：`uid=1000 system_app`，可 `app_process32` 反射**任意 TVNative 原生方法**（已验证 load + openUARTSerial/outputUARTSerialSignature 可反射调用）。
- **uid 0 域命令执行**（code 4400 + `tv_root_exec.sh`）：`uid=0 misysdiagnose`，受 SELinux 限（无网络、受限写盘、KASLR 下 kallsyms 全 0）。
- **正确 armv7a 编译诀窍**：`armv7a-linux-androideabi21-clang -O2 -o x x.c`（**不加 `-static/-fPIE/-fPIC`** → 动态 DYN、无 PT_TLS）才可在 Bionic 上 exec；否则 `TLS segment underaligned`。

## 3. 剩下唯一可持续投入的路：CVE-2023-32830（TVAPI OOB write）

- 漏洞窗口**未关**：固件 security_patch 停 `2021-03-05`，CVE 修复在 `2023-10`。
- 触发链路**已打通**（本轮 app_farther 触发成功可直接调 TVNative 原生）。
- 卡点：**未找到能真正喂入原生越界的正确参数形态/未初始化的接口（UART 已返 -1）**。
- 预计：真要在 system_app→内核越界写→root，需对 `libwtk.so`/`libtwoworlds` 中间件 C 代码做**反汇编逐函数 XREF 找 CVE-32830 具体受影响函数簇**，然后构造正确大小的数组 → **数周研究 + 注入**。

## 4. 建议

- **短期**：除非用户接受数周 CVE 研究投入，否则**诚实交付"免硬件快速 root = 目前无捷径"**，避免继续烧轮。
- **中期默认可选**：
  a）砸 CVE-2023-3283（研究向，里程碑=找到某个 native 的越界参数长度并复现 SIGSEGV）；
  b）**接受长期 root，转投 `lem（拆机 UART 串口 ax）**：能拿真实 root shell 且**把当前 uid0 通道能力带进串口内核**（不受 SELinux 网络限制）——但用户明确排除硬件。
- **绝不**：对 `/dev/miomap` 做 mmap、对 `sysfs` sl800 无谓写、扫 26 万 ioctl、给自定义域设 enforce 0。

---
*决定性结论：在当前锁死 boot + Enforcing + 无网络 + 无内核内存写 + miomap dead + UART driver 未 init 的约束下，**免硬件、当天完整 root 不存在**。*