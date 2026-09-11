# 29 — phaseB 真机触发闭环 + CVE 载体(`mtktvapi@1.0-service`)拉取与函数地图

> 时间：2026-09-11 16:00（设备重启后 fully online）
> 前置：docs/22/23/25（JNI 越界候选 + 触发计划 + batch-diff 全零结论）
> 本文档：设备重启后的实战触发闭环验证 + 把 CVE-2023-32830 真正 OOB 载体的
> **HAL service 主二进制**从设备拉回 host，并给出其 exchange_data 处理函数地图。

---

## 1. 设备状态（重启后，2026-09-11 15:33 上线）

- `adb connect 192.168.1.164:5555` 约 18s 恢复，`sys.boot_completed=1`，`dtv_svc` 存活。
- system_app 通道（`service call TvService 3 s16 .../runSystemCommand`）重启后仍可用，uid=1000 system。

---

## 2. CVE 触发框架真实运行（一键脚本到达终态）

本轮完成 **Step 0 → Step 2** 全线实测：

### 2.1 修正 app_process32 参数顺序（关键）
```
❌ app_process32 /system/bin -Djava.class.path=... TvgTrigger safe 32   → Aborted(ClassNotFound -Djava.class.path=当作主类)
✅ app_process32 -Djava.library.path=/vendor/lib -Djava.class.path=<a.dex>:<b.dex> /system/bin TvgTrigger safe 32
```
- **`-Djava.class.path=` 和 `-Djava.library.path=` 必须放在 `<目录>` 参数之前**；
  否则 app_process 当它是主类名 → `ClassNotFoundException: -Djava.class.path…` SIGABRT。
- 双裸 dex 冒号分隔 classpath（`/sdcard/cve_tv.dex`=TvService 全类, `/sdcard/cve_tr.dex`=`TvgTrigger` 主类）
  规避 multi-dex jar 的 PathClassLoader 只读 classes.dex 限制，两 dex 均被 DexPathList 收录。

### 2.2 加 `-Djava.library.path=/vendor/lib` 的收获
- 未加的：`outputUARTSerial_native` 抛 `InvocationTargetException`（根因 `TVNative` JNI
  `System.loadLibrary("com_mediatek_twoworlds_tv_jni")` 找不到 `/vendor/lib` 下的 `.so`）。
- 加上后：`[UART] len=64 ret=-1` —— **native 真正被调用**。JNI 从"加载失败"到"真实调用"的
  关键修复。

### 2.3 全链路事件（system_app 域 uid1000）
| 测试 | 结果 | 判读 |
|---|---|---|
| Step0 `safe` | reflect OK；`TVNativeWrapper`/`MtkTvFactoryService` 类全 load OK；`TVNative` UnsatisfiedLink(需 lib, 规避) | 反射+类加载验证 PASS |
| `uart` len∈{1..65536} | 全 `ret=-1`（UART 未开，handle 无效提前返回） | 到不了 memcpy，无越界 |
| `uartopen` | `openUARTSerial(0..3)` 全 `-1`（配置未就绪）；output 全 `-1` | 无有效 UART handle |
| `all` | `out` len1~16K 全 -1；`dvbt_exchange` n1~1000 全 ret=0 | **零崩溃**，但 `logcat` 见 `jni GET type length is not enough`(n=1000) |

> **结论与 docs/25 完全吻合**：JNI 层所有 ExchangeData/UART 注入均无越界 —— JNI 层是
> 纯转发 + 长度保护，**CVE 真实 OOB 在其下的 HAL service**。

### 2.4 差分触发中间还发现 key 点
- `mtk` 日志 `[33]{Java_..._ScanDvbtExchangeData_1native} (jni GET type length is not enough)`
  —— 长度预检存在，**需要"定长 合法结构"而非任意长度**，印证 docs/22 假设。

> ⚠️ 底座：触发均发生在**独立 app_process32 子进程**，崩溃点在不同进程/不同内存，绝不触碰
> `system_app` 主进程、不做任何持久写入/ioctl；全部零风险。

---

## 3. CVE 载体：`vendor.mediatek.tv.mtktvapi@1.0-service` 主二进制拉取

