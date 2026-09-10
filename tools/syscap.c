/* syscap.c - 决定性系统调用能力普查：确定哪些内核接口对我们的域开放 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#ifndef __NR_perf_event_open
#define __NR_perf_event_open 364
#endif
#ifndef __NR_userfaultfd
#define __NR_userfaultfd 388
#endif
#ifndef __NR_memfd_create
#define __NR_memfd_create 385
#endif
#ifndef __NR_bpf
#define __NR_bpf 386
#endif
#ifndef __NR_kexec_load
#define __NR_kexec_load 347
#endif
#ifndef __NR_reboot
#define __NR_reboot 88
#endif
#ifndef __NR_finit_module
#define __NR_finit_module 379
#endif
#ifndef __NR_add_key
#define __NR_add_key 309
#endif
#ifndef __NR_keyctl
#define __NR_keyctl 311
#endif
#ifndef __NR_process_vm_readv
#define __NR_process_vm_readv 376
#endif
#ifndef __NR_setns
#define __NR_setns 375
#endif
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

static const struct { int fam; const char *n; } fams[] = {
    {1,"AF_UNIX"},{2,"AF_INET"},{10,"AF_INET6"},{16,"AF_NETLINK"},{17,"AF_PACKET"},
    {26,"AF_LLC"},{27,"AF_IB"},{28,"AF_MPLS"},{29,"AF_CAN"},{30,"AF_TIPC"},
    {31,"AF_BLUETOOTH"},{33,"AF_RXRPC"},{38,"AF_ALG"},{39,"AF_NFC"},{40,"AF_VSOCK"},
    {41,"AF_KCM"},{43,"AF_SMC"},{44,"AF_XDP"},{45,"AF_MCTP"},{0,NULL}
};
static const int types[] = {1 /*STREAM*/, 2 /*DGRAM*/, 3 /*RAW*/, 5 /*SEQPACKET*/, 0};

int main(void) {
    printf("===== A. socket() 家族可用性 =====\n");
    for (int i = 0; fams[i].n; i++) {
        printf("  %-14s:", fams[i].n);
        for (int j = 0; types[j]; j++) {
            errno = 0;
            int s = socket(fams[i].fam, types[j], 0);
            printf(" t%d=%s", types[j], s >= 0 ? "OK" : "no");
            if (s >= 0) close(s);
            else if (errno != EAFNOSUPPORT && errno != EPROTONOSUPPORT && errno != ESOCKTNOSUPPORT)
                printf("(%s)", strerror(errno));
        }
        printf("\n");
    }

    printf("\n===== B. 关键系统调用 =====\n");
    int r; long lr;
    errno = 0; lr = syscall(__NR_memfd_create, "x", 0);
    printf("  memfd_create       -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");

    errno = 0; lr = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    printf("  userfaultfd        -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");
    if (lr >= 0) close((int)lr);

    { struct { unsigned int type, size; unsigned char rest[112]; } attr;
      memset(&attr, 0, sizeof attr); attr.type = 0; attr.size = sizeof attr;
      errno = 0; lr = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
      printf("  perf_event_open    -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");
      if (lr >= 0) close((int)lr); }

    { struct { unsigned int map_type, key_size, value_size, max_entries, flags; } a;
      memset(&a, 0, sizeof a); a.map_type = 1; a.key_size = 4; a.value_size = 4; a.max_entries = 1;
      errno = 0; lr = syscall(__NR_bpf, 0 /*MAP_CREATE*/, &a, sizeof a);
      printf("  bpf(MAP_CREATE)    -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");
      if (lr >= 0) close((int)lr); }

    errno = 0; lr = syscall(__NR_kexec_load, 0UL, 0UL, 0UL, 0UL);
    printf("  kexec_load         -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");

    errno = 0; lr = syscall(__NR_reboot, 0x00000000UL, 0x00000000UL, 0UL, 0UL);
    printf("  reboot(bad magic)  -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");

    errno = 0; r = open("/dev/null", O_RDONLY);
    lr = syscall(__NR_finit_module, r, "", 0);
    printf("  finit_module       -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");
    close(r);

    errno = 0; lr = syscall(__NR_add_key, "user", "k", "v", 1, -2 /*KEY_SPEC_PROCESS_KEYRING*/);
    printf("  add_key            -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");

    { char b[16]; struct iovec li = { b, 4 }, ri = { b, 4 };
      errno = 0; lr = syscall(__NR_process_vm_readv, getpid(), &li, 1, &ri, 1, 0);
      printf("  process_vm_readv   -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK"); }

    errno = 0; lr = syscall(__NR_io_uring_setup, 2, (void *)0);
    printf("  io_uring_setup     -> %ld (%s)\n", lr, lr < 0 ? strerror(errno) : "OK");

    printf("\n===== C. 改自身 SELinux 上下文 =====\n");
    r = open("/proc/self/attr/current", O_WRONLY);
    printf("  open attr/current O_WRONLY -> %d (%s)\n", r, r < 0 ? strerror(errno) : "OK");
    if (r >= 0) { int w = write(r, "u:r:shell:s0", 12); printf("  write ctx -> %d (%s)\n", w, w < 0 ? strerror(errno) : "OK"); close(r); }

    printf("\n===== D. prctl / mmap 能力 =====\n");
    errno = 0; r = prctl(PR_SET_MM, 0, 0, 0, 0);
    printf("  prctl(PR_SET_MM)   -> %d (%s)\n", r, r < 0 ? strerror(errno) : "OK");
    void *p = mmap((void *)0x10000, 4096, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("  mmap MAP_FIXED low -> %p (%s)\n", p, p == MAP_FAILED ? strerror(errno) : "OK");

    printf("\n===== E. /dev/mma 专项: 探测 0x0001/0x0002 参数需求 =====\n");
    int fd = open("/dev/mma", O_RDWR);
    if (fd >= 0) {
        char zbuf[64]; memset(zbuf, 0, sizeof zbuf);
        for (int c = 1; c <= 2; c++) {
            errno = 0; r = ioctl(fd, c, zbuf);
            printf("  ioctl(0x%04x, buf) -> %d (%s)\n", c, r, r < 0 ? strerror(errno) : "OK");
            errno = 0; r = ioctl(fd, c, 0);
            printf("  ioctl(0x%04x, NULL)-> %d (%s)\n", c, r, r < 0 ? strerror(errno) : "OK");
        }
        close(fd);
    } else printf("  open /dev/mma failed\n");
    return 0;
}
