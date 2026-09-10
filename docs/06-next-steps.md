# 06 — 现状总结与下一步

## 已经拿到的

1. **免拆机、纯软件的 uid 0 命令执行通道**（后门链，稳定可用）。
2. **全部 38 个 Linux capability**（`CapEff=0x3fffffffff`）。
3. 一个**常驻 uid 0 daemon**（`rootd`），可任意时刻以 uid 0 跑命令/跑二进制。
4. **完整的 MMA ioctl 协议表**（29 条，见 `../reverse/mma-ioctl-table.md`）。
5. **有利的内核环境**：KASLR 关、`/proc/kallsyms` 可读、slab 加固几乎全关。

## 还差什么

差的**只有一件事**：SELinux 域仍是受限的 `u:r:misysdiagnose:s0`，
因此无法 `mount`/`insmod`/`setenforce`。

一旦能改自身上下文到 `u:r:init:s0` 或把 `selinux_state->enforcing` 清零，
就是**完整 root**。

## 下一步路线（按优先级）

### 路线 A — MMA 物理内存原语（主攻）

1. 上机跑 `tools/mmatest.c`，用只读命令族校准 ioctl 结构体布局；
2. 若布局确认，构造 `mma_alloc_internal` + `mma_map`，
   观察能否把**任意物理地址**映射进用户态；
3. 拿到物理内存读写后：
   * 从 `task_struct->cred` 定位 `struct cred`，改写 `selinux_cred->sid`；
   * 或直接改 `selinux_state.enforcing`；
   * 或覆写 `init_cred` 后用 `setuid(0)` 走正常路径切域。

**风险**：结构体布局若猜错，最坏情况是内核 oops（`PANIC_ON_OOPS` 关闭 ⇒ 大概率只是进程被杀，可重试）。

### 路线 B — CVE-2021-0920（AF_UNIX GC UAF，备用）

已确认 `socketpair(AF_UNIX, SOCK_STREAM)` 可用，该 CVE 的入口条件满足。
但需要控制内核堆布局，而本机 `userfaultfd` 不可用（少了常用的堆风水工具），
实现难度高于路线 A，**仅作备选**。

### 路线 C — uid 0 的 binder 绕过（辅助杠杆）

`uid==0` 让 Android 的 `checkComponentPermission` 直接放行，实测
`service call activity/package/mount 1` 均不返回 SecurityException。
可用来：
* 调 `mount` 服务的相关接口（绕过 `mount(2)` 的 SELinux 限制）；
* 调 `package` 服务做包管理操作；
* 调 `activity` 服务做 `startActivity` / 提权组件启动。

值得**独立深入**——这是一条被低估的捷径。

### 路线 D — 路由器侧配合

目标电视与一台已 root 的 OpenWrt 路由器（192.168.1.1）同网段。
路由器可作为：
* **持久化落脚点**（不占电视资源）；
* **文件中转 / 编译结果投递**；
* **DNS / 流量控制**（已知可用：AdGuard Home rewrites 可做 DNS 黑洞，
  曾用此法解决电视上一个线程风暴问题）。

## 快速复现步骤

```bash
# 0. 连上电视（无需开发者选项）
adb connect 192.168.1.164:5555   # 第一次若报 authenticate 失败，再连一次

# 1. 拿 uid 0 并验证
./tools/tv_root_exec.sh "id; capsh --print 2>/dev/null | head -3; cat /proc/version"

# 2. 部署常驻 daemon
./tools/tv_root_exec.sh "cp /sdcard/rootd /data/diagnosis/ && chmod 755 /data/diagnosis/rootd && /data/diagnosis/rootd"

# 3. 枚举可用的内核入口
#    编译 probe_open.c / devenum.c / syscap.c 推上去跑

# 4. MMA 协议校准（下一步）
#    编译 mmatest.c 推上去跑
```

编译示例（NDK r27c，动态链接避开 PT_TLS 对齐坑）：

```bash
$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/armv7a-linux-androideabi21-clang \
    -O2 -o mmatest mmatest.c        # 不要加 -static
```
