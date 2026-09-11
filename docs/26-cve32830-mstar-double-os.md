# 26 — CVE-2023-32830 底层架构实锤：OOB 在 root 的 MStar DTV 双系统 (2026-09-11)

> 本轮进展：**uid0 通道成功拉取 HAL service 主二进制 + libmtal.so + 读取 dtv_svc 进程 maps**，
> 彻底搞清了 CVE-2023-32830 的真实执行一跳栈 —— **漏洞实现不在 Android /vendor 库，
> 而在一个独立的、以 root(uid 0) 运行的 MStar DTV 管理器进程（glibc 双系统）**。

---

## 1. 关键新突破：uid0 通用文件拉取通道打通

之前阻塞是"uid0 通道读 service 二进制时设备掉线"。本轮做出**稳健版** `uid0_pull.sh`：

- **每个 adb 子命令前都 disconnect→reconnect 循环**（最多 10 次×2s）→ 彻底消除 adbd 飘移。
- 用户命令用 `timeout -s KILL 50` 包裹（防 init exec 卡死），`cp 到 /sdcard → chmod → pull`。
- 已成功拉取：**`/vendor/bin/hw/vendor.mediatek.tv.mtktvapi@1.0-service`**（288,268B）、
  **`/vendor/lib/libmtal.so`**（859,084B）。
- 这是**规则级的复用资产**：今后任何 `Permission denied` 的 vendor 文件都能用这个通道取回。

## 2. service 二进制定性：只是 HIDL binder 壳

`svc_bin`（288KB）：
- `et_DYN/ARM32`，**仅有 .dynsym（无 .symtab）** → 剥离。
- 983 个动态符号**几乎全是 UND（导入）**；定义为空 —— 是纯启动器：`main → registerAsService ×N → joinRpcThreadpool`。
- NEEDED 里 import `vendor.mediatek.tv.mtktvapi@1.0.so` + `libmtal.so` + hidl/binder 系列。
- **无任何 TVAPI 业务实现**。

## 2. 库定位修正链（一层一层剥）

| 层 | 文件 | 是什么 | 是否有 OOB |
|---|---|---|---|
| JNI | `/vendor/lib/libvibio.so`(=libjni.so) | TVNative JNI 包装 | 长度校验，非漏洞 |
| HIDL stub | `/vendor/lib/libmtktvapi@1.0.so`(full) | 纯 `BpHw/BnHw/IMtkv` binder 框架，3087 全协议 | 无业务代码 |
| C wrapper | `/vendor/lib/hw/mtktvapi_cwrapper.so` | `a_scan_*_exchange_data` 消费 int[]/byte[] → **dispatch [数处+0x78]→[数处+0x360]** | 反汇编证实是 vtable 分发 |
| HAL daemon | `/vendor/bin/hw/mtktvapi@1.0-service` | 剥离启动器 | 无 |
| **真实现** | **`dtv_svc` (root) 拉 MStar Linux 栈** | 见下 | **← CVE 猎场** |

`a_scan_*_exchange_data`（cwrapper）反汇编证实：
```
r0=对象, r1=hidl_vec<int>数据指针
cmp r1, r4<<2      ; 长度×4 为 0 → 报错
str 对象 [args]
ldr r0,[r1] ; ldr r5,[r0,#0x70或0x78]   ← 运行时回调对象 vtable
blx r5   ; 分发到 MTK 派发
```
即 int[]/byte[] 第二 parameteter 是一个**消息结构体**（首 word 是运行时对象指针），wrapper 走 `对象 vtable[off]` → 进 MStar stack 的 scanTable。

## 3. 决定性重构：MStar "双内核" 架构下 dtv_svc 以 ROOT 跑真 TV 栈

通过 uid0 读 `/proc/$(pgrep dtv_svc)/maps` + cmdline：

```
cmdline = /mnt/vendor/linux_rootfs/basic/dtv_svc --am -input_fifo
PID 3065, uid 0(root)
已加载（MStar 私有 Linux，非 Android bionic）：
  /mnt/vendor/linux_rootfs/basic/dtv_svc        ← 主可执行 0x04645000 r-xp(32MB)
  /mnt/vendor/linux_rootfs/lib/libc-2.21.so     ← glibc 2.21！不是 bionic
  .../libapp_if_rpc.so                          ← Android↔DTV RPC 桥
  .../libmtk_stream.so / libmtkdrm.so / libMtkRmClient.so / libarm_sig_tracer.so
  /mnt/vendor/tvservice/glibc/libapi*.so(DMX/VDEC/GFX/GOP/AUDIO/JPEG)...  DirectFB
  libmi.so / libdrvMMIO/GPIO/MVOP/SYS / libMsFS / libapiXC ...
```
> cmdline 里 `--am`（Android Module）；`-input_fifo`（与 Android 的通信 FIFO）。

**架构结论**：这是经典 **MStar TV "异构双 CPU / 双 OS"** —— Android(ARM 32) 的用户态 SERVE 只做 UI/binder，真实电视功能(搜台 scan/解扰/EPG/播放)在 **同一 SoC 的另一个 Linux 微内核上**的 `dtv_svc`（root) 执行，二者经 **`libapp_if_rpc.so` 的 RPC/FIFO** 通信。

## 4. 对 CVE-2023-32830 攻防含义（关键）

- **`*_ExchangeData` 调用的目标正是在 root 的 `dtv_svc`**：Android TVAPI(HIDL) → `capp_interface` → FIFO/RPC → `dtv_src`(root)。
- **若在该栈的 glibc 部分命中 OOB write ⇒ 直接获得 root(uid0) 的代码执行** —— **不需要过 Android SELinux、不需要 cred toast**！比之前所有路径都干净。
- 之前 10+ 个 `*_ExchangeData` 未崩，说明 scan 路径要么 OOB 在其他方法、要么这些函数本身带防护；**真正的缺陷函数名仍未定**。
- 目标面 = `dtv_svc` 主二进制 + `/mnt/vendor/linux_rootfs/` 下 MStar 库（glibc 2.21、带符号的可能大）。
- **它是 MStar 闭源二进制**（CH 厂商 Mstar），需要跨 PIE 级差逆 32 位 ARM glibc ELF（与 Android 的 THUMB 不同，`dtv_svc` 是 32 位**ARM 指令集**，映射区 `r-xp` 从 0x04645000 起）。

## 5. 诚实的下一步（工程化建议）

CVE 长线，但架构已经精确定位。真正要做的：
1. **拉取整个 dtv_svc 的主二进制**（`/mnt/vendor/linux_rootfs/basic/dtv_svc`，~几十 MB）+ `utils`/`libapp*` 否 —— uid0 通道能读，但大文件要分段拉取（cp→/sdcard→pull 已证可行，改大文件）。
2. **用 RPC/if.`._`_`（fifo）**：因为 Android→DTV 走 FIFO，可能**直接在用户态与 root 的 dtv_svc 对话**调任意 TV 命令 —— 这是"用 finded 大 API 面"的高度 rch L 网络（扫描命令字面是否都能到）。
3. 若 **fifo 无鉴权** → 可从 system_app 域直接发命令给 root dtv_svc（本身就 class 是校长）——先验证这个面是否已能用来 close selinux 或直接 root.

*文档性质：把之前"4 库都拉全就完了"的假设修正为"真存在 root 双 OS 目标"，为后续专注目标锚定。*