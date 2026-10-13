/* freestanding ARM32: wl_shm + wl_shell toplevel showing a gradient. no libc. */
typedef unsigned int u32;
static long sc(long n, long a, long b, long c) {
    register long r7 __asm__("r7") = n, r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
enum { W = 1280, H = 720 };
static u32 m[256];
static void msg(u32 id, u32 op, int n, const u32 *args) {   /* n words of args */
    m[0] = id; m[1] = ((8 + 4 * n) << 16) | op; for (int i = 0; i < n; i++) m[2 + i] = args[i];
    sc(4, 3, (long)m, 8 + 4 * n);
}
static int fd = 3;
static u32 bind_(u32 name, const char *ifc, u32 ver, u32 nid) {
    u32 b[16]; int l = sl(ifc) + 1, w = (l + 3) / 4, n = 0;
    b[n++] = name; b[n++] = l; memset(&b[n], 0, w * 4); memcpy(&b[n], ifc, l); n += w; b[n++] = ver; b[n++] = nid;
    msg(2, 0, n, b); return nid;
}
static int readev(u32 *buf, int *have, int (*cb)(u32 id, u32 op, u32 *p, u32 sz)) {
    long r = sc(3, fd, (long)buf + *have, 8192 * 4 - *have); if (r <= 0) return -1;
    *have += r; int off = 0;
    while (*have - off >= 8) {
        u32 id = buf[off/4], sz = buf[off/4+1] >> 16, op = buf[off/4+1] & 0xffff;
        if (*have - off < (int)sz) break;
        if (cb(id, op, &buf[off/4+2], sz)) return 1;
        off += sz;
    }
    for (int i = 0; i < *have - off; i++) ((char *)buf)[i] = ((char *)buf)[off + i];
    *have -= off; return 0;
}
static u32 g_comp, g_shm, g_shell;
static void setprop(const char *k, const char *v) {
    u32 b[40]; int n = 0, l = sl(k) + 1, w = (l + 3) / 4;
    b[n++] = l; memset(&b[n], 0, w * 4); memcpy(&b[n], k, l); n += w;
    l = sl(v) + 1; w = (l + 3) / 4; b[n++] = l; memset(&b[n], 0, w * 4); memcpy(&b[n], v, l); n += w;
    msg(8, 2, n, b);
}

static int cb_reg(u32 id, u32 op, u32 *p, u32 sz) {
    if (id == 2 && op == 0) {
        const char *s = (const char *)&p[2]; u32 ver = p[2 + (p[1] + 3) / 4];
        if (eq(s, "wl_compositor")) g_comp = p[0];
        else if (eq(s, "wl_shm")) g_shm = p[0];
        else if (eq(s, "wl_webos_shell")) g_shell = p[0];
        (void)ver;
    }
    return id == 3;
}
static int cb_run(u32 id, u32 op, u32 *p, u32 sz) {
    (void)id; (void)op; (void)p;
    return 0;
}
void _start_c(long *sp) {
    const char *dir = "/tmp/xdg";
    long argc = sp[0]; char **env = (char **)(sp + argc + 2);
    for (; *env; env++) { const char *e = *env;
        if (e[0]=='X'&&e[1]=='D'&&e[2]=='G'&&e[3]=='_'&&e[4]=='R'&&e[5]=='U'&&e[6]=='N'&&e[7]=='T'&&e[8]=='I'&&e[9]=='M'&&e[10]=='E'&&e[11]=='_'&&e[12]=='D'&&e[13]=='I'&&e[14]=='R'&&e[15]=='=') dir = e + 16; }
    struct { unsigned short fam; char path[108]; } a = {1, {0}};
    int n = 0; for (; dir[n]; n++) a.path[n] = dir[n];
    const char *nm = "/wayland-0"; for (int i = 0; nm[i]; i++) a.path[n++] = nm[i];
    fd = sc(281, 1, 1, 0);
    if (sc(283, fd, (long)&a, 2 + n + 1) < 0) sc(1, 1, 0, 0);
    { u32 r = 2; msg(1, 1, 1, &r); u32 c = 3; msg(1, 0, 1, &c); }
    static u32 buf[8192]; int have = 0, r;
    while (!(r = readev(buf, &have, cb_reg))) ;
    if (!g_comp || !g_shm || !g_shell) sc(1, 2, 0, 0);
    bind_(g_comp, "wl_compositor", 3, 4); bind_(g_shm, "wl_shm", 1, 5); bind_(g_shell, "wl_webos_shell", 1, 6);
    { u32 x = 7; msg(4, 0, 1, &x); }                              /* create_surface */
    { u32 x[2] = {8, 7}; msg(6, 1, 2, x); }
    setprop("appId", "org.tvwl.wlwin"); setprop("_WEBOS_WINDOW_TYPE", "_WEBOS_WINDOW_TYPE_CARD"); setprop("instanceId", "org.tvwl.wlwin");                       /* get_shell_surface */
    /* shm pool from memfd */
    int mf = sc(385, (long)"wlwin", 0, 0); sc(93, mf, W * H * 4, 0);
    static u32 row[W];
    for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) row[x] = 0xff000000 | ((x * 255 / W) << 16) | ((y * 255 / H) << 8) | 0x60; sc(4, mf, (long)row, sizeof row); }
    { u32 h[3] = {5, (8 + 12) << 16 | 0, 9}; u32 sz = W * H * 4; h[1] = (16u << 16) | 0;
      u32 pk[5] = {h[0], h[1], h[2], sz, 0};
      struct { void *b; u32 bl; } iov = {pk, 16};
      struct { u32 c0; u32 level, type; int fd; } cm = {16, 1, 1, mf};
      struct { void *nm; u32 nl; void *iov; u32 il; void *c; u32 cl; u32 fl; } mh = {0, 0, &iov, 1, &cm, 16, 0};
      sc(296, fd, (long)&mh, 0); }
    { u32 x[6] = {10, 0, W, H, W * 4, 0}; msg(9, 0, 6, x); }      /* create_buffer argb8888 */
    { u32 st = 3; msg(8, 1, 1, &st); }
    { u32 x[3] = {10, 0, 0}; msg(7, 1, 3, x); }                   /* attach */
    { u32 x[4] = {0, 0, W, H}; msg(7, 2, 4, x); }                 /* damage */
    msg(7, 6, 0, 0);                                              /* commit */
    while (!(r = readev(buf, &have, cb_run))) ;
    sc(1, 0, 0, 0);
}
__asm__(".globl _start\n_start:\n mov r0, sp\n bl _start_c\n");
