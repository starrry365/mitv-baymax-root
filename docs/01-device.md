# 01 — 目标设备

| 项 | 值 |
|---|---|
| 型号 | 小米电视 ES65 2022 款（65" 4K 面板） |
| 内部代号 | `baymax` |
| 设备名 | `MiTV_MTEQ0` |
| SoC | MStar **MT5872**（ARMv7 / 32-bit ARM，Cortex-A 系列，Mali GPU） |
| 系统 | Android 10（SDK 29） |
| RAM | 1.8 GiB |
| 存储 | eMMC 32 GB（`/data` 25 GB，实际占用 ≈2%） |
| 内核 | 32-bit ARM，KASLR 关闭（详见 `05-environment.md`） |
| root | 出厂无 root（`ro.secure=1`） |

## 接入方式（免开发者选项）

设备出厂已持久化 `persist.adb.tcp.port=5555`，因此**无需打开开发者选项**即可直连：

```bash
adb connect 192.168.1.164:5555
```

两个已知坑：

1. **首次 connect 常报 `failed to authenticate`** —— 再连一次即变 `device`。
2. **重启后 adbd 起得晚**：开机头 ~20 s 内 connect 报「积极拒绝 (10061)」，
   需**循环重试 20~30 s**（约第 4 次成功），不要误判为连不上。

## 渲染与编码器限制（与提权无关，但影响改机）

* 面板 4K，但 **Android 框架层按 1080p 渲染**（`screenrecord` 抓出来必是 1080p）。
* 硬件编码器仅 `OMX.MS.AVC.Encoder`，锁 `Baseline / Level 3.1`
  ⇒ **硬编上限 1280x720@30**。给 1920/1600 会抛 `IllegalStateException`。
* 编码器**卡死**最高频诱因 = 强杀 `screenrecord`/`scrcpy` 进程；
  唯一解法是重启电视。**不要强杀投屏进程。**

## 截图注意

```bash
# ❌ 不要这样（该机 sh 每次往 stdout 打 "Init wrapper sys mutex successful."，
#    会污染成坏 PNG）
adb exec-out screencap -p > f.png

# ✅ 正确
adb shell screencap -p /sdcard/x.png && adb pull /sdcard/x.png
```

## OTA 通道已废弃

固件升级域名 `upgrade.ptmi.gitv.tv` 在阿里 DNS 与 114 DNS 双验证下均返回
**`0.0.0.0` / `::`**（上游主动下架，不是 NXDOMAIN）。电视每次开机都会去问它。
⇒ **该机型不会再收到任何新固件**，不存在「等官方更新修好」这条路。

（判断「服务被下架」的通用技巧：看 `0.0.0.0`（有应答的黑洞）vs `NXDOMAIN`（不存在）。）
