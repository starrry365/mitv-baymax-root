# 15 — adbd 不稳定问题分析与闪电窗口策略（2026-09-10）

## 问题概述

电视重启后 adbd 在 5555 端口短暂监听（约 30-60 秒），随后停止监听。
系统本身正常运行（ping 通、ARP REACHABLE、conntrack 活跃、DNS 持续查询），
只是 adbd 进程不再接受新连接。

## 时间线

| 时间 | 事件 |
|---|---|
| 用户重启电视 | 电视开机 |
| uptime ~30s | adbd 开始监听 5555 |
| uptime ~43s | 闪电脚本成功连接 |
| uptime ~45s | 数据收集完成 |
| uptime ~60s | adbd 停止监听（新连接被拒绝） |

## 根因分析

### 不是内核 panic

路由器侧确认：设备重启后 ping/ARP/conntrack 全部正常，
DNS 查询持续（系统服务在运行）。
这说明内核没有崩溃，系统正常运行。

### 最可能的原因：TvService 关联

adbd 网络模式的启动依赖 `persist.adb.tcp.port=5555` 属性。
这个属性由 init 读取，init 启动 adbd 时设置监听端口。

但 adbd 的**持续运行**可能间接依赖 TvService 或其他系统服务。
我们之前对 TvService 进行了大量调用（`service call TvService 4400`），
包括：
- 爆破事务码时杀死过 TvService（code ≈ 82）
- 每次 `tv_root_exec.sh` 都给 TvService 发 binder 事务
- 每次 `hidlrun.sh` 也通过 TvService code=3 执行命令

TvService 如果因内存压力或其他原因重启，
adbd 的网络连接可能被波及。

### 次要原因：内存压力

该机 RAM 仅 1.8GB，且存在 `remotecontroller.service` 线程风暴。
开机后大量服务启动期间内存紧张，LMK 可能杀掉 adbd。

## 解决方案：闪电窗口策略

### `lightning.sh` 脚本

在开机后的短暂窗口内极速抢占 adb 连接，一次性收集所有信息：

```bash
# 脚本位置: tools/lightning.sh
# 用法: ./tools/lightning.sh
# 原理: 每 1 秒尝试连接，连上后立即执行打包好的命令序列
```

### 使用方式

1. 电视端完全断电 → 重新通电
2. 在电脑端运行 `./tools/lightning.sh`
3. 脚本自动等待并抓住窗口
4. 收集的信息保存到 `tools/lightning_out.txt`

### 收集到的关键信息

```
/proc/utopia          → 存在，-rw-rw-rw-，读报 I/O error（写触发型）
/proc/utopia2k        → 存在
/proc/utopia_mdb      → 存在
SELinux enforcing     → 1（确认强制模式）
sysrq                 → 存在但 shell 域无权读
modules               → 40+ 内核模块（含 dtv_driver 2.3MB、mdrv_ldm 594KB）
adbd logcat           → 正常认证流程，无崩溃日志
```

## 后续建议

1. **避免在窗口期内调用 TvService**——只用 adb shell 直接操作
2. **所有信息收集在一次性完成**——不要分多次调用
3. **如果需要 uid0 通道**——准备好脚本，连上后立即执行，不要犹豫
4. **考虑使用 USB adb**——如果有 USB 线连接电视和电脑，USB adb 更稳定
