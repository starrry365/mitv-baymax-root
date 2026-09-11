# 26 — CVE-2023-32830 底层架构实锤：OOB 在 root 的 MStar DTV 双系统 (2026-09-11)

> 本轮进展：**uid0 通道成功拉取 HAL service 主二进制 + libmtal.so + 读取 dtv_svc 进程 maps**，
> 彻底搞清了 CVE-2023-32830 的真实执行一跳栈 —— **漏洞实现不在 Android /vendor 库，
> 而在一个独立的、以 root(uid 0) 运行的 MStar DTV 管理器进程（glibc 双体系）**。

---

## 1. 关键新突破：uid0 通用文件拉取通道打通

之前阻塞是"uid0 通道读 service 二进制时设备掉线"。本轮做出**稳健版** `uid0_pull.sh`：

- **每个 adb 子命令前都 disconnect→reconnect 循环**（最多 10 次×2s）→ 消除 adbd 漂移。
- 用户命令用 `timeout -s KILL 50` 包裹（防 init exec 卡死），`cp 到 /sdcard → chmod → pull`。
- 已成功拉取：**`/vendor/bin/hw/vendor.mediatek.tv.mtktvapi@1.0-service`**（288,268B）、
  **`/vendor/lib/libmtal.so`**（859,084B）。
- 这是**规则级复用资产**：今后任何 shell 读不到的 vendor 文件都可用此通道取回。

## 2. service 二进制定性：只是 HIDL binder 壳

`svc_bin`（288KB）：
- `et_DYN/ARM32`，**仅有 .dynsym（无 .symtab）** → 剥离。
- 983 个动态符号**几乎全是 UND（导入）**；定义为空 —— 是纯启动器：`main → registerAsService ×N → joinRpcThreadpool`。
- NEEDED 导入 `vendor.mediatek.tv.mtktvapi@1.0.so` + `libmtal.so` + hidl/binder 系列。
- **无任何 TVAPI 业务实现**。

## 3. 库定位修正链（一层一层剥）

| 层 | 文件 | 是什么 | 是否有 OOB |
|---|---|---|---|
| JNI | `/vendor/lib/libcom_mediatek_twoworlds_tv_jni.so`(=libjni.so) | TVNative JNI 包装 | 长度校验，非漏洞 |
| HIDL stub | `/vendor/lib/vendor.mediatek.tv.mtktvapi@1.0.so`(full) | 纯 `BpHw/BnHw/IMtkTvApiXXXX` binder 框架，3087 全协议 | 无业务代码 |
| C wrapper | `/vendor/lib/hw/mtktvapi_cwrapper.so` | `a_scan_*_exchange_data` 消费 int[]/byte[] → **dispatch [descriptor+0x78/0x360]** | 仅是 vtable 分发 |
| HAL daemon | `/vendor/bin/hw/vendor.mediatek.tv.mtktvapi@1.0-service` | 剥离启动器 | 无 |
| **真实现** | **`dtv_svc` (root) 的 MStar Linux 栈** | 见下 | **← CVE 猎场** |

`a_scan_*_exchange_data`（cwrapper）反汇编证实：
```
r0=对象, r1=hidl_vec<int>数据指针
cbz r1 → 报错
str 对象 [args]...        ; 构建消息结构
ldr r0,[r1]; ldr r5,[r0,#0x70 或 0x78]   ← 运行时回调??对象 vtable
blx r5                        ; 分发到 MStar 栈的 scan 处理
```
即 int[]/byte[] 被解释为一个**消息结构体**（首 word 是运行时对象指针），wrapper 走 `对象 vtable[off]` → 进 MStar 栈。

## 4. 决定性重构：MStar "双内核" 架构下 dtv_svc 以 ROOT 跑真 TV 栈

通过 uid0 读 `/proc/$(pgrep dtv_svc)/maps` + cmdline：

