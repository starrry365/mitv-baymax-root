/* ioread.c —— 零风险：只读取 proc/sysfs 文本文件，完全不碰 /dev/miomap、不做任何 mmap。
   目的：拿到 DRAM 物理布局（System RAM 区间）与 cmdline 里的 LX_MEM 等信息，
        供后续精准定位内核 Image，避免盲目扫描物理地址把机器扫挂。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define LOGPATH "/data/data/com.mediatek.tv.factory/ioread.log"
static FILE *g_log;
static void catfile(const char *path, long limit) {
    FILE *f = fopen(path, "r");
    static char buf[4096];
    long total = 0;
    if (g_log) fprintf(g_log, "----- %s -----\n", path);
    if (!f) { fprintf(g_log, "     <open failed: %s>\n", strerror(errno)); return; }
    while (fgets(buf, sizeof(buf), f)) {
        long n = (long)strlen(buf);
        total += n;
        if (limit > 0 && total > limit) { fprintf(g_log, "     <truncated>\n"); break; }
        fputs("     ", g_log);
        fputs(buf, g_log);
        if (n > 0 && buf[n-1] != '\n') fputc('\n', g_log);
    }
    fclose(f);
}

int main(void) {
    g_log = fopen(LOGPATH, "w");
    if (g_log) setvbuf(g_log, NULL, _IONBF, 0);
    FILE *sf = fopen("/proc/self/status", "r");
    if (sf && g_log) {
        char line[512];
        while (fgets(line, sizeof(line), sf))
            if (!strncmp(line, "Uid:", 4) || !strncmp(line, "Groups", 6) ||
                !strncmp(line, "CapEff", 6) || !strncmp(line, "CapPrm", 6))
                fprintf(g_log, "%s", line);
        fclose(sf);
    }
    catfile("/proc/cmdline", 4096);
    catfile("/proc/iomem", 200000);
    catfile("/proc/modules", 20000);
    catfile("/proc/vmallocinfo", 200000);
    catfile("/sys/module/mik/sections/.text", 1024);
    catfile("/proc/kallsyms", 20000);
    catfile("/proc/version", 1024);
    if (g_log) { fprintf(g_log, "[*] ioread done\n"); fclose(g_log); }
    return 0;
}
