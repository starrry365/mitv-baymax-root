# 11 — HAL 文件原语的精确语义（反汇编 + 实测）

**日期**：2026-09-10
**方法**：`vendor.mediatek.tv.mtktvfactory@1.0-imp.so` 反汇编（Thumb-2）+ 设备上差分实验
**工具**：`reverse/dis_thumb.py`（capstone 封装）

---

## 1. 为什么需要这份文档

`write_file("/sys/kernel/mik/MI_UTIL", …)` 返回 `-1`，但
`write_file("/sys/kernel/mm/ksm/run", "1")` 返回 `0` 并真的写进去了。
两者都是 **sysfs**、都在 HAL 的 `sysfs_29_0` 只读+写授权范围内、**都没有 avc 拒绝**。
差异只能来自**节点本身的打开方式**。以下是逐函数反汇编得到的结论。

---

## 2. 各原语的真实实现

### 2.1 `check_file` — 就是 `open(path, O_RDONLY)`

`_Z26a_mtktvfapi_sys_check_filePKh` @ `0x2ce9c`：

```asm
02ce9e  cbz  r0, 0x2ced6        ; path == NULL -> 失败
02cea4  blx  0x34f80            ; f(path, 0)          ← 第二参数恒为 0 = O_RDONLY
02cea8  mov  r5, r0             ; fd
02ceac  cmp.w r5, #-1
02ceb6  ble  0x2cef2            ; fd <= -1 -> 失败
02cecc  mov  r0, r5
02cece  blx  0x34f90            ; close(fd)
02ced2  movs r0, #0             ; 返回 0（成功）
02cf06  mov.w r0, #-1           ; 失败路径
```

**⇒ `check_file` 成功 = 「该路径能被 `O_RDONLY` 打开」。**

这给了我们一个**纯读的路径探测器**（`tvhidl` mode 60 用它枚举了 49 条路径）。

### 2.2 `write_file` — 打开模式**需要读权限**（`O_RDWR`）

`_Z26a_mtktvfapi_sys_write_filePKhS0_jS0_` @ `0x2e30d`：

```asm
02e38e  mov  r0, r5
02e390  blx  0x34170            ; strlen(path)
02e394  adds r4, r0, #1
02e398  blx  0x33ff0            ; malloc(len+1)
02e3a6  blx  0x34f00            ; memcpy(heap, path, len+1)
...
02e48c  mov  r0, r5             ; path
02e48e  mov  r1, r7             ; ← 第二个字符串（调用方给的 mode/flag 串）
02e490  blx  0x34c30            ; open/fopen(path, r1)
02e494  cbz  r0, 0x2e4d0        ; 返回 0 -> 失败
02e496  mov  r5, r0
02e498  mov  r0, sb             ; 内容缓冲
02e49a  movs r1, #1
02e49c  mov  r2, sl             ; 长度
02e49e  mov  r3, r5             ; handle
02e4a0  blx  0x34c40            ; write(handle, 1, buf, len)
02e4a4  mov  r4, r0
02e4a8  blx  0x34c50            ; close(handle)
```

**关键点**：`0x34c30` 与 `read_file` 里用的是**同一个 PLT 条目**（见 `0x2e0d6`），
即**读和写共用同一个打开函数**，只是传入的第二个字符串不同（`"r"` / `"w"` 之类）。
若该函数是 `fopen`，`"w"` = `O_WRONLY|O_CREAT|O_TRUNC`；但实测行为否定了这一点（见 §3）。

### 2.3 `copy_file` — 流式复制，**不依赖 `st_size`**

`_Z25a_mtktvfapi_sys_copy_filePKhS0_b` @ `0x2cf2d`：

```asm
02cf30  sub.w sp, sp, #0x480     ; 1152 字节栈帧
02cf3c  mov.w r1, #0x400         ; 1024 字节读缓冲
02cf52  blx   0x34260            ; 初始化缓冲
...
02cfd6  mov  r0, r5
02cfda  blx  0x34fa0             ; 校验/规范化
```

**⇒ 1 KB 栈缓冲 + 循环读写 = 流式。** 因此复制**大分区**（如 64 MB 的 MPOOL）
不会爆内存 —— 这是能安全拉取块设备镜像的前提。

