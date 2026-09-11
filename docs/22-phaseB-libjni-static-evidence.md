# 22 — Phase B 静态分析：TVNative 越界候选函数真实汇编证据（libjni.so）

> 时间：2026-09-11 11:45
> 设备：小米电视 ES65 2022（在线，已物理重启并授权 adb）
> 素材：已从真机 pull `/vendor/lib/libcom_mediatek_twoworlds_tv_jni.so`（738,972 B，symtab 完整保留）
> 工具：`native/dump_syms.py`（pyelftools 列符号）+ `native/disasm_func.py`（capstone Thumb 反汇编）
> 结论先行：**P0 候选符号全部存在于真实 libjni.so，且已从汇编层面确认了「JNI 数组元素指针 + 长度」传入底层调用的清晰路径**；但同时也看到若干**数组长度分支检查**，详下。

---

## 1. 已确认真实符号（PDB: libjni.so 是 TVNative JNI 桥本体）

`com_mediatek_twoworlds_tv_jni` = JNI 类名 `com.mediatek.twoworlds.tv.TVNative` 的宿主库。
从中提取到全部 P0/P1 候选（VA = 函数符号虚拟地址，Thumb）：

| JNI 方法（Java 侧 protected static） | 符号 VA | size | 备注 |
|---|---|---|---|
| `ScanATSCExchangeData_1native(int[])` | 0x4b8d9 | 444 | P0 族 |
| `ScanCeExchangeData_1native` | 0x6dd95 | 568 | P0 |
| `ScanDtmbExchangeData_1native` | 0x6d591 | 600 | P0 |
| `ScanDvbcExchangeData_1native` | 0x6f195 | 616 | P0 |
| `ScanDvbsExchangeData_1native` | 0x767ed | 608 | P0 |
| `ScanDvbtExchangeData_1native` | 0x71b61 | 668 | P0 |
| `ScanISDBExchangeData_1native` | 0x65ac1 | 460 | P0 |
| `ScanPalSecamExchangeData_1native` | 0x713f5 | 460 | P0 |
| `hbbtvExchangeData_1native` | 0x5deb9 | 492 | P0 |
| `htmlAgentExchangeData_1native` | 0x4cf95 | 464 | P0 |
| `openUARTSerial_1native(int,int[],int[])` | 0x7248d | 796 | P0 |
| `outputUARTSerial_1native(inti,byte[])` | 0x73021 | 508 | **最典型** |
| `setUARTSerialSetting_1native` | 0x72cc5 | 340 | P1 |
| `getInfo_1native(byte,j,i,int[])` | (见 dump) | — | P1 |
| `setInfo_1native` | — | — | P1 |
| `getCIHostID_1navtive(int[])` | — | — | P1 |

---

## 2. `outputUARTSerial_1native` 汇编逻辑（VA 0x73020，508B）

JNI 标准签名（ARM）：`r0=JNIEnv*，r1=jobject，r2=jint i，r3=jbyteArray bArr`
寄存器分配：
- r2(第1个 param int i) → `[sp,#0x50]`（误差保存）
- r3(jbyteArray) → `[sp,#0x48]`
- 本栈 `[sp,#0x54]=JNIEnv*`，`[sp,#0x50]=jobject`

核心序列：
```
0x73072  ldr r1,[sp,#0x54]      ; env
0x73074  ldr r2,[r1]            ; env->functions
0x73076  ldr.w r2,[r2,#0x2ac]   ; env->functions[171]  ← GetByteArrayLength
0x7307e  mov r0,r1 ; r1=r3=[sp,#0x48](jbyteArray)
0x73082  blx r2                 ; jbyteArray length -> [sp,#0x40]
0x73088  cmp r0,#0 ; bne   — 若 len==0 则 record 日志并 return -1
0x7310c  ldr r1,[r0] ; r1=env->functions
0x73110  ldr.w r1,[r1,#0x320]  ; functions[0x320/4] ← GetByteArrayElements
0x73116  r2=[sp,#0x48](jbyteArray) ; r3=[sp,#0x40](len)
0x73118  ip=[sp,#0x3c](NULL) ; [sp]=ip(NaN)
0x73130  blx ip              ; elem_pt = GetByteArrayElements(env, arr, NULL) -> [sp,#0x3c]
0x7313a  r0=[sp,#0x40](len) ; str r0,[sp,#0x5c]
0x7313c  add r1,sp,#0x5c    ; &{buf=元素指针, len=长度}
0x7313e  blx #0x8db00       ; ★ 核心写路径: 传入字段 `{void* buffer, size_t len}` → UART
0x73148  beq skip
0x7314e  blx #0x8bcf0       ; ReleaseByteArrayElements
```

### 关键观察
- `[sp,#0x5c]` 构成 **`{buf_ptr, len}`** 的 8 字节块，0x7313e 调用 `#0x8db00`（`Output_UART`/`MTK_UART_write` 等底层的 C 封装）。
- 这个调用直接用 **调用方控制长度** = 传给底层；底层若按固定 size 或把 len 当指令参数再写 → 越界点就在 `#0x8db00` 内部（不在 JNI 层）。
- 数组长度**先被检查了一次**（`cmp #0; beq` 0 长度拒绝），但 **`#0x8db00` 内部是否对 len 又做 bounds 是未知** —— 需继续 `#8db00`/`#8bcc0`。

---

## 3. `ScanATSCExchangeData_1native`（0x4b8d9）汇编逻辑

JNI 入口 `r2=jintArray`。分支：
- fetch `GetArrayLength(env, iArr)` → `[sp,#0x54]`
- `cmp r0,#0x33(51)`：`>=51` 走「先读已填好后再转入 exchange」分支
- `blt`（<51）→ 另一日志分支（可能信号未就绪）
- 长度 threshold=51 是该数组的**期望长度**；传入 <51 会走「未就绪」错误分支而非直接越界 → 这暗示 native 期望 iArr.length==51 这个常量。

> 因此，ExchangeData 族很可能是「**期望定长数组**」：caller 传**合法 51 长度**即正常交互；**过大/异常**才可能 OOB。真正需要测试的是 `outputUARTSerial`（byte[] 任意长度直入 write）。

---

## 4. 结论与下一步

1. **已从静态层确认**：越界面在 JNI 之下（`#0x8db00` HAL / 后续 C 层），仅凭 `.so` 反汇编已拿到“数组指针+长度”传入的证据。
2. **触发方式**：要真正调用 `outputUARTSerial_native` 需在 device 上跑一个 **Android 域内的 Java 调用点**（`protected static native`）。候选方案：
   - ① 用 NDK 编一个 `app_process` Java 启动器（dexdump 打包反射调用）注入 system_app 域 → 复杂度高；
   - ② 编译一个「只含 `com.mediatek.twoworlds.tv.TVNative` 最小值 stub」+ 反射 Apk，在 TvService 或独立 system_app 进程里触发（最贴合 CVE）；
   - ③ 直接用 frida（若设备有）原地 hook 调用。
3. `#0x8ccf0 / #0x8db00` 是 `libjni.so` 内的**本地导入**解析（ret thunk/ifunc 的可能），需反汇编这两处确认是不是写 / call 原语。

> ⚠️ 红线：所有测试**只在可控进程**（独立 app_process / 独立进程）内打，绝不打 TvService 主进程；分步、可回读、异常即停。绝不在 display/`/dev/miomap` 上做 ioctl。