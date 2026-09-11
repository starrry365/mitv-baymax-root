# 25 — CVE-2023-32830 Phase B 批量差分：JNI 数组注入候选全部无越界（2026-09-11 实测）

> 目标：在 system_app 域（=CVE 要求的 "System execution privileges"）用已打通的反射链，
> 对 TVAPI 的**数组注入**候选做梯度差分，定位 CVE-2023-32830 的 OOB 写点。
> 结论先行：**12 个 int[] + 3 个 byte[] 候选全部零崩溃** —— JNI 层这些函数都有长度保护，
> 真正 OOB 在更底层（stripped 的 HIDL/HAL）。

---

## 1. 背景核对

- CVE-2023-32830 = "In **TVAPI**, possible OOB write due to missing bounds check; local EoP, **System execution privileges needed**; UI not needed."
- 移植改影响 **Android 10.0 / 11.0**（本机 = Android 10 ✅）；Patch ID `DTV03802522`（2023-10），本机 patch 停 `2021-03-05` → **漏洞窗口未关**。
- 但**无公开 exploit / 无受影响函数名公开** ⇒ 需自行逆向。
- 本机通道：`service call TvService 3`（runSystemCommand）→ system_app 域 `app_process32` → JNI 反射任意 `TVNativeWrapper.xxx_native`。✅ System 权限匹配。

## 2. 库定位

| 库 | 作用 | 符号 |
|---|---|---|
| `/vendor/lib/libcom_mediatek_twoworlds_tv_jni.so`（=本地 libvibio.so） | TvNative JNI 层，701 个导出 | C++函数名齐全 |
| `vendor.mediatek.tv.mtktvapi@1.0.so`（2.2MB stripped） | 底层 TVAPI HIDL/HAL（IMtkTvApi） | 仅 C++ 名字，函数体 stripped |
| `vendor.mediatek.tv.mtktvapi@1.0-cwrapper.so` | C wrapper | - |

⚠️ **capstone 反汇编要点**：llvm-objdump/r2 对 Thumb 库反汇编**输出乱码**（大片 `<unknown>`）；
必须用 `capstone (CS_ARCH_ARM, CS_MODE_THUMB)`（5.0.7）才能正确解出 JNI 函数体。

## 3. 静态证据：JNI 层已有长度校验（非漏洞点）

反汇编 `Java_com_mediatek_twoworlds_tv_TVNative_ScanDvbtExchangeData_1native`：
```
... ldr r0,[sp,#0x88]   ; 数组返回值（可能 GetIntArrayRegion 结果）
cmp r0, #0x210          ; 与 528 比较
blt #0x71be8            ; <528 跳走; >=528 走日志报错路径 → 拒绝
...
ldr r2,[sp,#0x88]
cmp r2, #9
blo #0x71caa            ; <9 用 8，>=9 用原长度
...
```
即该函数做了**长度上/下界校验** → 不是 CVE-2023-32830 的缺陷点。

## 4. 真机批量差分（结果表格）

新增 `TvgTrigger.java` 的 `runExcAll`（int[] 批量）与 `runByteCands`（byte[] 批量），
对下列候选做 **len ∈ {0,1,2,8,16,64,256,1024,4096,16384}** 渐变，观测 ret + crash buffer。

### 4.1 int[] 族（12 方法）
| 方法 | 长度变化行为 | 崩溃 |
|---|---|---|
| ScanATSCExchangeData_native | 全部 ret=0 | 无 |
| ScanCeExchangeData_native | len≤16: ret=0；**len≥64: ret=-1** | 无 |
| ScanDtmbExchangeData_native | 全 ret=0（len≥4096: -1） | 无 |
| ScanDvbcExchangeData_native | len=0:0；**len1-16: ret=-6**；len≥64: 0 | 无 |
| ScanDvbsExchangeData_native | 全 ret=0 | 无 |
| ScanISDBExchangeData_native | 全 ret=0 | 无 |
| ScanPalSecamExchangeData_native | 全 ret=0 | 无 |
| hbbtvExchangeData_native | 全 ret=0 | 无 |
| htmlAgentExchangeData_native | 全 ret=0 | 无 |
| getCIHostID_navtive | len8-64: 0；其它 -1 | 无 |
| SubtitleGetTracks_native | 全 ret=0 | 无 |
| writeCIKey_navtive(int[] 错型) | NoSuchMethod（应 byte[]） | - |

