/* GPU front-end for wlterm2 (compile with -DWLTERM_GL): 32-bit ARM, TV's libwayland/EGL/GLES2 (PowerVR).
   Glyphs are rasterised once by stb_truetype into cell-sized slots of an alpha atlas texture; every frame the whole grid is drawn as
   one indexed quad list (background, glyph coverage, underline/strike are computed in the fragment shader). Included by wlterm2.c. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>

extern const struct wl_interface wl_webos_shell_interface, wl_webos_shell_surface_interface;
/* The TV's libwayland is old: it has no wl_proxy_marshal_flags, which the current headers' inline wrappers use. Call the
   older entry points directly instead of wl_display_get_registry / wl_registry_bind / wl_compositor_create_surface / wl_seat_get_keyboard. */
static void *bind_global(struct wl_registry *r, uint32_t name, const struct wl_interface *i, uint32_t ver) {
    return wl_proxy_marshal_constructor_versioned((struct wl_proxy *)r, 0, i, ver, name, i->name, ver, NULL);
}

static struct wl_display *dpy; static struct wl_registry *reg; static struct wl_compositor *compositor; static struct wl_proxy *webos_shell;
static struct wl_seat *seat; static struct wl_keyboard *kbd;
static EGLDisplay egl_dpy; static EGLSurface egl_surf; static GLuint prog, atlas_tex;
static GLint u_atlas;

/* ---- keyboard (wl_keyboard events → the shared keyev()) */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { (void)d; (void)k; (void)f; (void)sz; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) { (void)d; (void)k; (void)s; (void)sf; (void)keys; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; rep_key = -1; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) { (void)d; (void)k; (void)s; (void)t; keyev(key, st != 0); }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; rep_rate = rate > 0 ? 1000 / rate : 40; rep_delay = delay; }
static const struct wl_keyboard_listener kb_listener = {kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat};
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d; if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) { kbd = (struct wl_keyboard *)wl_proxy_marshal_constructor((struct wl_proxy *)s, 1, &wl_keyboard_interface, NULL); wl_keyboard_add_listener(kbd, &kb_listener, NULL); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};
static void on_global(void *d, struct wl_registry *r, uint32_t name, const char *ifc, uint32_t ver) {
    (void)d; (void)ver;
    if (eq(ifc, "wl_compositor")) compositor = (struct wl_compositor *)bind_global(r, name, &wl_compositor_interface, 3);
    else if (eq(ifc, "wl_webos_shell")) webos_shell = (struct wl_proxy *)bind_global(r, name, &wl_webos_shell_interface, 1);
    else if (eq(ifc, "wl_seat") && !seat) { seat = (struct wl_seat *)bind_global(r, name, &wl_seat_interface, 4); wl_seat_add_listener(seat, &seat_listener, NULL); }
}
static void on_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = {on_global, on_remove};

