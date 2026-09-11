#!/bin/bash
# uid0 通道拉取任意文件（稳健版，含重连） 用法: ./uid0_pull.sh <设备路径> [本地名]
A=/d/Tools/platform-tools/adb.exe
D=192.168.1.164:5555
SRV="$1"
NAME="${2:-$(basename "$SRV")}"
WORK="D:/Work/WorkBuddy/.tools/tv"
PULL_DIR="$WORK/native/mtkvapi_work"
mkdir -p "$PULL_DIR" "$WORK/tmp_uid0"

cmd_adb() {
  "$A" disconnect "$D" >/dev/null 2>&1
  for i in $(seq 1 10); do
    "$A" connect "$D" >/dev/null 2>&1
    if "$A" -s "$D" get-state 2>/dev/null | grep -q device; then break; fi
    sleep 2
  done
  timeout 30 "$A" -s "$D" "$@"
}

NONCE="$$_$(date +%s%N | tail -c 6)"
USER="/sdcard/uu_$NONCE.sh"
WRAP="/sdcard/cc_$NONCE.sh"

cat > "$WORK/tmp_uid0/ucmd.sh" <<EOS
export PATH=/system/bin:/vendor/bin:/system/xbin:\$PATH
{
set -x
SRV=$SRV
ls -l \$SRV
cp \$SRV /sdcard/pull_tmp
chmod 644 /sdcard/pull_tmp
sync
ls -l /sdcard/pull_tmp
} > /sdcard/cmd_out.txt 2>&1
echo "[done]" >> /sdcard/cmd_out.txt
EOS
{
  echo 'export PATH=/system/bin:/vendor/bin:/system/xbin:$PATH'
  echo 'timeout -s KILL 50 sh /sdcard/'$(basename "$USER")
  echo 'echo "[exit=$?]" >> /sdcard/cmd_out.txt'
} > "$WORK/tmp_uid0/cmd.sh"

rm -f "$WORK/tmp_uid0/cmd_out.txt"
cmd_adb push "$WORK/tmp_uid0/ucmd.sh" "$USER"
cmd_adb push "$WORK/tmp_uid0/cmd.sh" "$WRAP"
cmd_adb shell "rm -f /sdcard/cmd_out.txt; service call TvService 4400 s16 \"s\" s16 \"$WRAP\""
for i in $(seq 1 16); do
  sleep 3
  cmd_adb pull /sdcard/cmd_out.txt "$WORK/tmp_uid0/cmd_out.txt" >/dev/null 2>&1
  if [ -s "$WORK/tmp_uid0/cmd_out.txt" ] && grep -q 'exit=' "$WORK/tmp_uid0/cmd_out.txt" 2>/dev/null; then break; fi
done
echo "===== uid0 执行结果 ====="
tr -d '\000' < "$WORK/tmp_uid0/cmd_out.txt" 2>/dev/null | tr -d '\r'
echo "========================"
cmd_adb pull /sdcard/pull_tmp "$PULL_DIR/$NAME" 2>&1 | tr -d '\r'
ls -la "$PULL_DIR/$NAME" 2>/dev/null
cmd_adb shell "rm -f $USER $WRAP /sdcard/pull_tmp" >/dev/null 2>&1
echo DONE
