# 08 — 在 `system_app` 域执行任意原生二进制

`system_app` 能跑命令了，但 `runSystemCommand` 只能执行**已存在的可执行文件**
（`Runtime.exec` 按空格分词，不经 shell）。要把自研 ELF 跑起来，需要满足：

1. 文件放在 `system_app` **有 `execute` 权限的 SELinux 类型**下；
2. 该目录 `system_app` 有**写权限**（不然放不进去）。

## 1. `system_app` 的 `execute` 权限只有两条

```cil
(allow system_app_29_0 mi_audio_exec          (file (read getattr map execute execute_no_trans open)))
(allow system_app_29_0 system_app_data_file_29_0 (file (execute execute_no_trans)))
```

`mi_audio_exec` 是厂商音频可执行文件（`/vendor/bin` 下的），不可写。
**`system_app_data_file` 才是突破口。**

## 2. `system_app_data_file` 是什么

`/data(/.*)?` 在 `plat_file_contexts` 里映射到 `system_data_file`，
但 **`system_app_data_file` 不是靠路径后缀来的，而是靠 `seapp_contexts`**：
凡是**平台签名（`android.uid.system` 之类）应用的私有数据目录**，
其内容被打上 `system_app_data_file`。

在设备上枚举这种目录：

```bash
ls -laZ /data/data/ | grep system_app_data_file
# 例：/data/data/com.mediatek.tv.factory   （factory 应用，system_app_data_file）
```

## 3. 关键实测：新建文件自动继承该标签

```bash
D=/data/data/com.mediatek.tv.factory
echo test > $D/probe_lbl && chmod 755 $D/probe_lbl
ls -laZ $D/
# -rwxr-xr-x 1 root root u:object_r:system_app_data_file:s0  probe_lbl   ← 标签自动继承！
cp /system/bin/sh $D/pesh && chmod 755 $D/pesh
ls -laZ $D/pesh
# -rwxr-xr-x 1 root root u:object_r:system_app_data_file:s0  pesh
```

⇒ **把任意 armv7a ELF 放进这个目录，就获得了 `system_app` 域的 `execute` 权限。**

（写文件用两条路都行：uid 0 的 `misysdiagnose` 通道 `cp`，或
`TvService` 的 `writeSystemFile`(code 2) 直接写。）

## 4. 执行器

[`../tools/sarun.sh`](../tools/sarun.sh)：push → 由 uid 0 通道拷进
`/data/data/com.mediatek.tv.factory/` → `chmod 755` → 用
`service call TvService 3` 以 `system_app` 身份执行，输出写到同目录
`probe_out.txt`，再用 uid 0 通道读回来。

```bash
./tools/sarun.sh saprobe safe
```

## 5. ⚠️ 两个必须记住的坑

### 5.1 `system_app` **不能写 `/sdcard`**

`/sdcard` 是 FUSE/sdcardfs 挂载，`system_app` 域写它会被拒。
因此**探测程序的输出绝不能写到 `/sdcard`**，必须写在
`system_app_data_file` 目录里（`probe_out.txt`），
再借 uid 0 通道读回。

早期版本就是因为写到 `/sdcard/sa_probe.txt`，导致
"程序跑完但输出为空"，白白多花了几轮。

### 5.2 `runSystemCommand` 的参数**按空格分词**，且没有 shell

```
service call TvService 3 s16 "/system/bin/sh /sdcard/s.sh"     ✅
service call TvService 3 s16 "id>/sdcard/x.txt"                ❌ 重定向不会被解析
service call TvService 3 s16 "/data/.../saprobe safe"          ✅（用参数传递模式）
```

要重定向/管道，就**先 `writeSystemFile` 写一个 `.sh`，再用 `sh` 执行它**。

## 6. `system_app` 的设备权限清单（实测）

`adb shell` 直接访问这些设备全部 `Permission denied`，
但在 `system_app` 域**全部可开**：

| 节点 | 主次设备号 | DAC | MStar 用途 |
|---|---|---|---|
| `/dev/miomap` | 176,0 | `crw-rw-rw-` media:system | **物理内存映射（重点）** |
| `/dev/malloc` | 158,0 | `crw-rw-rw-` media:system | 物理内存分配 |
| `/dev/system` | — | — | 系统总线寄存器 |
| `/dev/msmailbox` | 175,0 | `crw-rw-rw-` system:graphics | 与子核（DVB/音频）通信 |
| `/dev/scaler` | 147,0 | `crw-rw-rw-` media:system | 缩放器 |
| `/dev/semutex` | 181,0 | `crw-rw-rw-` media:system | 硬件信号量 |
| `/dev/ashmem` | 10,62 | `crw-rw-rw-` | — |
| `/dev/mtal` / `mtal_media` / `mtal_system` | 241,0 | `crw-rw----` | MTAL 音频 |
| `/dev/cimodule_command0` | 180,192 | `crw-r-----` | CI 条件接收模块 |
| `/proc/utopia` | — | `-rw-rw-rw-` | MStar 命令接口（读会 `I/O error`） |

对应的 SELinux 授权（`vendor_sepolicy.cil` / `plat_sepolicy.cil`）：

```cil
(allow system_app_29_0 mstar_miomap_device   (chr_file (ioctl map open read write)))
(allow system_app_29_0 mstar_malloc_device   (chr_file (ioctl map open read write)))
(allow system_app_29_0 mstar_system_device   (chr_file (...)))
(allow system_app_29_0 mstar_mailbox_device  (chr_file (...)))
(allow system_app_29_0 mstar_scaler_device   (chr_file (...)))
(allow system_app_29_0 mstar_semutex_device  (chr_file (...)))
(allow system_app_29_0 kmem2_device          (chr_file (ioctl map open read write)))   # /dev/kmem2 本机不存在
(allow system_app_29_0 proc_utopia           (file  (ioctl map open read write)))      # /proc/utopia
(allow system_app_29_0 proc_cmdline_29_0     (file  (read write open)))
(allow system_app_29_0 sysfs_devices_mstar_mci (file (read write open)))
(allow system_app_29_0 mstar_eeprom_block_device (blk_file (read write open)))          # ⚠️ EEPROM 块设备
```

完整清单见 [`../policy/system-app-privileges.txt`](../policy/system-app-privileges.txt)。

## 7. `debugfs` / `tracefs` 也已 rw 挂载

```
debugfs on /sys/kernel/debug type debugfs (rw,...)
tracefs on /sys/kernel/debug/tracing type tracefs (rw,...)
```

`shell` 域被显式授权了**唯一一条 tracefs 写权限**：

```cil
(allow shell_29_0 debugfs_tracing_29_0 (file (write)))
```

理论上可以往 `kprobe_events` / `kprobe_profile` 写 ——
但注意**没有 `read` 权限**，且 `kprobe_events` 在 4.19 上能否创建成功
尚待实测确认（见 `recon/` 内的原始输出）。

## 8. 待办

* [ ] 在 `system_app` 域跑 `saprobe`，确认 `/dev/miomap` / `/dev/malloc` 的
      **ioctl 协议**（先从 vendor 库 `libmi3.so` / `libutopia.so` / `gralloc.mt5872.so` 反汇编）。
* [ ] 用 `system_app` 的 `/proc/cmdline` **写**权限：`androidboot.selinux=permissive`
      只对**下次启动**有效，且 Android 的 `selinux=permissive` 是否被
      `ro.boot.` 链路覆盖需验证。
* [ ] tracefs `kprobe_events` 实测。
