#!/bin/bash
# ============================================================
# 电视重启后的一键恢复：循环重连 adb → 校验 uid0 通道 → 校验 system_app 通道
#
# 背景：
#   * persist.adb.tcp.port=5555 已持久化，重启后 adbd 会自动起，
#     但开机头 ~20-40s 内 connect 会报 "Connection refused (10061)"，
#     必须循环重试（实测第 3~4 次成功）。
#   * 首次 connect 可能报 "failed to authenticate"，再连一次即 device。
#
# 用法:  ./reconnect.sh [等待秒数，默认 120]
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
WRK="$(cd "$(dirname "$0")/.." && pwd)"
WAIT="${1:-120}"

echo "[*] 等待电视重启并尝试连接 $D（最长 ${WAIT}s）"

ok=0
for i in $(seq 1 $((WAIT/4))); do
    "$A" connect "$D" >/dev/null 2>&1
    st=$("$A" -s "$D" get-state 2>&1 | tr -d '\r')
    if [ "$st" = "device" ]; then
        # 等 boot_completed，避免框架还没起来
        bc=$("$A" -s "$D" shell 'getprop sys.boot_completed' 2>&1 | tr -d '\r' | tr -d '\000' | grep -o '1' | head -1)
        if [ "$bc" = "1" ]; then
            echo "[+] 第 $i 次尝试连接成功，且 sys.boot_completed=1"
            ok=1; break
        fi
        echo "[-] 第 $i 次: adb 已通但系统未启动完成，继续等"
    else
        echo "[-] 第 $i 次: $st"
    fi
    sleep 4
done

[ "$ok" = "0" ] && { echo "[!] 超时仍未连上。可能需要在电视上手动重新打开一次 ADB 调试。"; exit 1; }

echo
echo "[*] 等待 5555 端口上的 adbd 稳定（避免刚连上就掉）"
sleep 3

echo
echo "=========== 1. uid0 通道 (misysdiagnose) ==========="
"$WRK/tv_root_exec.sh" 'id; cat /proc/version' 2>&1 | tail -8

echo
echo "=========== 2. system_app 通道 (TvService.runSystemCommand) ==========="
"$WRK/sysapp.sh" 'id; cat /proc/self/attr/current' 2>&1 | tail -8

echo
echo "[+] 恢复完成。"
