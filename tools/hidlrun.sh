#!/bin/bash
# ============================================================
# hidlrun.sh — 以 system_app 域运行 tvhidl（HIDL 工厂接口客户端）
#
# 为什么必须"直接 exec 数据目录里的二进制"：
#   经 /system/bin/sh 会触发 domain_auto_trans(system_app, shell_exec, shell)
#   -> 掉到 shell 域，而 hal_tv_tvfactory_client 只含 app 域，HIDL 会被拒。
#   直接 exec system_app_data_file 无域转换，保持 u:r:system_app:s0。
#
# 为什么 .so 要放 /data：
#   ld.config.29.txt 的 namespace.default.permitted.paths 只有 /data、/system/...，
#   不含 /vendor/lib -> 直接 dlopen /vendor/lib/xxx.so 报 "not accessible for
#   the namespace (default)"。复制到 /data 下即可。
#
# 用法: ./hidlrun.sh <路径> <命令~用~代表空格> [passthru]
# 例:   ./hidlrun.sh /sys/kernel/mik/MI_UTIL help
#       ./hidlrun.sh /sys/kernel/mik/MI_UTIL wphy~0x12340000~0
#       ./hidlrun.sh /proc/self/attr/current anything
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
WRK="D:/Work/WorkBuddy/.tools/tv"
DIR=/data/data/com.mediatek.tv.factory
SO=vendor.mediatek.tv.mtktvfactory@1.0.so

PATH_ARG="${1:-/sys/kernel/mik/MI_UTIL}"
DATA_ARG="${2:-help}"
PASS="${3:-0}"
MODE="${4:-1}"
B1="${5:-0}"
B2="${6:-0}"

"$A" -s "$D" push "$WRK/hwrev/tvhidl" /sdcard/tvhidl >/dev/null 2>&1
"$A" -s "$D" push "$WRK/hwrev/$SO" /sdcard/$SO >/dev/null 2>&1

cd "$WRK" || exit 1
./tv_root_exec.sh "cp /sdcard/tvhidl $DIR/tvhidl && chmod 755 $DIR/tvhidl && cp /sdcard/$SO $DIR/$SO && chmod 644 $DIR/$SO && rm -f $DIR/tvhidl.log" >/dev/null 2>&1

echo ">>> exec: $DIR/tvhidl $PATH_ARG $DATA_ARG $PASS $MODE $B1 $B2"
"$A" -s "$D" shell "service call TvService 3 s16 \"$DIR/tvhidl $PATH_ARG $DATA_ARG $PASS $MODE $B1 $B2\"" 2>&1 \
  | tr -d '\r' | grep -v "Init wrapper"
sleep 4
./tv_root_exec.sh "cat $DIR/tvhidl.log 2>&1" 2>&1 | sed -n '2,200p'
