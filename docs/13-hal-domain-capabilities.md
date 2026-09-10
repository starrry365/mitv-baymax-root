# 13 — `hal_tv_tvfactory_default` 域能力画像

**来源**：`vendor_sepolicy.cil` 中 `(allow hal_tv_tvfactory_default …)` 全量摘录
**意义**：这是绕过 SELinux 的**唯一高权限执行域**（由 `system_app` 经 HIDL 间接驱动）

---

## 1. 为什么这个域值得重点研究

三条已到手通道的权限对比：

| 能力 | `misysdiagnose`(uid0) | `system_app`(uid1000) | **`hal_tv_tvfactory_default`** |
|---|---|---|---|
| MStar 私有 chr 设备 | ❌ | ✅（仅 open） | ✅（ioctl/rw/map） |
| `/dev/block/*` 块设备 | ❌ | ❌ | ✅（部分） |
| **`/sys` 任意写** | ❌ | ❌ | ✅ |
| `/dev/kmsg` 写 | ❌ | ❌ | ✅ |
| `/sys/fs/selinux` | ❌ | ❌ | ⚠️ 仅 `dir (write)` |
| **`execmem`** | ❌ | ❌ | ✅ |

---

## 2. 关键授权（节选）

```
# ---- sysfs / procfs ----
(allow hal_tv_tvfactory_default sysfs_29_0 (file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default sysfs_scaler_pwm_curve_x (file (ioctl read write create getattr setattr lock append map unlink rename open)))
(allow hal_tv_tvfactory_default sysfs_scaler_pwm_curve_y (file (... create ... unlink rename open)))
(allow hal_tv_tvfactory_default sysfs_scaler_pwm_max_val (file (... create ...)))
(allow hal_tv_tvfactory_default sysfs_scaler_pwm_min_val (file (... create ...)))
(allow hal_tv_tvfactory_default sysfs_power_str_max_cnt   (file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default sysfs_devices_mstar_mci (dir (read search)))
(allow hal_tv_tvfactory_default sysfs_devices_mstar_mci (file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default proc_utopia (dir  (ioctl read getattr lock search open)))
(allow hal_tv_tvfactory_default proc_utopia (file (ioctl read write getattr lock append map open)))
(allowx hal_tv_tvfactory_default proc_utopia (ioctl file (0x4d03)))
(allowx hal_tv_tvfactory_default proc_utopia (ioctl file (0x5501 0x5503)))
(allow hal_tv_tvfactory_default proc_irq (file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default proc_on_demand_ctl (file (read write open)))
(allow hal_tv_tvfactory_default proc_cmdline_29_0 (file (ioctl read getattr lock map open)))   # 只读
(allow hal_tv_tvfactory_default proc_sys_kernel_printk (file (ioctl read getattr lock map open))) # 只读

# ---- 块设备 ----
(allow hal_tv_tvfactory_default mstar_block_device      (blk_file (ioctl read getattr lock map open)))
(allow hal_tv_tvfactory_default mstar_boot_block_device (blk_file (ioctl read getattr lock map open)))
(allow hal_tv_tvfactory_default mstar_mboot_block_device(blk_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_mpool_block_device(blk_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default block_device_29_0 (dir (search)))

# ---- 字符设备 ----
(allow hal_tv_tvfactory_default mstar_miomap_device  (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_malloc_device  (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_mma_device     (chr_file (read write open)))
(allow hal_tv_tvfactory_default mstar_mik_devices    (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_system_device  (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_scaler_device  (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_semutex_device (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_mailbox_device (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_tee_device     (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_cma_device     (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_cma_device / mstar_fusion_device / mstar_gflip_device …
(allow hal_tv_tvfactory_default kmsg_device_29_0     (chr_file (write open)))
(allow hal_tv_tvfactory_default input_device_29_0    (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default cb_device, rmmgr_device, mtal_device, ion_device_29_0 …

# ---- 文件系统 / 目录 ----
(allow hal_tv_tvfactory_default device_29_0  (dir  (write lock add_name remove_name search open)))
(allow hal_tv_tvfactory_default device_29_0  (file (ioctl read write create getattr setattr lock append map unlink rename open)))
(allow hal_tv_tvfactory_default rootfs_29_0  (dir  (write)))          # 能在 / 下建东西
(allow hal_tv_tvfactory_default configfs_29_0(dir  (write search)))
(allow hal_tv_tvfactory_default debugfs_29_0 (dir  (write)))
(allow hal_tv_tvfactory_default debugfs_tracing_29_0 (dir (write)))
(allow hal_tv_tvfactory_default pstorefs_29_0(dir  (write)))
(allow hal_tv_tvfactory_default sdcard_type  (dir  (ioctl read write getattr lock add_name remove_name search open)))
(allow hal_tv_tvfactory_default sdcardfs_29_0(file (read getattr open)))
(allow hal_tv_tvfactory_default mnt_media_rw_file_29_0 (dir (search)))
(allow hal_tv_tvfactory_default mt_rootfs_file (chr_file (read open)) / (dir (search)))
(allow hal_tv_tvfactory_default mt_tmp (dir (search)) / (sock_file (write)))

# ---- SELinux 控制面 ----
(allow hal_tv_tvfactory_default selinuxfs_29_0 (dir (write)))        # ← 只有 dir write

# ---- 进程 / IPC / 属性 ----
(allow hal_tv_tvfactory_default self (process (execmem)))            # 可执行内存
(allow hal_tv_tvfactory_default fd (use))
(allow hal_tv_tvfactory_default hal_allocator (binder (call)))
(allow hal_tv_tvfactory_default hal_graphics_composer_default (dir (read open)) / (file (read)))
(allow hal_tv_tvfactory_default hidl_memory_hwservice_29_0 (hwservice_manager (find)))
(allow hal_tv_tvfactory_default factory_service (unix_stream_socket (connectto)))
(allow hal_tv_tvfactory_default mt_dtv-svc (fd (use)) / (unix_stream_socket (connectto)))
(allow hal_tv_tvfactory_default ctl_default_prop_29_0 (property_service (set)))
(allow hal_tv_tvfactory_default mstar_system_prop (file (read getattr map open)) / (property_service (set)))
(allow hal_tv_tvfactory_default kernel_29_0 (process (getsched setsched)))
```

