/* mma2.c - 刻画 /dev/mma: fstat + read + mmap + 无害 ioctl 探测 */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>

int main(void) {
    int fd = open("/dev/mma", O_RDWR);
    if (fd < 0) { printf("open /dev/mma O_RDWR: %s\n", strerror(errno)); return 1; }
    struct stat st; memset(&st, 0, sizeof st);
    fstat(fd, &st);
    printf("/dev/mma char major=%d minor=%d mode=%06o\n", major(st.st_rdev), minor(st.st_rdev), st.st_mode);

    /* read 探测 */
    unsigned char buf[32];
    memset(buf, 0, sizeof buf);
    errno = 0;
    int n = read(fd, buf, 16);
    printf("read(16) -> %d errno=%d(%s)\n", n, errno, strerror(errno));
    if (n > 0) { printf("  data:"); for (int i = 0; i < n; i++) printf(" %02x", buf[i]); printf("\n"); }

    /* mmap 探测 */
    errno = 0;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    printf("mmap(4096,SHARED,off=0) -> %p errno=%d(%s)\n", m, errno, strerror(errno));
    if (m != MAP_FAILED) munmap(m, 4096);

    /* ioctl 探测: 扫描 type 0x00-0xff, nr 0-3，只记录非 ENOTTY 的 */
    printf("\n---- ioctl 扫描 (只记非 ENOTTY) ----\n");
    int hits = 0;
    for (int t = 0; t < 256; t++) {
        for (int nr = 0; nr < 4; nr++) {
            errno = 0;
            int r = ioctl(fd, (t << 8) | nr, 0);
            if (errno == ENOTTY) continue;
            printf("  cmd=0x%04x (type=%02x nr=%d) -> r=%d errno=%d(%s)\n",
                   (t << 8) | nr, t, nr, r, errno, strerror(errno));
            if (++hits > 40) goto done;
        }
    }
done:
    if (!hits) printf("  (全部 ENOTTY —— ioctl 未被识别)\n");
    close(fd);
    return 0;
}
