# 28 — 设备存储拓扑与 eMMC env 分区块定位（2026-09-11 深夜）

> 目标：把「重启关 SELinux → root」从抽象想法落到**可读的 eMMC env 分区块**。
> 本轮确认完整分区表（by-name 映射）、cmdline 中的 env 参数、以及 `IMtkTvFApiSystem`
> 里与 eMMC/env 相关的全部 HIDL 方法签名 —— 下一步就是选一条**只读**路径把这些数据读出来。

---

## 1. 前置问题：为什么需要读到 env

- SELinux enforcing + `device_state=locked` + `veritymode=enforcing` + `verifiedbootstate=green`（见 §3），
  没有任何硬件接口能廉改写。前 27 篇已系统关闭了物理内存/内核符号/MMA 等「运行时改」路线。
- 唯一改**持久配置**的路：内核启动从 eMMC env 读取 `androidboot.*` 参数。若能注入
  `androidboot.selinux=permissive`（或关 verity 所需对应项），**重启即永后门**，无需每次进 HAL 域。
- 前提是**先只读拿到 env 内容**（本阶段），确认格式 + 是否已含 `androidboot.selinux`/verity 字样，
  再判断是否可安全注入。任何盲写都可能 brick（无 UART）。

---

## 2. 分区表权威版 —— mmcblk0 完整 by-name 布局

`/dev/block/by-name/` 全部软链（uid0 域可读，system_app/hal 读目录被拒）。mmcblk0 共 **31 个分区**：

| by-name 符 | 设备节点 | 大小(块) | 说明 |
|---|---|---|---|
| `MBOOT` | mmcblk0p1 | 5120 | MStar boot（主 bootloader，含早期 uboot/env） |
| `MPOOL` | mmcblk0p2 | 3072 | MStar 启动参数/池 |
| `MBOOTBAK` | mmcblk0p3 | 5120 | 主 boot 备份 |
| `vbmeta` | mmcblk0p4 | 1024 | AVB 主 GBM (Android 侧校验) |
| `vbmeta_a` | mmcblk0p5 | 1024 | AVB 备用 |
| `tvcertificate` | mmcblk0p6 | 65536 | 电视证书区 |
| `eeprom_a` | mmcblk0p7 | 1024 | EEPROM 仿真 |
| `tvconfig` | mmcblk0p8 | 131072 | TV config 镜像 |
| `demura` | mmcblk0p9 | 3072 | 均色补充 |
| `misc` | mmcblk0p10 | 1024 | misc |
| `recovery` | mmcblk0p11 | 40960 | 刷机 recovery |
| `boot` | mmcblk0p12 | 40960 | 内核 boot |
| `optee` | mmcblk0p13 | 12288 | 可信执行环境 |
| `armfw` | mmcblk0p14 | 1024 | ARM 固件 |
| `RTPM` | mmcblk0p15 | 1024 | RTPM 信息 |
| `dtb` | mmcblk0p16 | 1024 | 设备树 |
| `dtbo` | mmcblk0p17 | 1024 | DTBO 重叠 |
| `frc` | mmcblk0p18 | 1024 | FRC 帧率 |
| `cm4` | mmcblk0p19 | 1024 | M4 协处理器 |
| `linux_rootfs_a` | mmcblk0p20 | 51200 | DTV 微系统 rootfs（glibc） |
| `3rd_a` | mmcblk0p21 | 30720 | 第三方 |
| `3rd_rw` | mmcblk0p22 | 30720 | 第三方 rw |
| `cha` | mmcblk0p23 | 1024 | 校验 a |
| `chb` | mmcblk0p24 | 1024 | 校验 b |
| `super` | mmcblk0p25 | 2592768 | 动态分区 super（≈1.2GB） |
| `cache` | mmcblk0p26 | — | 缓存（前面的超长分区） |
| `tvservice` | mmcblk0p27 | — | TV 服务数据 |
| `factory_a` | mmcblk0p28 | — | 工厂写分区 |
| `vbmeta_system` | mmcblk0p29 | — | 系统 vbmeta |
| `ipanic` | mmcblk0p30 | — | panic 日志 |
| `userdata` | mmcblk0p31 | — | 用户数据 |
| `mmcblk0boot0` | mmcblk0boot0 | 179,64 | eMMC 硬件 boot 区 |
| `mmcblk0boot1` | mmcblk0boot1 | 179,128 | eMMC 硬件 boot 备用 |

> 设备权限观察：`/dev/block/mmcblk0` 主体 `brw-rw---- .. system drmrpc`；分区多为
> `brw-rw---- root system`。只有 HAL 专属域（见 §5）持有效的 block 读权限。

---

## 3. cmdline 关键参数（system_app 域实测全文，去噪后）

```
security=selinux
androidboot.vbmeta.device=179:4        # vbmeta 在 mmcblk0p4 (major 179, minor 4)
androidboot.vbmeta.avb_version=1.0
androidboot.vbmeta.device_state=locked  # AVB 锁定
androidboot.vbmeta.hash_alg=sha256
androidboot.veritymode=enforcing       # dm-verity 强制
androidboot.verifiedbootstate=green     # verified boot 通过/绿色
ENV=EMMC  ENV_VAR_OFFSET=0x0  ENV_VAR_SIZE=0x10000   # ← env 在 eMMC，偏移 0，64KB
sysrq_always_enabled=1                   # 编译期启用，但 SELinux 拒写 /proc/sysrq-trigger
SECURITY=ON  security=selinux
```