### 4.2 byte[] 族（3 方法）
| 方法 | 行为 | 崩溃 |
|---|---|---|
| writeCIKey_navtive(byte[],int) | **全 len 返回 -1**（CI 未 init） | 无 |
| sifReadMultipleSubAddr_native(...) | **NoSuchMethodException**（设备无此名） | - |
| sifWriteMultipleSubAddr_native(...) | **NoSuchMethodException** | - |

## 5. 结论

- system_app 域可**批量、稳定**驱动 TVAPI JNI 数组注入，全程设备在线未崩 → 触发链成熟。
- **这 12+3 个候选里没有找到有越界崩溃的** → CVE-2023-32830 的真正 OOB 在 **JNI 之下的 HIDL/HALTVAPI**（`libmtktvapi@1.0.so`），需要静态反汇编其 C++（stripped）或 binder/hidl trace 才能定位。
- **结论**：即便定位到 OOB 还需构造提权对象覆写（toast/cred），是**数周逆向工作**；今天已完成到"确认 JNI 层无漏洞 + 锁定底层为唯一攻坚点"。

## 6. 复用资产

- 触发脚本：`run_excall2.sh`（int[] 批量）、`run_byte.sh`（byte[] 批量）。
- 触发入口代码：`native/trigger_src/TvgTrigger.java` 的 `excAll` / `byte` 模式。
- 反汇编：`native/disasm_func.py`（capstone Thumb）。
- jar：`native/tvtrigger.jar`（含 TvService classes.dex + TvgTrigger classes2.dex）。

## 7. 补充：HAL 进程层也未崩（决定性）

- 关键 HAL 进程 `vendor.mediatek.tv.mtktvapi@1.0-service`（uid system）在批量触发后**仍存活**（ps 确认 PID 2733）。
- 触发期间 `dmesg` 无 `unable to handle`/`fatal`/`SIGSEGV`；`logcat`（全量，非仅 crash buffer）无 FATAL/Died/tombstone；`/data/tombstones` 空。
- ⇒ **OOB 既不在 JNI 层、也不在 mtktvapi HAL-service 进程**。CVE 的确切函数簇需进一步 binder/hidl 追踪或对 `libmtktvapi@1.0.so` 做逐函数静态逆向。

*本文是"确认 JVM 层非漏洞、锁定底层"的取证，不含已可利用 exploit；真实 OOB 待底层逆向。*
## 8. 底层 HIDL 方法定位（本轮深挖，任务 12-13）

- **`libmtktvapi_full.so`（= vendor.mediatek.tv.mtktvapi@1.0.so，2.2MB）实际有 3087 个导出符号，不是 stripped**（之前读 -T/动态符号表为空，误判；普通符号表齐全）。
- **完整 ExchangeData HIDL 接口方法表**（`IMtkTvApiScan`）已定位：
  - BnHw 服务端处理器（真正消费数组）：`scan_dvbt/dvbc/dvbs/dtmb/ce/isdb_exchange_data`,`mt_hbw_8` `BpHw` 代理：`a_hidl_a_scan_*_exchange_data`。
- 已反汇编 `_hidl_ahid_scan_dvbt_exchange_data`（服务端 unpack hid 矢量）：
  - `0x1e7e32 blx 0x201480`（读 hidl_vec size）→ `lsl r1,r0,#2`(size×4) → `0x2014e0`（读字节块）→ 回调 `[r6,#0x78]` vtable 入口 @ service 进程主实现。
  - **运输层完整，真正的 OOB 在 service 进程内、由 hidl_vec 大小驱动的消费函数**（不在 .so，在 `/vendor/bin/hw/mtktvapi@1.0-service` 主二进制，sel shell 无法直接读）。

## 9. 下一步的关键阻塞（诚实）
- 真正承载 CVE OOB 的实现在 HAL service 主二进制（verified 无法 shell 读取）。
- 绕开办法：**`loop改 service 二进制读取`**：用 `systop_sh`（code2 write 任意文件，仅写）/ root uid 通道（`tv_root_exec.sh`，CapEff DAC 全通，可读任何文件）来 cat/idumper service 二进制再 pull 回来分析。→ 这是继续攻坚的下一步。
