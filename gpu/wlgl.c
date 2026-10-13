/* wlgl: GLES2 triangle on the TV's PowerVR via the TV's own libEGL/libwayland-egl.
   32-bit ARM, links dynamically against the TV libs, no libc. */
typedef unsigned int u32; typedef int i32; typedef void *ptr;
extern ptr wl_display_connect(const char *), wl_proxy_marshal_constructor(ptr, u32, const void *, ...),
    wl_proxy_marshal_constructor_versioned(ptr, u32, const void *, u32, ...);
extern int wl_display_roundtrip(ptr), wl_display_dispatch_pending(ptr), wl_display_flush(ptr), wl_proxy_add_listener(ptr, const void *, ptr);
extern void wl_proxy_marshal(ptr, u32, ...);
extern const char wl_registry_interface[], wl_compositor_interface[], wl_surface_interface[], wl_webos_shell_interface[], wl_webos_shell_surface_interface[];
extern ptr wl_egl_window_create(ptr, int, int);
extern ptr eglGetDisplay(ptr), eglCreateContext(ptr, ptr, ptr, const i32 *), eglCreateWindowSurface(ptr, ptr, ptr, const i32 *);
extern u32 eglInitialize(ptr, i32 *, i32 *), eglBindAPI(u32), eglChooseConfig(ptr, const i32 *, ptr *, i32, i32 *),
    eglMakeCurrent(ptr, ptr, ptr, ptr), eglSwapBuffers(ptr, ptr), eglSwapInterval(ptr, i32), eglGetError(void);
extern void glClearColor(float, float, float, float), glClear(u32), glViewport(i32, i32, i32, i32), glShaderSource(u32, i32, const char *const *, const i32 *),
    glCompileShader(u32), glAttachShader(u32, u32), glLinkProgram(u32), glUseProgram(u32), glVertexAttribPointer(u32, i32, u32, unsigned char, i32, const void *),
    glEnableVertexAttribArray(u32), glDrawArrays(u32, i32, i32), glUniform1f(i32, float), glBindAttribLocation(u32, u32, const char *);
extern u32 glCreateShader(u32), glCreateProgram(void);
extern i32 glGetUniformLocation(u32, const char *);

static long sc(long n, long a, long b, long c) {
    register long r7 __asm__("r7") = n, r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void say(const char *s) { sc(4, 2, (long)s, sl(s)); }
static void sayn(const char *s, u32 v) { char b[12]; int i = 12; say(s); do { b[--i] = '0' + v % 10; v /= 10; } while (v); sc(4, 2, (long)b + i, 12 - i); say("\n"); }

static ptr comp, shell;
struct iface { const char *name; i32 version; };
static void on_global(ptr data, ptr reg, u32 name, const char *ifc, u32 ver) {
    (void)data;
    if (eq(ifc, "wl_compositor")) comp = wl_proxy_marshal_constructor_versioned(reg, 0, wl_compositor_interface, 3, name, ifc, 3, 0);
    else if (eq(ifc, "wl_webos_shell")) shell = wl_proxy_marshal_constructor_versioned(reg, 0, wl_webos_shell_interface, 1, name, ifc, 1, 0);
    (void)ver;
}
static void on_remove(ptr d, ptr r, u32 n) { (void)d; (void)r; (void)n; }
static const void *reg_listener[2] = {(void *)on_global, (void *)on_remove};

static float fsin(float x) {                          /* range-reduce + 5th order */
    const float pi = 3.14159265f; while (x > pi) x -= 2 * pi; while (x < -pi) x += 2 * pi;
    float s = x < 0 ? -1.f : 1.f; if (x < 0) x = -x; if (x > pi / 2) x = pi - x;
    float x2 = x * x; return s * x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f)));
}
static float fcos(float x) { return fsin(x + 1.57079633f); }
static u32 mk(u32 type, const char *src) { u32 s = glCreateShader(type); glShaderSource(s, 1, &src, 0); glCompileShader(s); return s; }

