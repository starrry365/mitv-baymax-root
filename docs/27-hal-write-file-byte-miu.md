# 27 — 重启后攻坚重估：HAL 域 write_file_byte 打通 MI_UTIL 内核命令通道 (2026-09-11 下午)

> 设备重启后复查全局通道，取得一项被此前忽略力量的新确认：
> **`hal_tv_tvfactory_default` 域经 HIDL `write_file_byte(path, vec<u8>, mode="w")`
> 能真正写入 `/sys/kernel/mik/MI_UTIL`（MStar 内核寄存器/调试命令节点），无 AV deny。**

---

## 1. 前置：设备已重新在线（清障）

- 之前记忆剩"设备离线，需物理断电重启"。本次实测 `192.168.1.164:5555` 已在线，boot_completed=1。
- 两条硬通道重新校准 **可用**：
  - uid0 `tv_root_exec.sh`：uid=0, ctx `u:r:misysdiagnose:s0`, CapEff 全。
    security_patch=`2021-03-05` → **CVE-2023-32830 窗口仍开**（修复 2023-10）。
  - system_app `sysapp.sh`：uid=1000, ctx `u:r:system_app:s0`。
- HAL 域客户端链路（hidlrun.sh + tvhidl）重启后完整可用（getService=IMtkTvFApiSystem 成功）。

## 2. 本轮实证结论（否定+肯定）

### 否定（不要再试）
- **system_app 域对 `/proc/utopia` ioctl 全被拒**：open 成功(fd=4) 但 `ioctl(0x5501/0x5503/0x4d03/FIONREAD)` 全 EPERM(13)。
  原因：system_app 只有 file ioctl 笼统授权、**无 allowx 具体命令**；只有 hal_tv_tvfactory_default 等域
  才带 `allowx (ioctl file (0x4d03 0x5501 0x5503))`。
- **flash/block 分区直写不可廉价走通**：uid0、system_app 读 `/dev/mik!flash`、`/dev/block/mmcblk0*`、
  `by-name/` 目录 → SELinux Permission denied（非 DAC），需 MStar 专属域或内核。

### 肯定（本轮新确立）
- **HAL 域 write_file 的 4 参重载 `write_file(path,len,content...)` 拿不到 MI 写入**（retbuf=-1），
  必须用 **`write_file_byte(path, hidl_vec<uint8>(content), mode="w")`**（`a_..._sys_write_file_byte`）：
  - 已经过一次用 `write_file`(case1) 拿到 ret[6]=0xffffffff = 命令被截断/失败。
  - 改用 `write_file_byte`(case 8, 变体 B hidl_vec= {buf@0; pad@4; size@8}) + mode="w" → **rv=0, retbuf 全 0**。
  - dmesg 无 AV deny（write 在 HAL 服务进程执行）；只有 tvhidl 自己的 readback 被拒是预期。
- **结论**：HAL 域经此通道可向 MIT_UTIL 写任意调试命令（flag/rbank/rreg/wreg/color/dbg 等），
  即**具备内核寄存器级写能力**（不破坏设备，安全命令集）。

## 3. 为何"能写寄存器"仍不等于 root（红线提醒）

- MI_UTIL 的 `rphy/wphy/rmem/wmem` 四个内存读写命令**一律禁用**（曾致 panic/adbd 失联，见 docs/09/14）。
- 能用的是 **flag/color/rbank/rreg/wreg/dbg** 寄存器级命令 → 可读寄存器/写部分 IO 寄存器，
  但**不能直接改内核变量（如 selinux_state）或获得 uid=0 通用代码执行**。
- 因此本步：**攻击面已从"只能 open"提升到"能触发内核调试命令"**，但**仍非完整 root**。

## 3. 本机可用能力最终地图（重启后实证）

| 域 | 命令执行 | 能写 | 定义 |
|---|---|---|---|
| shell | 有 | /sdcard, 受限 | 普通 shell |
| uid0 misysdiagnose | uid=0,但无网络/受限 /proc | 任意文件(拥堵/不受 LSM 限制) | DAC 全通 |
| system_app | uid=1000 命令执行 | 任意文件写(code2) | 可反射 TVNative,**可开 mitflash open 但 ioctl 未过** |
| **hal_tv_tvfactory_default** | **HIDL 方法调用（客户端驱动）** | **write_file_byte 写任意 sysfs/节点 + 寄存器** | **内核调试原语** |

## 4. 建议下一步（用户决策）

- 路线 A（CVE 继续）：用已打通系统_app 的 JNI 触发链，继续动差分找 OOB（数周逆向，当前唯一"能走到 root"的已知性路径）。
- 路线 B（寄存器级）：研究 libutopia/hal 反汇编里 MIT_UTIL wreg 是否可写**能提权的寄存器**（如 I2C/G bit 改启动参数、或 TZ 接口），低成败率。
- 路线 C（诚实交付）：当前保住"免拆机快速 root = 已实证无便捷路径"，供用户权衡硬件 UART 与否。

*本文是攻坚定档取证：给"为什么 write_file 不行、write_file_byte 行"讲了底；并给出安全红线。*