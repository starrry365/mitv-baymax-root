/*
 * Verify the primitives CVE-2021-0920 needs, inside misysdiagnose context.
 *  1. socketpair(AF_UNIX)            - no filesystem path involved
 *  2. bind() to ABSTRACT namespace   - no filesystem path involved
 *  3. sendmsg/recvmsg + SCM_RIGHTS   - fd passing (core of the bug)
 *  4. recvmsg(MSG_PEEK)              - the actual trigger
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>

int main(void)
{
    FILE *f = fopen("/sdcard/unix2.txt", "w");
    if (!f) return 1;
    setbuf(f, NULL);

    /* 1. socketpair */
    int sv[2];
    errno = 0;
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fprintf(f, "1. socketpair(AF_UNIX,SOCK_STREAM) -> %s", r == 0 ? "OK" : "FAIL");
    if (r) fprintf(f, " errno=%d (%s)", errno, strerror(errno));
    fprintf(f, "\n");

    /* 2. abstract namespace bind */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    const char *name = "mitv_pwn_abstract";
    un.sun_path[0] = '\0';
    strcpy(un.sun_path + 1, name);
    socklen_t len = (socklen_t)(sizeof(sa_family_t) + 1 + strlen(name));
    errno = 0;
    int r2 = bind(s, (struct sockaddr *)&un, len);
    fprintf(f, "2. bind(ABSTRACT)                  -> %s", r2 == 0 ? "OK" : "FAIL");
    if (r2) fprintf(f, " errno=%d (%s)", errno, strerror(errno));
    fprintf(f, "\n");
    errno = 0;
    fprintf(f, "   listen()                        -> %s",
            listen(s, 4) == 0 ? "OK" : "FAIL");
    fprintf(f, " errno=%d\n", errno);

    /* 3. SCM_RIGHTS fd passing over socketpair */
    if (r == 0) {
        struct msghdr msg;
        struct cmsghdr *cmsg;
        char cbuf[CMSG_SPACE(sizeof(int))];
        char buf[CMSG_SPACE(sizeof(int))];
        struct iovec iov;
        char data = 'X';

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = &data; iov.iov_len = 1;
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        *((int *)CMSG_DATA(cmsg)) = s;      /* pass the abstract-bound socket */

        errno = 0;
        ssize_t n = sendmsg(sv[0], &msg, 0);
        fprintf(f, "3. sendmsg(SCM_RIGHTS)             -> %s (%zd bytes)",
                n > 0 ? "OK" : "FAIL", n);
        if (n < 0) fprintf(f, " errno=%d (%s)", errno, strerror(errno));
        fprintf(f, "\n");

        /* receive it back */
        memset(&msg, 0, sizeof(msg));
        memset(buf, 0, sizeof(buf));
        iov.iov_base = &data; iov.iov_len = 1;
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = buf; msg.msg_controllen = sizeof(buf);
        errno = 0;
        n = recvmsg(sv[1], &msg, 0);
        fprintf(f, "   recvmsg(SCM_RIGHTS)             -> %s (%zd bytes)",
                n > 0 ? "OK" : "FAIL", n);
        if (n < 0) fprintf(f, " errno=%d (%s)", errno, strerror(errno));
        fprintf(f, "\n");
        struct cmsghdr *rc = CMSG_FIRSTHDR(&msg);
        fprintf(f, "   got cmsg: level=%d type=%d -> fd=%d\n",
                rc ? rc->cmsg_level : -1,
                rc ? rc->cmsg_type : -1,
                rc ? *((int *)CMSG_DATA(rc)) : -1);
    }

    /* 4. MSG_PEEK recvmsg (the CVE-2021-0920 trigger) */
    if (r == 0) {
        char d2[16];
        struct iovec iov2 = { d2, sizeof(d2) };
        struct msghdr m2;
        memset(&m2, 0, sizeof(m2));
        m2.msg_iov = &iov2; m2.msg_iovlen = 1;
        write(sv[0], "hello", 5);
        errno = 0;
        ssize_t n = recvmsg(sv[1], &m2, MSG_PEEK);
        fprintf(f, "4. recvmsg(MSG_PEEK)               -> %s (%zd bytes: '%.*s')",
                n > 0 ? "OK" : "FAIL", n, (int)(n > 0 ? n : 0), d2);
        if (n < 0) fprintf(f, " errno=%d (%s)", errno, strerror(errno));
        fprintf(f, "\n");
    }

    fclose(f);
    return 0;
}