```
cmdline = /mnt/vendor/linux_rootfs/basic/dtv_svc --am -input_fifo
PID 3065, uid 0(root)
已加载（MStar 私有 Linux，非 Android bionic）：
  /mnt/vendor/linux_rootfs/basic/dtv_svc        ← 主可执行 (r-xp 起 0x04645000)
  /mnt/vendor/linux_rootfs/lib/libc-2.21.so     ← glibc 2.21！
  .../libapp_if_rpc.so                       ← Android↔DTV RPC 桥
  .../libmtk_stream.so / libmtkdrm.so / libMtkRmClient.so / libarm_sig_tracer.so
  /mnt/vendor/tmp/fusion.*                     ← directfb 共享面
  /mnt/vendor/tvservice/glibc/libapi(DMX/VDEC/GFX/GOP/AUDIO/JPEG).so
  libmi.so / libdrv(MMIO/GPIO/MVOP/SYS).so / libMsFS.so  DirectFB
```
> cmdline `--am`（Android Module）；`-input_fifo`（与 Android 的通信 FIFO）。

**架构结论**：经典 **MStar TV "异构双 OS"** —— Android(ARM 32) 只做 UI/binder，真实电视功能（搜台 scan/解扰/EPG/解码）在**同一 SoC 的独立 Linux 微内核**上的 `dtv_svc`（root）执行，二者经 `libapp_if_rpc` 的 RPC/FIFO 通信。

## 5. 对 CVE-2023-32830 攻防含义（关键）

- **`*_ExchangeData` 调用的目标正是在 root 的 `dtv_svc`**：Android TV(HIDL) → C wrapper → **DTV 栈 (root, glibc)**。
- **若在该 glibc 栈命中 OOB write ⇒ 直接获得 root(uid0) 代码执行** —— **不需要过 Android SELinux、不需要 cred toast**！比之前所有路径都干净。
- 之前 10+ 个 `*_ExchangeData` 未崩 ⇒ scan 路径要么 OOB 在其他方法、要么带防护；**真正缺陷函数名仍未定**。
- 目标面 = `dtv_svc` 主二进制 + `/mnt/vendor/linux_rootfs/` 下 MStar 库（32 位 ARM glibc ELF）。

## 6. 2026-09-11 实证：FIFO/RPC「直连 root」入口被深度封装（坏消息）

按用户选的"先验 FIFO/RPC 直连 root"方向实测，结论：**该捷径不可立即可用，需再逆向一层协议才能走通**。

实测记录（uid0 通道，dtv_svc PID 3065）：
- `/proc/3065/fd/` → **Permission denied**（misysdiagnose uid0 也读不了，PID 受限 LSM/ptrace）。
- `/proc/3065/net/*` → 无输出（读不到）。
- `find /*fifo*` + `/tmp`、`/mnt/vendor/tmp`、`/data/vendor` → **无命名 FIFO 文件**。
- dtv_svc 父进程 PID 3026 已退出 → 由 bootstrap 拉起的孤儿后台服务，无法借 fd。
- Android 侧无独立 dtv 桥接进程（除懒加载 mtktvfactory HAL）。

判定：
- Android→dtv_svc 的入口（`-input_fifo`、`libapp_if_fp`）**不在可枚举的常规路径**，ft/fd 对内不可见。
- 要**直连** dtv_svc，必须**逆向 `libapp_if_rpc` 私有 RPC 协议 + 定位其 socket/fifo 端点** —— 与"深挖 dtv_svc"同属逆向闭源协议，**不是廉价捷径**。
- **路线判定**：CVE-2023-32830 无论深挖 dtv_svc 还是逆向 RPC，本质都是逆向 MStar 闭源 TV 栈（数月量）；**无已证实的低成本 root 捷径**。
- **保留动作**：拉全 dtv 主程序 + libapp_if_rpc 作**离线攻击面盘点**（只读、可随时做）；仅当命中"无鉴权任意命令入口+ 可达端点"才有机会快速 root（低置信、高回报）。

*文档性质：把之前"4 库都拉平就完了"的假设修正为"真存在 root 双 OS 目标"，为后续专注目标锚定。*