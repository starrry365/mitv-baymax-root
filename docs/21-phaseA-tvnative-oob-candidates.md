# 21 — CVE-2023-32830 Phase A 离线侦察：TVNative JNI 越界候选面清单

> 时间：2026-09-11（离线，无需设备在线）
> 来源：本地反编译产物 `apk2/TvService_src/sources/`（TvService.apk 已 jadx 反编译）
> 目的：在设备离线期间，先纯静态地把「CVE-2023-32830（TVAPI OOB write）」的真实越界**候选方法簇**锁定，供设备上线后做差分触发验证。
> 结论先行：**已找到一组高优先级的 `int[]/byte[]` 直通 native 的方法**，极可能就是越界载体。

---

## 1. 静态分析结论（关键）

CVE-2023-32830 的越界写在 **Java/接口层看不到**——它藏在 **native（JNI/so）** 实现里。但 **Java 的 native 声明签名**暴露了「哪些方法会把 caller 提供的数组直接送进 native 层」。凡是签名里是 `int[]`/`byte[]`/长度由 caller 控制的方法，在 JNI 层按数组长度 `memcpy`/下标写而**没有边界校验**时，就是越界写。

**本机反编译 `com/mediatek/twoworlds/tv/TVNative.java`（1502 行 native 声明）确认存在这一族高风险签名。**

---

## 2. 高风险候选方法（按可信度分级）

### 🔴 P0：`ExchangeData` 簇（`int[]` caller 传入大小 → native 覆写）
这组 `*ExchangeData_native(int[] iArr)` 是 **"把数据写到 caller 提供的数组"** 模式，若 native 侧按**固定/期望大小**往数组里写、而数组传小了就 OOB OS 时即越界写：
```
getScanATSCExchangeData_native(int[])     (line 123)
ScanCeExchangeData_native(int[])          (125)
ScanDtmbExchangeData_native(int[])        (127)
ScanDvbcExchangeData_native(int[])        (129)
ScanDvbsExchangeData_native(int[])        (133)
ScanDvbtExchangeData_native(int[])        (137)
ScanISDBExchangeData_native(int[])        (139)
ScanPalSecamExchangeData_native(int[])    (141)
hbbtvExchangeData_native(int[])           (877)
htmlAgentExchangeData_native(int[])       (893)
```

### 🔴 P0：`outputUARTSerial_native(byte[])` —— 最典型的 memcpy 载体
```
outputUARTSerial_native(int i, byte[] bArr)   (line 1005)
```
参数 `byte[]` 长度由 caller 决定，native 层如果按 `sizeof(uart_t)` / 固定 buf `memcpy(dst, src, len)` 而无 `len` 校验 → 越界写。这是本清单里**结构化最清晰、最易打**的候选。

### 🟠 P1：`int[交换 + 参数含长度/索引` 簇
```
setInfo_native(byte b, int i, int[] iArr)     (1259)
getInfo_native(byte b, int i, int[] iArr)     (547)
setUARTSerialSetting_native(int i, int[] iArr)(hand/display)
getUARTSerialOperationMode_native(int, int[]) (845)
getUARTSerialSetting_native(int, int[])       (847)
openUARTSerial_native(int i, int[] p1, int[] p2) (1003)
getCIHostID_navtive(int[])                    (389)
SubtitleGetTracks_native(int[])               (157)
```
`libo0` 处 `int i`/`int[]` 都是「调用方控制数量」，JNI 层若把 `i` 当作写入 `iArr` 的下标 base → 越界。

### 🟡 P2：`List<神Base>/Base对象`（对象数组，native 内部解包后按元素写）
```
getApplicationInfoList_native(List<MtkTvGingaAppInfoBase>)   (327)
getAudioAvailableRecord_native(List<TvProviderAudioTrackBase>)(331)
getCecDevListInfo_native(List<MtkTvCecBase.CecDevInfo>)      (415)
getTeletextTopBlockList_native(List<>)                       (803)
setCurrentActiveWindowsInfoEx_native(List<MtkTvChannelInfoBase>,...) (1219)
setCurrentActiveWindowsInfo_native(List<...>, long)          (1221)
```
在 native 中原 Unwrap List → 写入固定数组（每项 Base 对象内部有数组/长度），若某项长度失控即越界。优先级低于 P0/P1（需多对多）。

### 🟡 P5（对照组，通常无害，用于排除）
- `getAllPictureMode_native()` / `getAllScreenMode_native()` / `getAllSoundEffect_native()` 返回 `int[]`（read-return，非注入，一般不谈越界）
- 大量 `getXX_native(int)`（纯 int 参数）——不是数组越界载体。

---

## 3. 关键依据：`onTransact` 与事务码常量（TvService）

