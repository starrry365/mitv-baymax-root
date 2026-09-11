# 30 — system_app 执行通道的可靠输出桥（logcat）与 adbd 稳定执行模式（2026-09-11 实测）

> 目标：在「system_app（uid 1000 / `u:r:system_app:s0`）经 TvService code 3 跑原生 ELF」这条既有执行通道上，
> 找到**可靠、跨 SELinux 域、不回崩 adbd** 的输出/结果回收方式，并为后续根利用工具的迭代留下可复用的工程结论。
> 结论先行：**logcat 是被实测验证的唯一可靠且不崩 adbd 的输出桥；property 桥被 SELinux 证伪；"单条 adb shell 命令内触发+睡眠+回读"是已证明稳定的执行模式。**

---

## 1. 背景：为什么需要"输出桥"

TvService code 3（`runSystemCommand`，`service call TvService 3 s16 <cmd>`）用 `execvp` 跑原生 ELF：

- 返回的 `Parcel(00000000 00000001)` **只表示"已投递/执行成功"，不中继子进程 stdout/stderr**。
- 于是我们无法直接在一条 `service call` 后拿到 ELF 的打印结果。
- 需要一个**跨域输出通道**：ELF（system_app 域）产生输出 → 由 shell/其他域（我们可读）回收。

## 2. 候选输出桥实测结论

| 桥 | 原理 | 实测结果 | 判定 |
|---|---|---|---|
| **logcat（liblog）** | ELF 内 `__android_log_print`，shell 用 `logcat -d -s TAG` 读 | `LOGPROBE_START uid=1000 euid=1000` 成功读到 | ✅ **可用（可靠）** |
| **属性（property）** | ELF 内 `__system_property_set("debug.miroot.msg",...)` → shell `getprop` | `getprop debug.miroot.msg` **为空** | ❌ 证伪（system_app 被 SELinux 拒写 `debug.*`） |
| stdout 回收 | 由主进程 exec 并 capture 子进程 stdout | Parcel 不中继 | ❌ |
| 文件回写 | ELF 直接写 `/data/data/com.mediatek.tv.factory/*.txt` | 可写（lp.txt 成功） | ✅（但读取需等 adbd+t+路径，作为辅助） |

> 关键：**logcat 是跨 SELinux 域（system_app 写入 → shell 读取）唯一实测可用的桥**，已用 `logprobe` 证明。

## 2. system_app 直接写文件的细节：GID 必须 1000

- uid0 通道（code 4400，misysdiagnose）`cp` 部署的 ELF，**GID 默认是 0**。
- `installd` 会以 `Ignoring .../miroot2 with unexpected GID 0 instead of 1000` 拒绝。
- **必须在 uid0 部署后 `chown 1000:1000`**，system_app 才能正常 exec。
- 已写入所有部署脚本（`exec_lp.sh` / `miroot_dry_ok.sh` / `miroot_final.sh`）。

## 3. adbd 稳定性：单条 adb shell 命令模式（关键）

反复连接会打疲劳 adbd（`device not found` / connect 10061），每次都重新拉起 host 守护进程。
**实测稳定的模式** = 把"触发 + 睡眠 + 回显"放进**同一条** `adb shell '...'` 调用：

```bash
adb shell 'log -t TAG "trig"; service call TvService 3 s16 ".../prog"; sleep 25; logcat -d -s TAG'
```

- 用 `log -t` 打标记 + 触发 + `sleep` + `logcat` 一次完成，**不重启 adbd**（由 `exec_log.sh` 等实证）。
- 每次触发后 adbd 有可能短暂不可连，但等待循环（60–120 次 × 3–5s）可自动等它自愈；仅在 5555 彻底关闭时才需物理断电。

## 4. 本轮最接近根利用的工具：miroot2（物理内存扫描）

`tools/miroot2.c`：开 `/dev/miomap`，找 ARM64 Image 魔数 `"ARMd"`（@+0x30），
在 `kallsyms 地址全为 0（kptr_restrict=2）` 的前提下，
用 `__ksymtab_strings` 里 `PREL32` 反查得到 `selinux_enforcing` 的 VA，
再由 `_text` 的 VA/PA 差定 delta → VA→PA → 预写性检查后写 enforcing=0（泄 permissive）。

**当前状态（读侧）**：
- 触发 dry-run 能返回 `Parcel 00000001`，但 **MIROOT 的 logcat 为空**（未复现）——可能 exec 失败 / `/dev/miomap` open 失败 / 早退在首条 logcat 之前 / 崩溃。
- 与 `logprobe`（同为 system_app 跑，能出 logcat）对照仍未定位，属**未决问题**。

## 5. 本轮总结 & 后续行动

- ✅ 已固化可复用的**输出桥 = logcat + 单条 adb shell 命令**，并附带 `chown 1000:1000` 修复。
- ✅ 已证伪用不到：property 桥（system_app 无 `debug.*` 写权）。
- ⏸️ **设备侧暂停**：当前电视存在遥控器失灵/显示总线冻结风险（见 docs/09、docs/17、docs/24），在恢复稳定前**不做 flash/大批量 ioctl/mmap**。
- 下一步：待电视稳定后，追查 miroot2 dry-run 无输出的根因（确认能否在 system_app 域打开 `/dev/miomap` 并扫描到 `ARMd`），再决定是否继续写 enforcing 降级。

> 附注：本文件只沉淀"执行通道与输出桥"工程结论，不含可直接运行的提权利用源码。