// system_app 域探测程序 (armv7a)  v2
// 编译: armv7a-linux-androideabi21-clang.cmd -O2 -o saprobe saprobe.c
// 输出: /data/data/com.mediatek.tv.factory/probe_out.txt  (misysdiagnose/uid0 可读)
// 用法: saprobe [safe|mmap|bf]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <dirent.h>

static FILE *G;
#define P(...) do { fprintf(G, __VA_ARGS__); fflush(G); } while (0)
#define OUT "/data/data/com.mediatek.tv.factory/probe_out.txt"

static void show_ctx(void) {
    char b[256]; int f = open("/proc/self/attr/current", O_RDONLY);
    if (f >= 0) { int n = read(f, b, 255); if (n > 0) { b[n] = 0; P("ctx    = %s", b); } close(f); }
    P("uid=%d euid=%d gid=%d\n", getuid(), geteuid(), getgid());
}

static void dump_file(const char *path, int maxlen) {
    int f = open(path, O_RDONLY);
    if (f < 0) { P("  READ %-42s FAIL %s\n", path, strerror(errno)); return; }
    char buf[600]; int n = read(f, buf, maxlen < 599 ? maxlen : 599);
    if (n < 0) P("  READ %-42s FAILREAD %s\n", path, strerror(errno));
    else { buf[n] = 0; for (int i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '|'; P("  READ %-42s OK: %.260s\n", path, buf); }
    close(f);
}

int main(int argc, char **argv) {
    G = fopen(OUT, "w");
    if (!G) G = fopen("/data/user/0/com.mediatek.tv.factory/probe_out.txt", "w");
    if (!G) return 1;
    const char *mode = (argc > 1) ? argv[1] : "safe";
    P("[saprobe v2] mode=%s\n", mode);
    show_ctx();

    P("\n--- /proc/cmdline ---\n");
    dump_file("/proc/cmdline", 2000);

    P("\n--- /proc/utopia ---\n");
    {
        int f = open("/proc/utopia", O_RDWR);
        P("  open(RW)=%d %s\n", f, f < 0 ? strerror(errno) : "OK");
        if (f >= 0) {
            char b[512]; errno = 0; int n = read(f, b, sizeof(b) - 1);
            P("  read=%d %s\n", n, n < 0 ? strerror(errno) : "");
            close(f);
        }
    }

    P("\n--- debugfs / sysfs ---\n");
    const char *F[] = { "/sys/kernel/debug/memblock", "/sys/kernel/debug/mma_meminfo",
                        "/sys/kernel/debug/gpu_memory", "/sys/kernel/debug/miomap",
                        "/sys/kernel/debug/ion", "/sys/kernel/debug/ion/heaps",
                        "/sys/kernel/debug/mma", "/proc/vmallocinfo", "/proc/iomem", NULL };
    for (int i = 0; F[i]; i++) dump_file(F[i], 400);

    P("\n--- 设备 open 测试 ---\n");
    const char *D[] = { "/dev/miomap", "/dev/malloc", "/dev/system", "/dev/msmailbox",
                        "/dev/mtal", "/dev/semutex", "/dev/scaler", "/dev/mma", "/dev/ion",
                        "/dev/mali0", "/dev/mik!sys", "/dev/mik!flash", "/dev/mik!os",
                        "/dev/mik!disp", "/dev/mik!gpio", "/dev/eeprom_0", "/dev/MPOOL", NULL };
    for (int i = 0; D[i]; i++) {
        errno = 0; int fd = open(D[i], O_RDWR);
        P("  %-20s RW=%d %s\n", D[i], fd, fd < 0 ? strerror(errno) : "OK");
        if (fd < 0) { errno = 0; fd = open(D[i], O_RDONLY); P("  %-20s RO=%d %s\n", D[i], fd, fd < 0 ? strerror(errno) : "OK"); }
        if (fd >= 0) close(fd);
    }

    if (strcmp(mode, "safe") == 0) { P("\n[safe 模式结束]\n"); fclose(G); return 0; }

    P("\n--- mmap 测试 (不读映射内容) ---\n");
    for (int i = 0; D[i]; i++) {
        errno = 0; int fd = open(D[i], O_RDWR);
        if (fd < 0) continue;
        for (int k = 0; k < 2; k++) {
            unsigned long prot = k ? PROT_READ : (PROT_READ | PROT_WRITE);
            void *p = mmap(NULL, 0x1000, prot, MAP_SHARED, fd, 0);
            P("  mmap %-16s prot=%lu -> %s\n", D[i], prot, p == MAP_FAILED ? strerror(errno) : "OK");
            if (p != MAP_FAILED) munmap(p, 0x1000);
        }
        close(fd);
    }

    if (strcmp(mode, "bf") == 0) {
        P("\n--- ioctl 暴力扫描 (低 64 个裸号) ---\n");
        for (int i = 0; D[i]; i++) {
            errno = 0; int fd = open(D[i], O_RDWR);
            if (fd < 0) continue;
            P("  [%s]\n", D[i]);
            unsigned char out[256];
            for (int code = 0; code < 0x40; code++) {
                memset(out, 0, sizeof(out)); errno = 0;
                int r = ioctl(fd, code, out);
                if (r >= 0 || (errno != EINVAL && errno != ENOTTY && errno != ENOSYS))
                    P("    ioctl(%#x)=%d errno=%d(%s) o=%02x%02x%02x%02x\n", code, r, errno, strerror(errno), out[0], out[1], out[2], out[3]);
            }
            close(fd);
        }
    }
    P("\n[done]\n");
    fclose(G);
    return 0;
}
