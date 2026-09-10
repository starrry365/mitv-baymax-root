#!/bin/bash
# ============================================================
# 小米电视 ES65 2022 (baymax) —— uid 0 命令执行器  v2
#
# 原理: TvService 的 binder transact(4400) 无调用方鉴权, 会把传入的
#       (code, arguments) 直接写进 vendor.misysdiagnose.cmd.code / .cmd.arguments,
#       触发 /init.mitv.rc:
#         on property:vendor.misysdiagnose.cmd.code=*
#           exec - root root -- /vendor/bin/misysdiagnose -{code} {arguments}
#       => init 以 uid 0 执行我们放在 /sdcard 的 shell 脚本。
#
# ⚠️⚠️ 血泪教训 (2026-09-10) ⚠️⚠️
#   init 的 `exec` 是 **阻塞** 的! 只要 misysdiagnose 或它的任一子进程
#   不退出, init 主循环就永久卡死 -> 之后所有 on property: 触发器全部失效,
#   连 service call 都再也不会被 exec (但 TvService 仍会回 "ok")。
#   症状: 输出永远是上一次的缓存; ps 里能看到 PPid=1 的 misysdiagnose+sh+xxx,
#         kill -9 报 Operation not permitted (init 是父进程, 杀不掉)。
#   唯一解法: adb reboot (init 对 sys.powerctl 有特殊旁路, 即使卡死也能重启)。
#   ==> 因此本 v2 强制用 `timeout -s KILL` 包裹用户命令, 任何情况 25s 内必被杀。
#
# 用法:  ./tv_root_exec.sh "id; ls /data/system"
# 环境:  TV_TIMEOUT=40 ./tv_root_exec.sh "长任务"   # 覆盖超时秒数(谨慎,上限建议60)
#
# 权限: 脚本运行于 u:r:misysdiagnose:s0 域, uid=0 + CapEff=0x3fffffffff(DAC全通),
#       但 SELinux 仍限制: 不能改 /system、不能 insmod、不能关 SELinux、
#       不能 binder 到 system_server、无 net_admin、无 bpf。
#       命令输出必须自己重定向到 /sdcard, 否则拿不到。
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
WRK="D:/Work/WorkBuddy/.tools/tv"
CMD_TIMEOUT="${TV_TIMEOUT:-25}"

if [ -z "$1" ]; then
    echo "用法: $0 '<要执行的命令>'   [TV_TIMEOUT=秒数]"
    exit 1
fi

# nonce: init 只在属性"值变化"时触发 on property:*, 故每次用不同文件名
NONCE="$$_$(date +%s%N | tail -c 7)"
USER="/sdcard/u_$NONCE.sh"
WRAP="/sdcard/c_$NONCE.sh"

# 1) 用户命令本体
{
  echo 'export PATH=/system/bin:/vendor/bin:/system/xbin:$PATH'
  echo '{'
  echo "$1"
  echo '} > /sdcard/cmd_out.txt 2>&1'
} > "$WRK/ucmd.sh"

# 2) 包装脚本: 硬超时 + 退出码回写 (即使子进程卡住, timeout 也会 KILL 掉整个进程组)
{
  echo 'export PATH=/system/bin:/vendor/bin:/system/xbin:$PATH'
  echo "timeout -s KILL $CMD_TIMEOUT sh $USER"
  echo 'echo "[exit=$?]" >> /sdcard/cmd_out.txt'
} > "$WRK/cmd.sh"

rm -f "$WRK/cmd_out.txt"
"$A" -s "$D" push "$WRK/ucmd.sh" "$USER" >/dev/null 2>&1
"$A" -s "$D" push "$WRK/cmd.sh" "$WRAP" >/dev/null 2>&1

# 3) 触发 (TvService 无鉴权)
"$A" -s "$D" shell "rm -f /sdcard/cmd_out.txt; service call TvService 4400 s16 \"s\" s16 \"$WRAP\"" 2>&1 \
  | tr -d '\r' | grep -v "Init wrapper"

# 4) 轮询取回 (最多 ~30s)
for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 3
    "$A" -s "$D" pull /sdcard/cmd_out.txt "$WRK/cmd_out.txt" >/dev/null 2>&1
    if [ -s "$WRK/cmd_out.txt" ] && grep -q '^\[exit=' "$WRK/cmd_out.txt" 2>/dev/null; then break; fi
done

# 5) 清理
"$A" -s "$D" shell "rm -f $USER $WRAP" >/dev/null 2>&1

echo "================= uid 0 执行结果 ================="
cat "$WRK/cmd_out.txt" 2>/dev/null | tr -d '\000'
echo "=================================================="
