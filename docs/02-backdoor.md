# 02 — `misysdiagnose` 后门

## 后门本体

`/init.mitv.rc` 里存在一条 `on property` 触发器：

```
on property:vendor.misysdiagnose.cmd.code=*
    exec - root root -- /vendor/bin/misysdiagnose -${vendor.misysdiagnose.cmd.code} ${vendor.misysdiagnose.cmd.arguments}
```

语义：只要 `vendor.misysdiagnose.cmd.code` 属性被写成任意值，
**init（uid 0）** 就会 fork/exec：

```
/vendor/bin/misysdiagnose -<code> <arguments>
```

`misysdiagnose` 本身是一个 38 KB 的原生诊断可执行文件（已从设备拉取）。
当 `-<code>` 指向一个 shell 脚本路径、`<arguments>` 为空时，等价于
**init 以 uid 0 直接执行任意脚本**。

## 合法写方是谁

属性 `vendor.misysdiagnose.cmd.*` 在 SELinux 策略中被声明为
**仅 `system_app` 域可写**。正常设计里，只有小米自家的系统应用能触发这条诊断通路。

问题出在**写入方没有做调用者鉴权**：

* `TvService` 是一个运行于 `u:r:system_app:s0` 的系统服务（常驻 `system_server` 之外/内）；
* 它暴露的 binder 接口 `transact(4400)` 接收 `(code, arguments)` 两个字符串，
  并**原样** `__system_property_set("vendor.misysdiagnose.cmd.code", code)`
  与 `...cmd.arguments`；
* **没有任何** `checkCallingPermission` / `checkCallingUid` 之类的鉴权。

于是任何本地进程（包括 adb shell）都能满足 SELinux 侧的「写方身份」要求，
因为**真正执行属性写入的是 `system_app` 域里的 `TvService`，而不是调用者**——
这是一条典型的 **confused deputy** 漏洞。

## 触发方式

```bash
service call TvService 4400 s16 "s" s16 "/sdcard/cmd.sh"
```

* 第 4 个参数是 `code` → 变成 `misysdiagnose -/sdcard/cmd.sh`
* 第 5 个参数是 `arguments` → 原样拼接在命令后

> ⚠️ **副作用**：exec 队列是**串行**的。如果上一次执行的脚本 fork 出后台进程
> 且持有 stdout 管道不退出，init 的 `exec` 会**永久阻塞**，后续所有触发全部失灵。
> 现象 = `service call` 仍在返回、但脚本不再执行。
> 解法 = `adb reboot`，并且**永远不要在通过该通道执行的脚本里留后台进程持有管道**
> （要守护进程请双重 fork + `setsid` + 重定向全部标准流，见 `tools/rootd.c`）。

## 为什么这算「后门」而不是「调试接口」

1. 触发器在 init.rc 里，**开机即生效**，无法通过应用层关闭；
2. 触发的执行体是 **uid 0、完整 capability**；
3. 入口只有一层「属主是 system_app」的 SELinux 约束，**没有任何运行时鉴权**；
4. `/vendor/bin/misysdiagnose` 的存在本身表明这是**厂商内部诊断通道**，
   却把执行权暴露给了任意本地调用者。

结论：任何能 `adb connect` 到该电视的人（**包括同局域网内任何主机**，
因为 adb 端口 5555 默认监听在 `0.0.0.0`）都可以瞬间拿到 uid 0。
这是一个**严重的安全问题**，对用户而言也是最好用的「免拆机提权入口」。