同时对 `/proc/cmdline`（`st_size == 0`）也能完整复制，**说明不依赖 `st_size`**。

### 2.4 `change_file_mode` — 真正的 `chmod`

`_Z31a_mtktvfapi_sys_change_file_modePKhj` @ `0x2e589`（108 B，无循环）。
实测对 `/dev/TVIDL_T3` 设 `0222`、`0644` **均返回 `0`**，对 sysfs 返回 `-1`（sysfs 无 setattr）。

---

## 3. 决定性差分实验

**假设**：`write_file` 若用 `O_WRONLY` 打开，则对 write-only 文件应成功。

**实验设计**（HAL 域执行，全部经 `change_file_mode` 精确控制权限）：

| 步骤 | 调用 | 结果 |
|---|---|---|
| 1 | `create_file("/dev/TVIDL_T3")` | — |
| 2 | `change_file_mode("/dev/TVIDL_T3", 0222)` | **rv=0** ✅ |
| 3 | `write_file("/dev/TVIDL_T3", "ABC")` | **rv=-1** ❌ |
| 4 | `change_file_mode("/dev/TVIDL_T3", 0644)` | **rv=0**（复位权限） |

**结论**：**`write_file` 对 `0222`（write-only）文件失败**。
由于 `0222` 允许 `O_WRONLY`，唯一解释是 **`write_file` 实际以需要读权限的模式打开**，
即 **`O_RDWR`（等价 `fopen(..., "r+")`）**。

**⇒ 对 `0222` 的 write-only sysfs 节点，`write_file` 结构性不可用。**

---

## 4. 直接后果：`MI_UTIL` 为什么写不进去

设备上枚举（`tvhidl` mode 60，49 条路径，6 条可打开）：

```
[47] rv=-1  /sys/kernel/mik/MI_UTIL      ← O_RDONLY 打不开
[48] rv=-1  /sys/kernel/mik/MI_MIU       ← 同上
[49] rv= 0  /sys/kernel/mm/ksm/run       ← 0644，可读写
```

* `MI_UTIL` 用 `O_RDONLY` 打不开 ⇒ 它是 **write-only** 节点（`0200` 类）。
* `write_file` 需要读权限 ⇒ **打不开 ⇒ 返回 -1**。
* `change_file_mode(MI_UTIL, 0666)` 也返回 `-1`（kernfs 不支持 setattr）⇒ 改权限不可行。
* 全程**没有 avc 拒绝** ⇒ 与 SELinux 无关，纯粹是 DAC/节点语义。

**所以 `MI_UTIL`（`rphy/wphy/rmem/wmem` 任意内核内存读写）当前被"打开模式"卡住**，
而不是被 SELinux 或权限卡住。这修正了早先"MI_UTIL 通道已打通、只差地址"的判断。

---

## 5. 可能的绕过方向（按可行性排序）

1. **`write_file_byte(path, hidl_vec<uint8>, string)`**
   —— 它的内部实现可能不是 `a_mtktvfapi_sys_write_file`
   （该符号列表中并无 `write_file_byte` 对应的 `a_mtktvfapi_sys_*`），
   需反汇编 `BpHwMtkTvFApiSystem::write_file_byte` 的**服务端**实现确认其 open 模式。
2. **`write_reg(bank, off, val)`**（无 callback，可直接调用）
   —— 对应 MStar 的 `wreg`，是绕过 sysfs 直接操作寄存器/内存窗口的另一条路。
3. **实现 HIDL callback** → 解锁 `read_file` / `read_reg`
   —— 这同时解决"读整盘 `mmcblk0` 拿内核镜像"和"回读验证"两个问题。
4. 找一个 **HAL 域下以 `O_WRONLY` 打开的** 其他 HAL 方法作为跳板。

---

## 6. 复现命令

```bash
# 权限差分实验
./tools/halrun.sh 4 /dev/TVIDL_T3 x            # create_file
./tools/halrun.sh 6 /dev/TVIDL_T3 x 146        # chmod 0222
./tools/halrun.sh 1 /dev/TVIDL_T3 ABC          # write_file -> retbuf[6] = -1
./tools/halrun.sh 6 /dev/TVIDL_T3 x 420        # chmod 0644

# 路径可打开性枚举
./tools/halrun.sh 60 /data/data/com.mediatek.tv.factory/probe-paths.txt X
```