`TvServiceDefaultImpl.java` 里可见一批 **非 AIDL 私有事务码**（常量在 96-138 行），部分正是把这些 native 方法桥到上层事件的通道：
```
3000 MITV_SERVICE_TRANSFER_NATIVE_COMMAND   （handleAmCmdWithIntent）
4000 ADD_NATIVE_DROPBOX   4001 SHOW_NATIVE_ALERT   4002 ENGINEER_EVENT_REPORT
4003 FORMAT_DROPBOX_LOG      4004 ENABLE_ADB
4100-4102 BRIEFEVENT_*       4200-4203 POLICY_*
4300 TURNOFF_SCREEN  4301 FAST_TURN_ON_OFF_SCREEN
4400 MITV_SERVICE_MISYSDIAGNOSE  （业务：uid0 — 已知）
5000-5002 PLAYER/REG/OUTPUT  6000-6999 PRODUCT_ACTION  7000-8999 OPEN_SDK_ACTION
10001+ TRANSACTION_MITV_*
```
> 重要的是：**TVNative 是底层 SDK，它不经 TvService 的 AIDL 也能被 TvService 进程内直接调用**。真调用链是
> `上层 App → com.mediatek.twoworlds.tv.MtkTvXxx → TvTVNative.xxx_native() → libtwoworlds/中间件(C/C++) → HAL`。
> 越界写在**最底层 C/C++ 中间件**，Java 只是把 `int[]/byte[]` 原样传给 native 而已。所以**我们在 Java 侧能做的，就是输出精确的「数组参数形状」让 native 层去越界**。

---

## 4. 设实的 Phase A 确认（2026-09-11 11:1x，设备已重连上线）

### 4.1 漏洞窗口核查（强信号：未修复）
```
ro.build.version.release       = 10              (Android 10 ✅)
ro.build.version.security_patch = 2021-03-05      # 补丁日期远早于 CVE-2023-32830 修复(2023-10-02)
ro.build.fingerprint           = xiaomi/baymax/baymax:10/QQ2A.200305.004.A1/2363:user/release-keys
ro.product.model               = MiTV-MTEQ0
```
补丁日期停根在 **2021-03**，而 CVE-2023-32830 的修复 `DTV03802522` 在 **2023-10** 发布。**本机 TVAPI 很可能仍处于漏洞窗口**（虽有"厂商独立固化"可能，但 patch 日强信号指向未修）。

### 4.2 两条提权通道真实复核（全通）
- **uid0 通道**（code 4400 / `tv_root_exec.sh`）：`uid=0(root) gid=0(root) context=u:r:misysdiagnose:s0` ✅
- **system_app 通道**（code 2/3 / `sysapp.sh`，注意写 `/sdcard` 而非 `/data/local/tmp`）：`uid=1000(system) context=u:r:system_app:s0` + Enforcing ✅
- **性能财富**：`runSystemCommand`（code 3）可执行任意 system_app ELF；`writeSystemFile`（code 2）可写 `/sdcard`。Phase B 的大多数越界触发**不一定需要拉 native，可在 system_app 原进程内调用 MtkTvXxx → TVNative.xxx_native()**（见 9.1 候选）。

## 5. 下一步（Phase B，需设备在线）

设备上线后，对 **P0/P1 的候选方法逐一做差分触发**：
1. 用 `tools/sysapp.sh`（system_app 域任意命令）或拉 `com.mediatek.twoworlds` 的 jadx 客户端，构造**渐变长度**的 `int[]`/`byte[]` 调用这些 `*_native`；
2. 观测 `/dev/kmsg`（system_app 可读）+ dmesg：无 `Bad/ fault` 或越界崩溃即命中；
3. 逐层收窄到 native `.so`（`libmtk`/`libtwoworld` 路径），用 `objdump`/`r2` 反汇编定位 `memcpy`/数组写指令与隐患点；
4. 若不脆，构造受控 OOB 写 → 覆盖 `cred`/返回地址。

> ⚠️ 红线：`ExchangeData`/`UARTSerial` 相关互发native在 system_app 域、经 binder 调 HAL，**不是 MStar ioctl、不会打挂显示总线**，但在构建时要避开 `/dev/miomap` 等危险 mmap（docs/09）。

---

## 6. 已落地文件
- 本文档（Phase A 技术侦察 + 实机确认）。
- `apk2/TvService_src/sources/com/mediatek/twoworlds/tv/TVNative.java` —— 待真机调用目标源。
- `apk2/TvService_src/sources/com/mediatek/twoworlds/tv/MtkTvXxx.java` —— 构建客户端调用入口。

---

*本文为静态/离线的「越界候选面锁定」，不含已可利用 exploit；会触发的边界检测在 Phase B。*