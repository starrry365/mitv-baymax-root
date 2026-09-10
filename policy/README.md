# SELinux 策略关键摘录

完整策略来自设备上的编译产物：

```bash
adb shell "cat /vendor/etc/selinux/vendor_sepolicy.cil"    # 652 KB
adb shell "cat /system/etc/selinux/plat_sepolicy.cil"
adb shell "cat /vendor/etc/selinux/vendor_file_contexts"
adb shell "cat /system/etc/selinux/plat_file_contexts"
```

本目录只保留与提权路径直接相关的片段，避免仓库里塞 1 MB 以上的自动生成文件。

---

## 1. 后门通道成立的原因

`misysdiagnose` 域**被允许写** `misysdiagnose` 相关属性；
而真正做属性写入的是 `system_app` 域的 `TvService`：

```
(allow misysdiagnose_29_0 system_server_29_0 (binder (call)))
```

> uid 0 的额外红利：Android 的 `checkComponentPermission()` 对 `uid == 0` 直接放行，
> 因此实测 `service call activity/package/mount 1` **均不返回 SecurityException**，
> 实际可调用的服务接口比上面这一条策略字面看起来多。

## 2. `/dev/mma` 的放行（攻击面来源）

文件上下文：

```
/dev/mma    u:object_r:mstar_mma_device:s0
```

策略：

```
(allow misysdiagnose_29_0 mstar_mma_device (chr_file (ioctl read write open)))
```

这就是**整个受限域里唯一被显式放行的内核设备接口**。

## 3. 被拒绝的设备（对照）

`/dev/miomap`、`/dev/malloc`、`/dev/cmapool`、`/dev/system`、`/dev/mdlactl`、
`/dev/pmae`、`/dev/tee0`、`/dev/teepriv0`、`/dev/log2usb`、`/dev/cli`、
`/dev/cb`、`/dev/cb2`、`/dev/mtal`、`/dev/msmailbox`、`/dev/semutex`、
`/dev/localdimming`、`/dev/mstar_share_resource`、`/dev/gpiochip0`、`/dev/rtc0`、
`/dev/gflip`、`/dev/ir`、`/dev/smart`、`/dev/feeder`、`/dev/mtphoto`、
`/dev/block/mmcblk0*`、`/dev/ttyS0/1` —— **全部 open() = EACCES**。

全部枚举结果见 [`../recon/dev1.txt`](../recon/dev1.txt)。

## 4. 存在但未被利用的类型

```bash
grep -E 'su_exec|\bsu\b' vfc      # /system/xbin/su -> su_exec（文件本身不存在）
```

`su` / `su_exec` 类型在策略里是定义好的，但 `/system/xbin/su` 这个文件不存在 ——
说明系统镜像里**预留了 su 的类型声明但没有放 su 二进制**。
如果哪天能写 `/system`（或把 su 塞进一个已标注 `su_exec` 的位置），
这条类型定义会立刻变得有用。

## 5. 复现实验

```bash
# 从设备取回策略并比对
adb shell "cat /vendor/etc/selinux/vendor_sepolicy.cil" > vendor_sepolicy.cil
grep -n "misysdiagnose" vendor_sepolicy.cil | head -40
```
