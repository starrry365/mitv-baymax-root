#!/bin/bash
# ============================================================
# 从设备抽取 MMA 相关 blob 并导出符号 / 字符串 / ioctl 常量
#
# 用法:  ./extract-mma-ioctl.sh [设备序列号]
# 依赖:  adb(platform-tools)、NDK 的 llvm-nm / llvm-strings（可选，没有则退化用设备端 strings）
# ============================================================
set -e
SER="${1:-192.168.1.164:5555}"
OUT="$(pwd)/mma_rev"
mkdir -p "$OUT"

echo "== 0. 连接 =="
adb connect "$SER" || true
adb -s "$SER" wait-for-device

echo "== 1. 拉取实现模块与用户态库 =="
adb -s "$SER" pull /vendor/lib/modules/utpa2k.ko        "$OUT/" 2>/dev/null || \
adb -s "$SER" shell "cat /vendor/lib/modules/utpa2k.ko" > "$OUT/utpa2k.ko"
adb -s "$SER" shell "cat /vendor/lib/libutopia.so"       > "$OUT/libutopia.so"
adb -s "$SER" shell "cat /vendor/bin/cmdl_service"       > "$OUT/cmdl_service"
ls -la "$OUT"

echo "== 2. 设备节点 / 加载状态 =="
adb -s "$SER" shell 'lsmod | grep -E "utpa2k|mik|misck"' | tr -d '\r'
adb -s "$SER" shell 'ls -la /dev/mma 2>&1' | tr -d '\r'

echo "== 3. 导出全部字符串（含公式化的错误日志） =="
adb -s "$SER" shell '/system/bin/strings -n 4 /vendor/lib/modules/utpa2k.ko' > "$OUT/utpa2k.strings"
wc -l "$OUT/utpa2k.strings"

echo "== 4. 提取 29 个 mma_* 处理函数名 =="
grep -oE 'mma_[a-z_0-9]+' "$OUT/utpa2k.strings" | sort -u \
  | grep -vE '^mma_(0dot|dither|dark_higher|linear)' > "$OUT/mma-handlers.txt"
echo "--- 处理函数 ---"; cat "$OUT/mma-handlers.txt"

echo "== 5. 提取参数校验证据（泄露结构体字段名） =="
grep -oE 'error: [A-Za-z_0-9]+ (==|<=|>) [A-Za-z_0-9]+' "$OUT/utpa2k.strings" | sort -u
echo
grep -oE 'mma_[a-z_0-9]+ fail[^\n]*' "$OUT/utpa2k.strings" | sort -u

echo "== 6. 扫描 _IOC 常量（magic='M'） =="
if [ -f scan_ioctl2.py ]; then
    python "$OUT/../scan_ioctl2.py" "$OUT/libutopia.so" "$OUT/utpa2k.ko" || true
fi

echo "== 完成。产物在 $OUT =="
