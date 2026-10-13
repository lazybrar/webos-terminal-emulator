/* freestanding ARM32 Linux: list wayland globals. no libc. */
typedef unsigned int u32;
static long sc(long n, long a, long b, long c) {
    register long r7 __asm__("r7") = n, r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = c; return d; }
static void out(const char *s, int n) { sc(4, 1, (long)s, n); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { out(s, slen(s)); }
static void putu(u32 v) { char b[12]; int i = 12; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i, 12 - i); }

void _start_c(long *sp) {
    const char *dir = 0, *name = "wayland-0";
    long argc = sp[0]; char **env = (char **)(sp + argc + 2);
    for (; *env; env++) {
        const char *e = *env;
        if (e[0]=='X'&&e[1]=='D'&&e[2]=='G'&&e[3]=='_'&&e[4]=='R'&&e[5]=='U'&&e[6]=='N'&&e[7]=='T'&&e[8]=='I'&&e[9]=='M'&&e[10]=='E'&&e[11]=='_'&&e[12]=='D'&&e[13]=='I'&&e[14]=='R'&&e[15]=='=') dir = e + 16;
    }
    if (!dir) dir = "/tmp/xdg";
    struct { unsigned short fam; char path[108]; } a = {1, {0}};
    int n = 0; for (; dir[n]; n++) a.path[n] = dir[n];
    a.path[n++] = '/';
    for (int i = 0; name[i]; i++) a.path[n++] = name[i];
    int fd = sc(281, 1, 1, 0);                       /* socket(AF_UNIX,SOCK_STREAM) */
    if (sc(283, fd, (long)&a, 2 + n + 1) < 0) { puts_("connect failed\n"); sc(1, 1, 0, 0); }
    u32 req[6] = {1, (12u << 16) | 1, 2,             /* display.get_registry(new_id=2) */
                  1, (12u << 16) | 0, 3};            /* display.sync(new_id=3) */
    sc(4, fd, (long)req, sizeof req);
    static u32 buf[16384]; int have = 0;
    for (;;) {
        long r = sc(3, fd, (long)buf + have, sizeof buf - have);
        if (r <= 0) break;
        have += r;
        int off = 0;
        while (have - off >= 8) {
            u32 id = buf[off/4], sz = buf[off/4 + 1] >> 16, op = buf[off/4 + 1] & 0xffff;
            if (have - off < (int)sz) break;
            u32 *p = &buf[off/4 + 2];
            if (id == 2 && op == 0) {
                u32 slen_ = p[1]; const char *s = (const char *)&p[2];
                u32 ver = p[2 + ((slen_ + 3) / 4)];
                putu(p[0]); puts_("\t"); puts_(s); puts_("\tv"); putu(ver); puts_("\n");
            } else if (id == 3) { sc(1, 1, 0, 0); }
            else if (id == 1 && op == 0) { puts_("error from compositor\n"); }
            off += sz;
        }
        for (int i = 0; i < have - off; i++) ((char *)buf)[i] = ((char *)buf)[off + i];
        have -= off;
    }
    sc(1, 0, 0, 0);
}
__asm__(".globl _start\n_start:\n mov r0, sp\n bl _start_c\n");