**关键解读**：
- `ENV=EMMC ENV_VAR_OFFSET=0x0 ENV_VAR_SIZE=0x10000`：MStar 的启动环境变量存储于 eMMC，
  偏移从 0 开始、总大小 0x10000（64KB）。即 **env 位于 eMMC 起始头部（等价于若干体积最小分区，或主 boot 的开头）**。
- `vbmeta` 走 179:4（= mmcblk0p4），**主要 Android 侧校验**；MStar 侧环境在 `ENV=EMMC`。
- 当前没有 `/androidboot.selinux=` 显式项 ⇒ 默认依 `security=selinux` 进 enforcing。若 env 含
  `androidboot.selinux=permissive` 就能改成宽松。

---

## 4. 读取 env 的目标接口：IMtkTvFApiSystem（HAL 域）的 eMMC 方法

从 `vendor.mediatek.tv.mtktvfactory@1.0.so`（HAL 客户端/代理）mangled 符号还原出的
**全部与 eMMC/环境/增强固件相关的 HIDL 方法**，均可在 HAL 域（`hal_tv_…_default`）调用：

| 方法 | bind 层签名（proxy） | 作用 | 回调？|
|---|---|---|---|
| `read_emmc` | `(hidl_string, uint32, uint32, std::function<void(const hidl_vec<uint8>&, int)>)` | **原始读 eMMC** | ✅ 带回调 |
| `write_emmc` | `(hidl_string, uint32, hidl_vec<uint8>)` | **原始写 eMMC** | ❌ 无回调 |
| `get_emmc_env_var` | `(hidl_string key, std::function<void(hidl_string,int)>)` | **读一个环境变量 key** | ✅ 带回调 |
| `set_emmc_env_var` | `(hidl_string key, hidl_string value)` | **写一个环境变量 key** | ❌ 无回调 |
| `upgrade_fw` | `(hidl_string) `| 固件升级 | ❌ |
| `set_wdt` / `get_wdt_status` | — | 看门狗 | ❌/✅ |
| `sync_fs` | — | 同步 fs | ❌ |
| `set_miu_ssc` | — | MIU SSC 设置 | ❌ |

另有 `get_dataindex_path`、`get_customer_hash`、`check_hdcp_key_isvalid` 等只读探测。

**对只读 env**：
- **`read_emmc(hidl_string 设备路径, offset, len, 回调)`** 最接近「原始读块」—— 若第一参是可传
  的块设备路径或 eMMC 分区标识，可直接读 MBOOT/头部。**这是本阶段最优先要正向验证的**。
- `get_emmc_env_var(key, 回调)` 是按名读单个变量——命令构造最小，但我们不知道 key 全名列表，
  且它走 MStar 解析器，能还原格式化值。
- `write_emmc`（无回调）**签名最简洁但绝对红线**：只读阶段禁碰，防止变砖。

> ⚠️ 纯 C 客户端（tvhidl`）构造 `std::function<void(hidl_string,int)>` / `std::function<void(const hidl_vec<uint8>&,int)>`
> 回调的 ABI，是 32 位 libc++ 内部（SFO 或 vtable 指针 + 目标指针 + stub）。下一阶段需
> 在本机 libc++（已从设备拉到 `hwrev/libc++.so`）上反汇编精确构造。这是「只读 env」的最后一步。

---

## 5. HAL 域块设备读权限（为何能由 HAL 读而 uid0/system_app 不能）

SELinux 中 `hal_tv_tvfactory_default`（HAL 服务进程域）对 **`mstar_mboot_block_device`**（= MBOOT 等
boot 分区）有 `blk_file (ioctl read write getattr lock append map open)` —— 这解释了为何
只有 HAL 域能 **读 block 内容**；而 uid0（misysdiagnose）与 system_app 对 `/dev/block/*`
与 `/dev/mik!flash` 收到的是 SELinux **Permission denied**（非 DAC，属策略，无法绕过）。

> 这也印证 doc 27「flash/block 直读不廉价走通」的本质：**卡在 SEL 域标注**，根因在 domain 转换。

---

## 6. 结论与下一步

1. **本阶段已确认**：分区全景 + env 在 eMMC 头部（start offset 0）/MBOOT 区；cmdline 无
   `androidboot.selinux` 显式覆盖；MStar 侧 env 大小 64KB。
2. **下一步（纯读）**：
   1. 在 `tvhidl`（或新 `tvhidl_read`）中用 **`read_emmc`**（第一参 = MBOOT 路径
      `/dev/block/mmcblk0p1` 或 `by-name/MBOOT`）读 eMMC 头部 64KB，看 env 明文格式
      （MStar env 常为 `name=value` 逐行文本，含 `bootargs`/`androidboot.*`）。
   2. 同时用 **`get_emmc_env_var("bootargs")`/`("androidboot.bootargs")`** 探已知 key。
   3. 只读拿到 env：枚举是否已含 `selinux`/`verity` 覆盖项；否则评估注入可行性（需写，另判）。
3. **红线**：严禁 `set_emmc_env_var` / `write_emmc` / `upgrade_fw`。无 UART 救不回。

> 本档案目标是盘活「持久后门」这条最后的完整 root 路；即便注入不可行，本次分区/方法
> 枚举在后续 CVE 利用或刷机研究里也是地基。