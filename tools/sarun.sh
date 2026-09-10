#!/bin/bash
# 在 system_app 域运行原生二进制
# 落地目录: /data/data/com.mediatek.tv.factory  (SELinux 类型 = system_app_data_file)
# 输出文件: $DIR/probe_out.txt  (由 uid0(misysdiagnose) 通道读取)
# 用法: ./sarun.sh <本地二进制> [args...]
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
WRK="D:/Work/WorkBuddy/.tools/tv"
DIR=/data/data/com.mediatek.tv.factory

BIN="$1"; shift
[ -z "$BIN" ] && { echo "用法: $0 <bin> [args]"; exit 1; }
BASE=$(basename "$BIN")

"$A" -s "$D" push "$BIN" "/sdcard/$BASE" >/dev/null 2>&1
cd "$WRK" || exit 1
./tv_root_exec.sh "cp /sdcard/$BASE $DIR/$BASE && chmod 755 $DIR/$BASE && rm -f $DIR/probe_out.txt && ls -laZ $DIR/$BASE" >/dev/null 2>&1

"$A" -s "$D" shell "service call TvService 3 s16 \"$DIR/$BASE $*\"" 2>&1 | tr -d '\r' | grep -v "Init wrapper" | head -2
sleep 4
./tv_root_exec.sh "cat $DIR/probe_out.txt 2>&1" 2>&1 | sed -n '2,400p'