void _start_c(long *sp) {
    (void)sp;
    ptr dpy = wl_display_connect(0); if (!dpy) { say("no wayland\n"); sc(1, 1, 0, 0); }
    ptr reg = wl_proxy_marshal_constructor(dpy, 1, wl_registry_interface, 0);
    wl_proxy_add_listener(reg, reg_listener, 0); wl_display_roundtrip(dpy);
    if (!comp || !shell) { say("missing globals\n"); sc(1, 2, 0, 0); }
    ptr surf = wl_proxy_marshal_constructor(comp, 0, wl_surface_interface, 0);
    ptr ss = wl_proxy_marshal_constructor(shell, 1, wl_webos_shell_surface_interface, 0, surf);
    wl_proxy_marshal(ss, 2, "appId", "org.tvwl.wlgl"); wl_proxy_marshal(ss, 2, "_WEBOS_WINDOW_TYPE", "_WEBOS_WINDOW_TYPE_CARD");
    wl_proxy_marshal(ss, 2, "instanceId", "org.tvwl.wlgl"); wl_proxy_marshal(ss, 1, 3);
    const int W = 1920, H = 1080;
    ptr win = wl_egl_window_create(surf, W, H);
    ptr ed = eglGetDisplay(dpy); i32 mj, mn; if (!eglInitialize(ed, &mj, &mn)) { sayn("eglInitialize err ", eglGetError()); sc(1, 3, 0, 0); }
    eglBindAPI(0x30A0);
    static const i32 ca[] = {0x3033, 4, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    ptr cfg; i32 nc = 0; eglChooseConfig(ed, ca, &cfg, 1, &nc); if (!nc) { say("no config\n"); sc(1, 4, 0, 0); }
    static const i32 xa[] = {0x3098, 2, 0x3038};
    ptr ctx = eglCreateContext(ed, cfg, 0, xa); ptr es = eglCreateWindowSurface(ed, cfg, win, 0);
    if (!ctx || !es) { sayn("ctx/surface err ", eglGetError()); sc(1, 5, 0, 0); }
    eglMakeCurrent(ed, es, es, ctx); eglSwapInterval(ed, 1);
    u32 vs = mk(0x8B31, "attribute vec2 p; attribute vec3 c; varying vec3 v; uniform float t;"
                        "void main(){float s=sin(t),k=cos(t);gl_Position=vec4(p.x*k-p.y*s,(p.x*s+p.y*k)*1.7778,0.,1.);v=c;}");
    u32 fs = mk(0x8B30, "precision mediump float; varying vec3 v; void main(){gl_FragColor=vec4(v,1.);}");
    u32 pr = glCreateProgram(); glAttachShader(pr, vs); glAttachShader(pr, fs);
    glBindAttribLocation(pr, 0, "p"); glBindAttribLocation(pr, 1, "c"); glLinkProgram(pr); glUseProgram(pr);
    i32 ut = glGetUniformLocation(pr, "t");
    static const float vtx[] = {0.f, 0.6f, 1.f, 0.f, 0.f, -0.55f, -0.4f, 0.f, 1.f, 0.f, 0.55f, -0.4f, 0.f, 0.f, 1.f};
    glVertexAttribPointer(0, 2, 0x1406, 0, 20, vtx); glVertexAttribPointer(1, 3, 0x1406, 0, 20, (const char *)vtx + 8);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glViewport(0, 0, W, H);
    (void)fcos;
    long ts[2]; sc(263, 1, (long)ts, 0); long t0 = ts[0] * 1000 + ts[1] / 1000000; long frames = 0;
    for (;;) {
        sc(263, 1, (long)ts, 0); long ms = ts[0] * 1000 + ts[1] / 1000000 - t0;
        float t = ms * 0.002f;
        glClearColor(0.05f + 0.05f * fsin(t), 0.05f, 0.1f, 1.f); glClear(0x4000);
        glUniform1f(ut, t); glDrawArrays(4, 0, 3);
        eglSwapBuffers(ed, es); wl_display_dispatch_pending(dpy); wl_display_flush(dpy);
        if (++frames % 300 == 0) sayn("frames ", frames);
        if (ms > 120000) break;
    }
    sc(1, 0, 0, 0);
}
__asm__(".globl _start\n_start:\n mov r0, sp\n bic sp, sp, #7\n bl _start_c\n");