docs/26 判定：**CVE-2023-32830 的 OOB 写在 HAL service 主进程
(`/vendor/mediatek/tv/v1` 主二进制)，不在 Android /vendor 库**。本次用
**uid0 通道** 打通读取（`tv_root_exec.sh` + `uid0_pull.sh`，DAC 全通可读任意文件，
非 root 的 SELinux shell 读不到 `/vendor/bin/hw/`）。

### 3.1 设备侧确认目标存在
```
-rwxr-xr-x 1 root shell  288268  /vendor/bin/hw/vendor.mediatek.tv.mtktvapi@1.0-service
```
拉回 host：`.tools/tv/native/mtkvapi_work/mtktvapi_service.bin`（288268 B）

### 3.2 ELF 快速画像
| 项 | 值 |
|---|---|
| arch | ARM (32-bit) ET_DYN |
| .symtab | strip（无调试符号） |
| .text | 0x1d000, size 0x1c6b4 (116KB) |
| .plt | 0x396c0, size 0x3030（ARM mode，stub16B×〜769） |
| 导入 | **memcpy(937)、__memcpy_chk(603)** ← OOB 载体 |
| .got.plt | 0x3f868 |
| 关键字符串 | 源码路径 `.../hh_mtktvapi_channlist.cpp`、`MtkTvApiScanImpl` 等 |

> **带 `__memcpy_chk` 说明已启用 `_FORTIFY_SOURCE`，但 `memcpy` 非 chk 调用点不受保护**
> —— 这正是不严的 memcpy（无 size 校验）是 CVE 打法。

### 3.3 ExchangeData HIDL 处理函数地图（rodata 内字符串定位）
| rodata addr | 函数名（HIDL handler / 代理） |
|---|---|
| 0x19591 | `a_hidl_a_scan_dvbt_exchange_data`（代理） |
| 0x1b5d5 | `hh_a_scan_dvbt_exchange_data`（服务端 handler） |
| 0x19964/0x19981 | `hh_a_scan_dvbs_exchange_data` / `hh_a_scan_dvbc_exchange_data` |
| 0x1787c / 0x17c58 / 0x18b88 | isdb / dtmb / atsc exchange |
| 0x167b0 / 0x1725f / 0x17285 / 0x1a5b6 | ce / pal_secam / dvbs / dvbc |
| 0x16981 / 0x1a13c | html_agent / hbbtv exchange |

`hh_*` 前缀 = HAL 服务端消费数组的主实现，是**追 OOB 的主攻面**。

---

## 4. 下一步（专门逆向阶段）

对 `mtktvapi_service.bin` 做**逐函数静态逆向**定位 OOB 写点：

1. **自动函数边界**（Ghidra / ID1 / radare2 以可 script 环境，radare2 在此二进制 `aa` 卡死，
   gh埋首行不可行 → 用 IDA Pro (本机有) 或 Ghidra headless 自动分析）。
2. **聚焦 `hh_*_exchange_data` + `MtkTvApiScanImpl`**：每函数内搜 `memcpy`/`memcpy@plt`，
   检查第三个参数（长度）是否来自 HIDL 传入的 `hidl_vec` size、且未做 bounds 校；
   命中即 **CVE-2023-32830 的写点**。
3. **倒推可构造的触发输入**：从"write 长度大于目标 buffer"反推 HIDL 客户端该传什么
   （`scan_*_exchange_data` 的 hidl_vec 结构和字段）。
4. 之后回到 device 用 **`TvgTrigger`（加精确结构）打真机**，观测 `dtv_svc`/service 崩溃。

> 红线延续：逆向结论前不得对 device 做破坏性实际注入；只做 host 静态分析 + 温和 diff。

## 5. 相关资产
- `native/mtkvapi_work/mtktvapi_service.bin` —— 已拉取的主二进制（288KB）
- `native/trigger_src/TvgTrigger.java` —— app_process32 触发入口（safe/all/uart/… 模式）
- `cve_dispatch/run_cve.sh` —— 一键调度（`./run_cve.sh 0 safe`/`1 all`）
- 工具：`xxx_memcpy_first.py`（线性 memcpy 调用扫）、`disasm_func.py`(ELF 函数反汇编)

*本文为 CVE-2023-32830 逆向推进的实战记录，不含可直接利用的 exploit；OOB 点待专项逆向确认。*