/* ---- glyph atlas: one cell-sized slot per (glyph, bold); alpha only */
enum { ATLAS = 2048 };
static int slots_x, slots_y, next_slot; static int *slot_of;   /* 2 per gcache entry: [normal, synthetic-bold] */
static u8 *slot_buf;                                           /* one cell, reused for rasterising */
static int atlas_gen;                                          /* bumped when the atlas is cleared */
static void atlas_reset(void) { for (int i = 0; i < 2 * 8192; i++) slot_of[i] = -1; next_slot = 1; atlas_gen++; }  /* slot 0 = empty */
static int glyph_slot(u32 cp, int bold) {
    Glyph *g = getglyph(cp, bold); int gi = (g - gcache) * 2 + (bold && !have_bold);
    if (slot_of[gi] >= 0) return slot_of[gi];
    int s = next_slot++; slot_of[gi] = s;
    memset(slot_buf, 0, (size_t)cw * ch);
    if (g->bmp) {
        int ox = g->xo < 0 ? 0 : g->xo, oy = baseline + g->yo;
        for (int j = 0; j < g->h; j++) {
            int yy = oy + j; if (yy < 0 || yy >= ch) continue;
            for (int i = 0; i < g->w; i++) {
                int xx = ox + i; if (xx >= cw) break; int a = g->bmp[j * g->w + i]; if (!a) continue;
                u8 *p = &slot_buf[yy * cw + xx]; if (a > *p) *p = a;
                if (bold && !have_bold && xx + 1 < cw && a > p[1]) p[1] = a;      /* synthetic bold: smear 1px */
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, (s % slots_x) * cw, (s / slots_x) * ch, cw, ch, GL_ALPHA, GL_UNSIGNED_BYTE, slot_buf);
    return s;
}

/* ---- per-frame vertex data */
typedef struct { float x, y, lx, ly, ux, uy, fg[3], bg[3], fl; } Vtx;           /* 13 floats */
static Vtx *verts; static unsigned short *idx;
static const char *VS =
    "attribute vec2 pos; attribute vec2 lc; attribute vec2 uo; attribute vec3 fg; attribute vec3 bg; attribute float fl;\n"
    "varying vec2 l; varying vec2 u; varying vec3 f; varying vec3 b; varying float flags;\n"
    "void main(){ gl_Position=vec4(pos,0.,1.); l=lc; u=uo; f=fg; b=bg; flags=fl; }\n";
static char fs_src[1024];
static void make_fs(void) {
    snprintf(fs_src, sizeof fs_src,
        "precision highp float; varying vec2 l; varying vec2 u; varying vec3 f; varying vec3 b; varying float flags; uniform sampler2D atlas;\n"
        "void main(){ float a=texture2D(atlas,u+l*vec2(%f,%f)).a;\n"
        "  float ul=step(0.5,mod(flags,2.))*step(%f,l.y)*step(l.y,%f);\n"                  /* underline rows ch-3..ch-2 */
        "  float st=step(1.5,mod(flags,4.))*step(%f,l.y)*step(l.y,%f);\n"                   /* strike row ch/2 */
        "  a=max(a,max(ul,st)); gl_FragColor=vec4(mix(b,f,a),1.); }\n",
        (double)cw / ATLAS, (double)ch / ATLAS,
        (double)(ch - 3) / ch, (double)(ch - 2) / ch, (double)(ch / 2) / ch, (double)(ch / 2 + 1) / ch);
}
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader error: %s\n", log); exit(1); }
    return s;
}
static void rgbf(u32 c, float *o) { o[0] = (c >> 16 & 255) / 255.f; o[1] = (c >> 8 & 255) / 255.f; o[2] = (c & 255) / 255.f; }

static int gl_init(void) {
    dpy = wl_display_connect(NULL); if (!dpy) { fprintf(stderr, "no wayland display (XDG_RUNTIME_DIR=%s)\n", getenv("XDG_RUNTIME_DIR")); return 0; }
    reg = (struct wl_registry *)wl_proxy_marshal_constructor((struct wl_proxy *)dpy, 1, &wl_registry_interface, NULL); wl_registry_add_listener(reg, &reg_listener, NULL); wl_display_roundtrip(dpy); wl_display_roundtrip(dpy);
    if (!compositor || !webos_shell) { fprintf(stderr, "missing wayland globals\n"); return 0; }
    struct wl_surface *surf = (struct wl_surface *)wl_proxy_marshal_constructor((struct wl_proxy *)compositor, 0, &wl_surface_interface, NULL);
    struct wl_proxy *ss = wl_proxy_marshal_constructor(webos_shell, 1, &wl_webos_shell_surface_interface, NULL, surf);   /* get_shell_surface */
    wl_proxy_marshal(ss, 2, "appId", "org.tvwl.wlterm"); wl_proxy_marshal(ss, 2, "_WEBOS_WINDOW_TYPE", "_WEBOS_WINDOW_TYPE_CARD");
    wl_proxy_marshal(ss, 2, "instanceId", "org.tvwl.wlterm"); wl_proxy_marshal(ss, 1, 3);                                 /* set_state(fullscreen) */
    struct wl_egl_window *win = wl_egl_window_create(surf, W, H);
    egl_dpy = eglGetDisplay((EGLNativeDisplayType)dpy); EGLint mj, mn;
    if (!eglInitialize(egl_dpy, &mj, &mn)) { fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError()); return 0; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint nc = 0; eglChooseConfig(egl_dpy, ca, &cfg, 1, &nc); if (!nc) { fprintf(stderr, "no EGL config\n"); return 0; }
    static const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, xa); egl_surf = eglCreateWindowSurface(egl_dpy, cfg, (EGLNativeWindowType)win, NULL);
    if (!ctx || egl_surf == EGL_NO_SURFACE) { fprintf(stderr, "EGL ctx/surface failed 0x%x\n", eglGetError()); return 0; }
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, ctx); eglSwapInterval(egl_dpy, 1);
    fprintf(stderr, "GL: %s | %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

    make_fs(); GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs);
    static const char *names[] = {"pos", "lc", "uo", "fg", "bg", "fl"};
    for (int i = 0; i < 6; i++) glBindAttribLocation(prog, i, names[i]);
    glLinkProgram(prog); GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "program link failed\n"); return 0; }
    glUseProgram(prog); u_atlas = glGetUniformLocation(prog, "atlas"); glUniform1i(u_atlas, 0);
    slots_x = ATLAS / cw; slots_y = ATLAS / ch;
    glGenTextures(1, &atlas_tex); glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    u8 *zero = calloc(1, (size_t)ATLAS * ATLAS);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE, zero); free(zero);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    slot_of = malloc(2 * 8192 * sizeof(int)); slot_buf = malloc((size_t)cw * ch); atlas_reset();
    int n = cols * rows; verts = malloc((size_t)n * 4 * sizeof(Vtx)); idx = malloc((size_t)n * 6 * sizeof(unsigned short));
    for (int i = 0; i < n; i++) { unsigned short b = i * 4, *p = &idx[i * 6]; p[0] = b; p[1] = b + 1; p[2] = b + 2; p[3] = b + 2; p[4] = b + 1; p[5] = b + 3; }
    glViewport(0, 0, W, H); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    return 1;
}

