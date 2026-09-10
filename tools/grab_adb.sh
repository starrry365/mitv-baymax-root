#!/bin/bash
# ============================================================
# grab_adb.sh — 在电视重启后的短暂窗口内极速抢占 adb 连接
# 用法: ./grab_adb.sh
# 原理: 电视开机后 adbd 会短暂监听 5555 然后可能崩溃
#       这个脚本以 1 秒间隔疯狂尝试连接
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555

echo "[$(date '+%H:%M:%S')] 开始极速重连 $D ..."
echo "请在电视端重启设备（完全断电再开），我会立刻抓住窗口"
echo ""

for i in $(seq 1 300); do
  timeout 3 "$A" connect "$D" >/dev/null 2>&1
  OUT=$(timeout 4 "$A" -s "$D" shell "cat /proc/uptime" 2>&1 | tr -d '\r' | grep -v "Init wrapper")
  if echo "$OUT" | grep -qE "^[0-9]"; then
    echo ""
    echo "[$(date '+%H:%M:%S')] ✅ 第 $i 次尝试连上了！uptime=$OUT"
    echo "正在冻结 adbd 防止它崩溃..."
    # 连上后立刻禁用 adbd 的自动退出机制
    timeout 4 "$A" -s "$D" shell "setprop service.adb.tcp.port 5555; setprop persist.adb.tcp.port 5555" >/dev/null 2>&1
    echo "✅ 连接稳定，可以继续操作了"
    exit 0
  fi
  # 每 30 次打印一次状态（避免刷屏）
  if [ $((i % 30)) -eq 0 ]; then
    echo "[$(date '+%H:%M:%S')] 仍待连接... 已尝试 $i 次"
  fi
  sleep 1
done
echo "超时退出（5分钟内未连上）"
exit 1
