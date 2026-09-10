#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/stat.h>

int main(void)
{
    FILE *f = fopen("/sdcard/socktest.txt", "w");
    if (!f) return 1;
    setbuf(f, NULL);

    fprintf(f, "uid=%d  pid=%d\n", getuid(), getpid());
    {
        char ctx[256] = {0};
        FILE *c = fopen("/proc/self/attr/current", "r");
        if (c) { if (fgets(ctx, sizeof(ctx), c)) {} fclose(c); }
        fprintf(f, "selinux=%s", ctx);
        /* ctx 可能没有换行 */
        fprintf(f, "\n");
    }

    struct { int fam; const char *name; } fams[] = {
        {AF_UNIX, "AF_UNIX"},
        {AF_INET, "AF_INET"},
#ifndef AF_INET6
#define AF_INET6 10
#endif
        {AF_INET6, "AF_INET6"},
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
        {AF_NETLINK, "AF_NETLINK"},
#ifndef AF_PACKET
#define AF_PACKET 17
#endif
        {AF_PACKET, "AF_PACKET"},
    };
    int types[] = { SOCK_STREAM, SOCK_DGRAM };
    const char *tnames[] = { "STREAM", "DGRAM" };

    for (unsigned i = 0; i < sizeof(fams)/sizeof(fams[0]); i++) {
        for (unsigned j = 0; j < 2; j++) {
            errno = 0;
            int s = socket(fams[i].fam, types[j], 0);
            fprintf(f, "  socket(%-10s, %-7s) -> %s",
                    fams[i].name, tnames[j], s >= 0 ? "OK" : "FAIL");
            if (s < 0) fprintf(f, "  errno=%d (%s)", errno, strerror(errno));
            fprintf(f, "\n");
            if (s >= 0) close(s);
        }
    }

    /* AF_UNIX bind/listen 能力 —— CVE-2021-0920 的必要条件 */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        strcpy(un.sun_path, "/data/diagnosis/.socktest");
        unlink("/data/diagnosis/.socktest");
        errno = 0;
        fprintf(f, "  AF_UNIX bind  -> %s",
                bind(s, (struct sockaddr*)&un, sizeof(un)) == 0 ? "OK" : "FAIL");
        fprintf(f, " errno=%d\n", errno);
        fprintf(f, "  AF_UNIX listen-> %s\n", listen(s, 5) == 0 ? "OK" : "FAIL");
        close(s);
        unlink("/data/diagnosis/.socktest");
    }

    fclose(f);
    return 0;
}
