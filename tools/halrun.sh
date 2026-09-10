#!/bin/bash
# ============================================================
# halrun.sh — 以 system_app 域调用 HIDL tvfactory 接口（轻量版）
#   假设 tvhidl 与 proxy .so 已部署到 $DIR（首次用 hidlrun.sh 部署）
#
# 用法: ./halrun.sh <method> <arg1> <arg2> [b1] [b2]
#   method: 1=write_file 2=copy_file 3=check_file 4=create_file 5=remove_file
#   arg2 用 '~' 代表空格
# ============================================================
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
DIR=/data/data/com.mediatek.tv.factory
WRK="D:/Work/WorkBuddy/.tools/tv"

M="${1:?method}"; A1="${2:-/sys/kernel/mik/MI_UTIL}"; A2="${3:-help}"
B1="${4:-0}"; B2="${5:-0}"

cd "$WRK" || exit 1
./tv_root_exec.sh "rm -f $DIR/tvhidl.log" >/dev/null 2>&1
"$A" -s "$D" shell "service call TvService 3 s16 \"$DIR/tvhidl $A1 $A2 0 $M $B1 $B2\"" 2>&1 \
  | tr -d '\r' | grep -v "Init wrapper" >/dev/null
sleep 3
./tv_root_exec.sh "cat $DIR/tvhidl.log 2>&1" 2>&1 | sed -n '2,200p'
