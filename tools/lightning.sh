#!/bin/bash
# ============================================================
# lightning.sh — 开机 47 秒窗口内闪电执行（收集+修复）
# 用法: ./lightning.sh
# 原理: 电视开机后 adbd 有短暂存活窗口，必须抓住
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
OUT=/d/Work/WorkBuddy/.tools/tv/lightning_out.txt

echo "[$(date '+%H:%M:%S')] 闪电脚本启动，等待设备..."

# Phase 1: 极速抢占连接
for i in $(seq 1 300); do
  timeout 3 "$A" connect "$D" >/dev/null 2>&1
  OUT2=$(timeout 4 "$A" -s "$D" shell "cat /proc/uptime 2>/dev/null" 2>&1 | tr -d '\r' | grep -v "Init wrapper")
  if echo "$OUT2" | grep -qE "^[0-9]"; then
    echo "[$(date '+%H:%M:%S')] ✅ 连上了！uptime=$OUT2 (try $i)"
    
    # Phase 2: 闪电执行（所有命令打包成一个，减少往返）
    timeout 30 "$A" -s "$D" shell '
      echo "===BASE==="
      id
      cat /proc/uptime
      getprop sys.boot_completed
      getprop persist.adb.tcp.port
      
      echo "===UTOPIA==="
      ls -la /proc/utopia 2>&1
      head -20 /proc/utopia 2>&1
      
      echo "===PROC_MSTAR==="
      ls /proc/ 2>/dev/null | grep -iE "utopia|mik|mi3|mstar|cmdline"
      
      echo "===ADB_CRASH_LOG==="
      logcat -d -s adbd 2>/dev/null | tail -20
      dmesg 2>/dev/null | grep -iE "adb|adbd|selinux|denied|panic|oops|bug" | tail -30
      
      echo "===SELINUX==="
      ls -la /sys/fs/selinux/enforce 2>&1
      cat /sys/fs/selinux/enforce 2>&1
      
      echo "===VMALLOC_HEAD==="
      head -5 /proc/vmallocinfo 2>/dev/null
      
      echo "===MODULES==="
      cat /proc/modules 2>&1 | head -10
      
      echo "===SYSrq==="
      cat /proc/sys/kernel/sysrq 2>&1
      
      echo "===DONE==="
    ' 2>&1 | tr -d '\r' | grep -v "Init wrapper" > "$OUT"
    
    echo "[$(date '+%H:%M:%S')] 数据已保存到 $OUT"
    cat "$OUT"
    
    # Phase 3: 尝试修复 adbd 防崩溃
    echo "[$(date '+%H:%M:%S')] 尝试稳定 adbd..."
    timeout 5 "$A" -s "$D" shell "setprop service.adb.tcp.port 5555" >/dev/null 2>&1
    
    # 检查是否还活着
    OUT3=$(timeout 4 "$A" -s "$D" shell "cat /proc/uptime 2>/dev/null" 2>&1 | tr -d '\r' | grep -v "Init wrapper")
    if echo "$OUT3" | grep -qE "^[0-9]"; then
      echo "[$(date '+%H:%M:%S')] ✅ adbd 仍存活！uptime=$OUT3"
    else
      echo "[$(date '+%H:%M:%S')] ❌ adbd 又断了"
    fi
    exit 0
  fi
  sleep 1
done
echo "超时（5分钟未连上）"
exit 1