---

## 3. 值得注意的几个点

1. **`selinuxfs_29_0` 只有 `dir (write)`，没有 `file (write)`** ⇒ **无法写 `/sys/fs/selinux/enforce`**。
   这正是"卡在 SELinux"的根本原因，也说明**必须走内核内存改写**才能关 SELinux。
2. **`proc_sys_kernel_printk` 只读** ⇒ 不能用 `write_file` 调 `/proc/sys/kernel/printk` 调日志级别。
3. **`self (execmem)` 存在但没有域转换** ⇒ 能分配可执行内存，却没有"以另一个域执行"的入口；
   真正能利用 `execmem` 的前提是**已经能在该域里跑代码**（我们目前只能"驱动它调 HIDL 方法"）。
4. **`mstar_miomap_device` / `mstar_malloc_device` 全权限** ⇒ 物理内存映射/分配接口对本域开放，
   但 HIDL 层未暴露 `mmap`，只有文件类原语，**当前无法直接用**。
5. **`rootfs_29_0 (dir write)`** ⇒ 能在 `/` 下创建文件（rootfs 是 ramfs，可写）。
6. **`change_file_mode` 实测可用**（对 `/dev` 下文件成功，对 sysfs 失败）。

---

## 4. 复现

```bash
grep -E '^\(allow hal_tv_tvfactory_default ' vendor_sepolicy.cil \
  | sed 's/(allow hal_tv_tvfactory_default //; s/)$//' | sort
```
