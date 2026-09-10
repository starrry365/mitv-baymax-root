/* probe_open.c - 只测试 open(2) 权限，不写入任何数据（安全） */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

struct item { const char *path; int wr; };

int main(int argc, char **argv) {
    const char *paths[] = {
        /* 内核 sysctl —— 能写 core_pattern/modprobe 就等于直接 root */
        "/proc/sys/kernel/core_pattern",
        "/proc/sys/kernel/modprobe",
        "/proc/sys/kernel/kptr_restrict",
        "/proc/sys/kernel/dmesg_restrict",
        "/proc/sys/kernel/panic_on_oops",
        "/proc/sys/kernel/perf_event_paranoid",
        "/proc/sys/kernel/unprivileged_bpf_disabled",
        "/proc/sys/vm/mmap_min_addr",
        "/proc/sys/vm/panic_on_oom",
        "/proc/sysrq-trigger",
        /* SELinux */
        "/sys/fs/selinux/enforce",
        "/sys/fs/selinux/checkreqprot",
        "/sys/fs/selinux/policy",
        "/sys/fs/selinux/load",
        /* 内核信息泄露 */
        "/proc/kallsyms",
        "/proc/kcore",
        "/proc/vmallocinfo",
        "/proc/modules",
        "/sys/module/kernel/sections/.text",
        /* 内存/设备 */
        "/dev/mem",
        "/dev/kmem",
        "/dev/port",
        "/dev/block/mmcblk0",
        "/dev/block/mmcblk0p12",          /* boot */
        "/dev/block/mmcblk0p11",          /* recovery */
        "/dev/block/mmcblk0p27",          /* 数据区(路由器那张卡无关,这里是电视) */
        "/dev/block/by-name/boot",
        "/dev/block/by-name/system",
        "/dev/block/by-name/vendor",
        /* 厂商节点 */
        "/dev/mstar_mma",
        "/dev/mma",
        "/dev/utopia",
        "/proc/mstar_dvfs",
        /* 文件系统 */
        "/system/build.prop",
        "/vendor/build.prop",
        "/system/app/TvService/TvService.apk",
        "/vendor/bin/misysdiagnose",
        "/data/diagnosis/command.sh",
        "/data/local/tmp",
        "/sdcard",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        int r = open(paths[i], O_RDONLY);
        int re = errno;
        if (r >= 0) close(r);
        int w = open(paths[i], O_WRONLY);
        int we = errno;
        if (w >= 0) close(w);
        printf("%-42s R=%-3s W=%-3s", paths[i], r >= 0 ? "OK" : "no", w >= 0 ? "OK" : "no");
        if (r < 0) printf("  [R:%s]", strerror(re));
        if (w < 0) printf("  [W:%s]", strerror(we));
        printf("\n");
    }
    return 0;
}
