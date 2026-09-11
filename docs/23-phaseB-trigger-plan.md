# 23 — Phase B 真机动态触发：可行路径与实现决策

> 时间：2026-09-11 12:00（设备在线，adb 已授权）
> 前置：docs/22 已从真机 libjni.so 确认 P0/P1 越界候选函数全部存在并拿到反汇编证据
> 本文档：给出「如何在真机上真正触发这些 native」的 3 条候选路径评估 + 最终决策 + 已有可复用资产

---

## 1. 目标回顾

能"打"任何一个越界面（Java 层的 `protected static native int[]/byte[]` 方法 → JNI C），在**独立可控进程**里触发，用 `logcat -b crash` + `/data/tombstones` 观测越界 SIGSEGV/abort。这是 CVE-2023-32830 逆向链的第一步实质落地。

## 2. 设备侧事实（已实测）

| 项 | 值 | 含义 |
|---|---|---|
| `ro.product.cpu.abilist` | `armeabi-v7a,armeabi` | 32 位 ARM thumb |
| `/system/bin/app_process32` | exists（zygote32） | 可跑 Java main |
| `ro.debuggable` | 0 | release，不可 ptrace/system× debugger |
| 系统 framework | `mitvmiddlewareimpl.jar`(仅 manifest)、`MitvAPIImpl.jar` | TVNative **不在** framework jar，而在 **TvService.apk** |
| TvService.apk 本地反编译 | `apk2/TvService_src/` | 含 `TVNativeWrapper`（public static 透传）+ `MtkTvFactoryService*` |
| lib 本体 | `/vendor/lib/libcom_mediatek_twoworlds_tv_jni.so` | 已 pull 到 host `native/` |

## 3. 三条路径对比（依可行性/风险）

### 路径 A：Binder 直调（★★☆☆☆）—— 被否决
板子：`MtkTvFactoryService`/`MVTvFactoryServiceBase` 是 TvService **进程内对象**，不是独立 binder service；`service call` 无法直发到这些 Java 方法。除 1/2/3/4400 外无对应透传 transaction。**不可行**。

### 路径 B：`app_process32 + 自定义 dex + 反射`（★☆☆☆☆ 已选定为主线）
method（系统对 app 自有类 protected 反射 + setAccessible 不受限）：
```
runSystemCommand("app_process32 /  -Djava.library.path=/vendor/lib \
  -Djava.class.path=<TvService_classes+trigger 的 dex> com.trigger.Main")
```
- **classpath 关键**：必须让 app_process 的 classloader 能 findClass `com.mediatek.twoworlds.tv.TVNativeWrapper`。方案＝把 trigger 类与 TVS 的 classes.dex 合并/拼接为一个 jar/dex，或用 `-Djava.class.path=/sdcard/tvaclass.jar`。运行时经 `runSystemCommand`（system_app 域）→ **dlopen libsthl 不撞 SELinux**（同源域）。
- **文件放置**：`/data/local/tmp` 在 app 域不可写 → 触发器 jar/dex 放 **/sdcard/**（app 可读）+ app_process 从 classpath 读。
- 触发调用点：直接用 `TVNativeWrapper.outputUARTSerial_native(handle, byte[])`（**public static 直达**，无需反射 protected；只有 UART 族走 wrapper；ExchangeData 需反射 TVNative。）

### 路径 C：HIDL/HAL 原生（★★☆☆☆ 备选）
`tvhidl.c`（已有 HAL 客户端）走 `vendor.mediatek.tv.mtktvf@1.0-service`（pid 2736），但 TVNative 越界面在 **mtktvapi@1.0**（非 factory），HIDL 接口未 host 该越界点。仅当 path B 被类加载/SELinux 卡死时回退。

---

## 4. 已确定执行步骤（Phase B 落地）

### Step 0 —— 最小可行性验证（必须先做，1 次 adb 调用）
经 `runSystemCommand` 跑 **最简** app_process32：只输出 `uid=` + `Class.getMethod 反射一个系统类`（如 `Integer`）。验证「app_process32 能从 system_app 域、正确读 classpath、等于可 fibrick 反射」三件事。**若此处失败 → 立即停，改路径。**

### Step 1 —— 触发 jar 构建（host 端）
- NDK 或 host JDK 把 `TVNativeWrapperTrigger.class`（我写的）`+` TvService.classes.dex（已 pull）合并成 `trigger.jar`（`dex`）。
- class 里：`Class.forName("com.mediatek.twoworlds.tv.TVNativeWrapper")` → `getMethod("outputUARTSerial_native",int.class,byte[].class)` → 用**渐变长度** bytes（如 1,4,16,64,256,4096...)依次调用，每次分隔 `Thread.sleep(200)`，异常 `catch(Throwable)` 打印 → 观测。
- 越界观测：`logcat -b crash -d` 之后连续弹出 tombstone / SIGSEGV。

### Step 2 —— 真机差分触发（红线内）
- 用 `sysapp.sh "app_process32 ..."` 分发 → 每变一档长度、每次 pull crash log 对比。
- 若 `outputUARTSerial` 因 unmapped 空 crash → 换 ExchangeData 族（`ScanDtmb/Des` 60? arm 一步见清单）或 `openUARTSerial(handle, [...])` 系列。
- 记录 hit 曲线：**第一次出现越界/崩溃的数组长度** = 越界窗口。

### Step 3 —— 若 B 受阻：HIDL / UART 拆机 回退
见 Phase E。

---

## 5. host 侧资产
- `native/dump_syms.py` —— ELF 符号枚举（pyelftools）
- `native/disasm_func.py` —— capstone Thumb 函数反汇编
- `native/libjni.so`、`native/libmtktvapi.so`、`native/mitvmiddlewareimpl.jar` —— 已 pull 原始素材
- `apk2/TvService_src/sources/com/mediatek/twoworlds/tv/*.java` —— 触发目标 Java 层
- NDK r27c：`armv7a-linux-androideabi21-clang`（API29 匹配）

## 6. 建议下一轮决策点
A 可扩展路线：投入做 Step 0（最小验证，10min）→ Step1 jar 构建（1h）→ Step2 触发。
周期内可产出：**某 native 的参数引发越界崩溃的可复现证据 + 越界窗口长度** —— 这是 CVE 逆向的"可复现里程碑"。
B 若业力不胜（feature验证反复失败），诚实回退 UART（此时仍需拉起 shell 以便后续有线提权）。

> ⚠️ 红线重提：只在**独立子进程**（app_process 独立）触发，绝不打 TvService 主进程；每次 pull crash 前先快离线可；不在 `/dev/miomap`/display 设备上 ioctl。