static void do_shot(const char *path); static const char *shot_path;
static void flush(void) {
    static int pcy = -1, pcx = -1, pview = -1, pgen = -1; static u8 *rowdirty;
    if (!rowdirty) rowdirty = calloc(rows, 1);
    int any = 0, all = (pgen != atlas_gen) || (pview != viewoff);
    if (pcy != cy || pcx != cx) { any = 1; if (pcy >= 0 && pcy < rows) rowdirty[pcy] = 1; rowdirty[cy] = 1; }
    pcy = cy; pcx = cx; pview = viewoff;
    for (int y = 0; y < rows; y++) if (dirty[y]) { any = 1; dirty[y] = 0; rowdirty[y] = 1; }
    if (!any && !all) return;
    if (next_slot + cols * rows > slots_x * slots_y) { atlas_reset(); all = 1; }   /* clear before building so slots stay valid */
    pgen = atlas_gen;
    for (int y = 0; y < rows; y++) {
        if (!all && !rowdirty[y]) continue;
        rowdirty[y] = 0; Cell *r = getrow(y); Vtx *v = &verts[(size_t)y * cols * 4];
        for (int x = 0; x < cols; x++) {
            const Cell *c = &r[x]; u32 fg = c->fg, bg = c->bg;
            if (c->at & A_INV) { u32 t = fg; fg = bg; bg = t; }
            if (c->at & A_DIM) fg = rgb_mix(bg, fg, 150);
            if (curvis && !viewoff && x == cx && y == cy) { u32 t = fg; fg = bg; bg = t; if (fg == bg) fg = DEFBG; }
            int s = (c->cp > 32 && !(c->at & A_HIDE)) ? glyph_slot(c->cp, c->at & A_BOLD) : 0;
            float ux = (float)((s % slots_x) * cw) / ATLAS, uy = (float)((s / slots_x) * ch) / ATLAS;
            float x0 = (float)x * cw / W * 2 - 1, x1 = (float)(x + 1) * cw / W * 2 - 1, y0 = 1 - (float)y * ch / H * 2, y1 = 1 - (float)(y + 1) * ch / H * 2;
            float fl = ((c->at & A_UL) ? 1.f : 0.f) + ((c->at & A_STRIKE) ? 2.f : 0.f);
            float ff[3], bb[3]; rgbf(fg, ff); rgbf(bg, bb);
            float cxs[4] = {x0, x1, x0, x1}, cys[4] = {y0, y0, y1, y1}, lx[4] = {0, 1, 0, 1}, ly[4] = {0, 0, 1, 1};
            for (int k = 0; k < 4; k++, v++) { v->x = cxs[k]; v->y = cys[k]; v->lx = lx[k]; v->ly = ly[k]; v->ux = ux; v->uy = uy; memcpy(v->fg, ff, 12); memcpy(v->bg, bb, 12); v->fl = fl; }
        }
    }
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    const char *base = (const char *)verts; GLsizei st = sizeof(Vtx);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, base + 0);  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, base + 8);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, base + 16); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st, base + 24);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, st, base + 36); glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, st, base + 48);
    for (int i = 0; i < 6; i++) glEnableVertexAttribArray(i);
    glDrawElements(GL_TRIANGLES, cols * rows * 6, GL_UNSIGNED_SHORT, idx);
    if (shot_path) { do_shot(shot_path); free((void *)shot_path); shot_path = NULL; }
    eglSwapBuffers(egl_dpy, egl_surf);
}

/* "shot": the readback happens inside flush(), after drawing and before the swap, so it captures the frame being presented */
static void front_shot(const char *path) { shot_path = strdup(path); markall(); }
static void do_shot(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    u8 *rgba = malloc((size_t)W * H * 4); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    fprintf(f, "P6\n%d %d\n255\n", W, H); u8 *row = malloc(W * 3);
    for (int y = H - 1; y >= 0; y--) { for (int x = 0; x < W; x++) memcpy(&row[x * 3], &rgba[((size_t)y * W + x) * 4], 3); fwrite(row, 3, W, f); }
    free(row); free(rgba); fclose(f);
}
