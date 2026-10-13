/* wlterm2: terminal for the webOS wayland compositor.
 * 64-bit static (aarch64), CPU rendering into wl_shm, TrueType fonts via stb_truetype.
 * usage: wlterm2 [-f regular.ttf] [-b bold.ttf] [-x fallback.ttf]... [-s pixels] [-e shell]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

typedef uint32_t u32; typedef uint8_t u8; typedef uint16_t u16;
enum { W = 1920, H = 1080, SBMAX = 5000, MAXFONTS = 6 };
#define DEFFG 0xd8dee9u
#define DEFBG 0x101418u
enum { A_BOLD = 1, A_DIM = 2, A_ITAL = 4, A_UL = 8, A_INV = 16, A_STRIKE = 32, A_HIDE = 64 };
typedef struct { u32 cp, fg, bg; u16 at; } Cell;

/* ------------------------------------------------------------------ fonts */
typedef struct { stbtt_fontinfo fi; u8 *data; float scale; } Font;
static Font fonts[MAXFONTS]; static int nfonts;          /* [0] regular, [1] bold (or copy of 0), rest fallbacks */
static int cw, ch, baseline, px_size = 30; static int have_bold;
typedef struct { u32 key; u8 *bmp; int w, h, xo, yo; char used; } Glyph;
static Glyph gcache[8192];

static u8 *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    u8 *b = malloc(*n); if (fread(b, 1, *n, f) != (size_t)*n) { free(b); b = NULL; } fclose(f); return b;
}
static int loadfont(Font *f, const char *path) {
    long n; f->data = slurp(path, &n); if (!f->data) return 0;
    if (!stbtt_InitFont(&f->fi, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) return 0;
    f->scale = stbtt_ScaleForPixelHeight(&f->fi, px_size); return 1;
}
static Glyph *getglyph(u32 cp, int bold) {
    u32 key = cp * 2 + (bold && have_bold);
    u32 h = (key * 2654435761u) >> 19 & 8191;
    for (int i = 0; i < 8192; i++, h = (h + 1) & 8191) {
        Glyph *g = &gcache[h];
        if (g->used && g->key == key) return g;
        if (!g->used) {
            g->used = 1; g->key = key;
            int fi = (bold && have_bold) ? 1 : 0;
            if (!stbtt_FindGlyphIndex(&fonts[fi].fi, cp))
                for (int k = 2; k < nfonts; k++) if (stbtt_FindGlyphIndex(&fonts[k].fi, cp)) { fi = k; break; }
            Font *f = &fonts[fi];
            g->bmp = stbtt_GetCodepointBitmap(&f->fi, f->scale, f->scale, cp, &g->w, &g->h, &g->xo, &g->yo);
            return g;
        }
    }
    return &gcache[0];
}

/* ------------------------------------------------------------------ screen */
static int cols, rows;
static Cell **scr, **mainscr, **altscr; static int in_alt;
static Cell **sb; static int sbhead, sbcount, viewoff;
static u8 *dirty;
static int cx, cy, wrapnext, curvis = 1, top, bot, saved_cx, saved_cy, appcur, autowrap = 1, irm;
static u32 curfg = DEFFG, curbg = DEFBG; static u16 curat;
static u8 *tabs; static int charset[2], gl;
static Cell blank;

static Cell *newrow(void) { Cell *r = malloc(cols * sizeof(Cell)); for (int i = 0; i < cols; i++) r[i] = blank; return r; }
static void markall(void) { memset(dirty, 1, rows); }
static void clearcells(Cell *r, int x0, int x1) { for (int x = x0; x < x1; x++) { r[x] = blank; r[x].bg = curbg; } }
static void scroll_up(int t, int b, int n) {
    for (int i = 0; i < n; i++) {
        Cell *r = scr[t];
        if (t == 0 && !in_alt) {
            if (sbcount == SBMAX) { free(sb[sbhead]); sb[sbhead] = r; sbhead = (sbhead + 1) % SBMAX; }
            else sb[(sbhead + sbcount++) % SBMAX] = r;
            r = newrow();
        }
        memmove(&scr[t], &scr[t + 1], (b - t) * sizeof(Cell *));
        if (t == 0 && !in_alt) scr[b] = r; else { scr[b] = r; clearcells(r, 0, cols); }
        if (t == 0 && !in_alt) clearcells(scr[b], 0, cols);
    }
    markall();
}
static void scroll_down(int t, int b, int n) {
    for (int i = 0; i < n; i++) {
        Cell *r = scr[b]; memmove(&scr[t + 1], &scr[t], (b - t) * sizeof(Cell *));
        scr[t] = r; clearcells(r, 0, cols);
    }
    markall();
}
static void linefeed(void) { if (cy == bot) scroll_up(top, bot, 1); else if (cy < rows - 1) cy++; }
static Cell *getrow(int y) {
    if (!viewoff) return scr[y];
    int idx = sbcount - viewoff + y;
    return idx < sbcount ? sb[(sbhead + idx) % SBMAX] : scr[idx - sbcount];
}
static void switch_screen(int alt) {
    if (alt == in_alt) return;
    in_alt = alt; scr = alt ? altscr : mainscr; viewoff = 0;
    if (alt) for (int y = 0; y < rows; y++) clearcells(scr[y], 0, cols);
    markall();
}

/* ------------------------------------------------------------------ rendering */
static u32 *px; static int wfd, master;
static u32 rgb_mix(u32 bg, u32 fg, int a) {
    int r = ((bg >> 16 & 255) * (255 - a) + (fg >> 16 & 255) * a) / 255;
    int g = ((bg >> 8 & 255) * (255 - a) + (fg >> 8 & 255) * a) / 255;
    int b = ((bg & 255) * (255 - a) + (fg & 255) * a) / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}
static void drawcell(int x, int y, const Cell *c, int iscur) {
    u32 fg = c->fg, bg = c->bg;
    if (c->at & A_INV) { u32 t = fg; fg = bg; bg = t; }
    if (c->at & A_DIM) fg = rgb_mix(bg, fg, 150);
    if (iscur) { u32 t = fg; fg = bg; bg = t; if (fg == bg) fg = DEFBG; }
    u32 *p0 = px + (size_t)y * ch * W + x * cw; u32 bgp = 0xff000000u | bg;
    for (int j = 0; j < ch; j++) { u32 *p = p0 + (size_t)j * W; for (int i = 0; i < cw; i++) p[i] = bgp; }
    if (c->cp > 32 && !(c->at & A_HIDE)) {
        Glyph *g = getglyph(c->cp, c->at & A_BOLD);
        if (g->bmp) {
            int ox = (g->xo < 0 ? 0 : g->xo), oy = baseline + g->yo;
            for (int j = 0; j < g->h; j++) {
                int yy = oy + j; if (yy < 0 || yy >= ch) continue;
                for (int i = 0; i < g->w; i++) {
                    int xx = ox + i; if (xx >= cw) break;
                    int a = g->bmp[j * g->w + i]; if (!a) continue;
                    u32 *p = p0 + (size_t)yy * W + xx; *p = rgb_mix(*p & 0xffffff, fg, a);
                    if ((c->at & A_BOLD) && !have_bold && xx + 1 < cw) { u32 *q = p + 1; *q = rgb_mix(*q & 0xffffff, fg, a); }
                }
            }
        }
    }
    if (c->at & A_UL) for (int i = 0; i < cw; i++) p0[(size_t)(ch - 3) * W + i] = 0xff000000u | fg;
    if (c->at & A_STRIKE) for (int i = 0; i < cw; i++) p0[(size_t)(ch / 2) * W + i] = 0xff000000u | fg;
}
static void flush(void);
static void msg(u32 id, u32 op, int n, const u32 *args) {
    u32 m[64]; m[0] = id; m[1] = ((8 + 4 * n) << 16) | op; for (int i = 0; i < n; i++) m[2 + i] = args[i];
    send(wfd, m, 8 + 4 * n, MSG_NOSIGNAL);
}
static void flush(void) {
    static int pcy = -1; int lo = -1, hi = -1;
    if (pcy >= 0 && pcy < rows) dirty[pcy] = 1;                 /* erase the old cursor block */
    if (viewoff == 0 && curvis) dirty[cy] = 1;
    pcy = cy;
    for (int y = 0; y < rows; y++) if (dirty[y]) {
        Cell *r = getrow(y);
        for (int x = 0; x < cols; x++) drawcell(x, y, &r[x], curvis && !viewoff && x == cx && y == cy);
        dirty[y] = 0; if (lo < 0) lo = y; hi = y;
    }
    if (lo < 0) return;
    u32 a[3] = {10, 0, 0}; msg(7, 1, 3, a);
    u32 d[4] = {0, lo * ch, W, (hi - lo + 1) * ch}; msg(7, 2, 4, d);
    msg(7, 6, 0, 0);
}

/* ------------------------------------------------------------------ terminal emulation */
static u32 pal256[256];
static void initpal(void) {
    static const u32 base[16] = {0x1b2026, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xd8dee9,
                                 0x5c6370, 0xff7b86, 0xb5e890, 0xf5d68b, 0x82c4ff, 0xe39bf5, 0x7ad4e0, 0xffffff};
    for (int i = 0; i < 16; i++) pal256[i] = base[i];
    for (int i = 0; i < 216; i++) { int r = i / 36, g = i / 6 % 6, b = i % 6; pal256[16 + i] = (r ? r * 40 + 55 : 0) << 16 | (g ? g * 40 + 55 : 0) << 8 | (b ? b * 40 + 55 : 0); }
    for (int i = 0; i < 24; i++) pal256[232 + i] = (8 + i * 10) * 0x010101;
}
static void reply(const char *s) { if (write(master, s, strlen(s)) < 0) {} }
static void putcp(u32 cp) {
    if (cp >= 0x300 && cp <= 0x36f) return;                          /* combining: not shaped */
    if (wrapnext) { cx = 0; wrapnext = 0; linefeed(); }
    if (irm) { Cell *r = scr[cy]; memmove(&r[cx + 1], &r[cx], (cols - cx - 1) * sizeof(Cell)); }
    Cell *c = &scr[cy][cx]; c->cp = cp; c->fg = curfg; c->bg = curbg; c->at = curat; dirty[cy] = 1;
    if (cx + 1 >= cols) { if (autowrap) wrapnext = 1; } else cx++;
}
static const u32 dec_gfx[31] = {0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0xb0, 0xb1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
                                0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x3c0, 0x2260, 0xa3, 0xb7};
static int st, par[32], npar, priv, inter, utf_need; static u32 utf_cp;
static int P(int i, int d) { return (i < npar && par[i] > 0) ? par[i] : d; }
static void clampcur(void) { if (cx < 0) cx = 0; if (cx >= cols) cx = cols - 1; if (cy < 0) cy = 0; if (cy >= rows) cy = rows - 1; }
static void sgr(void) {
    if (!npar) { npar = 1; par[0] = 0; }
    for (int i = 0; i < npar; i++) {
        int v = par[i];
        if (v == 0) { curfg = DEFFG; curbg = DEFBG; curat = 0; }
        else if (v == 1) curat |= A_BOLD; else if (v == 2) curat |= A_DIM; else if (v == 3) curat |= A_ITAL;
        else if (v == 4 || v == 21) curat |= A_UL; else if (v == 7) curat |= A_INV; else if (v == 8) curat |= A_HIDE; else if (v == 9) curat |= A_STRIKE;
        else if (v == 22) curat &= ~(A_BOLD | A_DIM); else if (v == 23) curat &= ~A_ITAL; else if (v == 24) curat &= ~A_UL;
        else if (v == 27) curat &= ~A_INV; else if (v == 28) curat &= ~A_HIDE; else if (v == 29) curat &= ~A_STRIKE;
        else if (v >= 30 && v <= 37) curfg = pal256[v - 30 + ((curat & A_BOLD) ? 8 : 0)];
        else if (v >= 90 && v <= 97) curfg = pal256[v - 90 + 8];
        else if (v >= 40 && v <= 47) curbg = pal256[v - 40];
        else if (v >= 100 && v <= 107) curbg = pal256[v - 100 + 8];
        else if (v == 39) curfg = DEFFG; else if (v == 49) curbg = DEFBG;
        else if (v == 38 || v == 48) {
            u32 col = 0; int ok = 0;
            if (i + 2 < npar && par[i + 1] == 5) { col = pal256[par[i + 2] & 255]; i += 2; ok = 1; }
            else if (i + 4 < npar && par[i + 1] == 2) { col = (par[i + 2] & 255) << 16 | (par[i + 3] & 255) << 8 | (par[i + 4] & 255); i += 4; ok = 1; }
            if (ok) { if (v == 38) curfg = col; else curbg = col; }
        }
    }
}
static void setmode(int on) {
    for (int i = 0; i < npar; i++) {
        int v = par[i];
        if (priv) switch (v) {
            case 1: appcur = on; break; case 7: autowrap = on; break; case 25: curvis = on; break;
            case 47: case 1047: switch_screen(on); break;
            case 1049: if (on) { saved_cx = cx; saved_cy = cy; switch_screen(1); } else { switch_screen(0); cx = saved_cx; cy = saved_cy; } break;
        } else if (v == 4) irm = on;
    }
}
static void csi(u32 c) {
    wrapnext = 0;
    switch (c) {
    case '@': { int n = P(0, 1); Cell *r = scr[cy]; if (n > cols - cx) n = cols - cx; memmove(&r[cx + n], &r[cx], (cols - cx - n) * sizeof(Cell)); clearcells(r, cx, cx + n); dirty[cy] = 1; break; }
    case 'A': case 'k': cy -= P(0, 1); if (cy < top && cy + P(0, 1) >= top) cy = top; clampcur(); break;
    case 'B': case 'e': cy += P(0, 1); if (cy > bot && cy - P(0, 1) <= bot) cy = bot; clampcur(); break;
    case 'C': case 'a': cx += P(0, 1); clampcur(); break;
    case 'D': cx -= P(0, 1); clampcur(); break;
    case 'E': cy += P(0, 1); cx = 0; clampcur(); break;
    case 'F': cy -= P(0, 1); cx = 0; clampcur(); break;
    case 'G': case '`': cx = P(0, 1) - 1; clampcur(); break;
    case 'd': cy = P(0, 1) - 1; clampcur(); break;
    case 'H': case 'f': cy = P(0, 1) - 1; cx = P(1, 1) - 1; clampcur(); break;
    case 'I': { int n = P(0, 1); while (n-- && cx < cols - 1) do cx++; while (cx < cols - 1 && !tabs[cx]); break; }
    case 'Z': { int n = P(0, 1); while (n-- && cx > 0) do cx--; while (cx > 0 && !tabs[cx]); break; }
    case 'J': { int v = npar ? par[0] : 0;
        if (v == 0) { clearcells(scr[cy], cx, cols); for (int y = cy + 1; y < rows; y++) clearcells(scr[y], 0, cols); }
        else if (v == 1) { for (int y = 0; y < cy; y++) clearcells(scr[y], 0, cols); clearcells(scr[cy], 0, cx + 1); }
        else if (v == 2) for (int y = 0; y < rows; y++) clearcells(scr[y], 0, cols);
        else if (v == 3) { for (int i = 0; i < sbcount; i++) free(sb[(sbhead + i) % SBMAX]); sbcount = sbhead = 0; viewoff = 0; }
        markall(); break; }
    case 'K': { int v = npar ? par[0] : 0; if (v == 0) clearcells(scr[cy], cx, cols); else if (v == 1) clearcells(scr[cy], 0, cx + 1); else clearcells(scr[cy], 0, cols); dirty[cy] = 1; break; }
    case 'L': if (cy >= top && cy <= bot) scroll_down(cy, bot, P(0, 1)); break;
    case 'M': if (cy >= top && cy <= bot) scroll_up(cy, bot, P(0, 1)); break;
    case 'P': { int n = P(0, 1); Cell *r = scr[cy]; if (n > cols - cx) n = cols - cx; memmove(&r[cx], &r[cx + n], (cols - cx - n) * sizeof(Cell)); clearcells(r, cols - n, cols); dirty[cy] = 1; break; }
    case 'S': scroll_up(top, bot, P(0, 1)); break;
    case 'T': scroll_down(top, bot, P(0, 1)); break;
    case 'X': { int n = P(0, 1); clearcells(scr[cy], cx, cx + n > cols ? cols : cx + n); dirty[cy] = 1; break; }
    case 'b': { u32 last = cx > 0 ? scr[cy][cx - 1].cp : ' '; for (int n = P(0, 1); n > 0; n--) putcp(last); break; }
    case 'c': if (priv == 2) reply("\033[>0;115;0c"); else if (!priv) reply("\033[?62;4c"); break;
    case 'g': if (!npar || par[0] == 0) tabs[cx] = 0; else if (par[0] == 3) memset(tabs, 0, cols); break;
    case 'h': setmode(1); break; case 'l': setmode(0); break;
    case 'm': if (!priv) sgr(); break;
    case 'n': if (P(0, 0) == 5) reply("\033[0n"); else if (P(0, 0) == 6) { char b[32]; snprintf(b, sizeof b, "\033[%d;%dR", cy + 1, cx + 1); reply(b); } break;
    case 'r': { int t = P(0, 1) - 1, b = P(1, rows) - 1; if (t < 0) t = 0; if (b >= rows) b = rows - 1; if (t < b) { top = t; bot = b; } cx = 0; cy = 0; break; }
    case 's': saved_cx = cx; saved_cy = cy; break;
    case 'u': cx = saved_cx; cy = saved_cy; clampcur(); break;
    case 't': if (P(0, 0) == 18) { char b[32]; snprintf(b, sizeof b, "\033[8;%d;%dt", rows, cols); reply(b); } break;
    }
}
static void reset(void) {
    curfg = DEFFG; curbg = DEFBG; curat = 0; top = 0; bot = rows - 1; appcur = 0; autowrap = 1; irm = 0; curvis = 1; charset[0] = charset[1] = 0; gl = 0;
    switch_screen(0); for (int y = 0; y < rows; y++) clearcells(scr[y], 0, cols); cx = cy = 0; wrapnext = 0; markall();
}
static void feed(u8 b) {
    if (st == 0) {
        if (utf_need) {
            if ((b & 0xc0) == 0x80) { utf_cp = utf_cp << 6 | (b & 0x3f); if (!--utf_need) putcp(utf_cp); return; }
            utf_need = 0;
        }
        if (b >= 0xc2 && b < 0xe0) { utf_cp = b & 0x1f; utf_need = 1; return; }
        if (b >= 0xe0 && b < 0xf0) { utf_cp = b & 0x0f; utf_need = 2; return; }
        if (b >= 0xf0 && b < 0xf5) { utf_cp = b & 0x07; utf_need = 3; return; }
        if (b >= 0x80) return;
        switch (b) {
        case 0x1b: st = 1; return;
        case '\n': case 0x0b: case 0x0c: wrapnext = 0; linefeed(); return;
        case '\r': cx = 0; wrapnext = 0; return;
        case 8: if (cx > 0) cx--; wrapnext = 0; return;
        case 9: wrapnext = 0; do cx++; while (cx < cols - 1 && !tabs[cx]); if (cx >= cols) cx = cols - 1; return;
        case 0x0e: gl = 1; return; case 0x0f: gl = 0; return;
        }
        if (b >= 32 && b < 127) putcp(charset[gl] && b >= 0x5f && b <= 0x7e ? dec_gfx[b - 0x5f] : b);
    } else if (st == 1) {
        st = 0;
        switch (b) {
        case '[': st = 2; npar = 0; priv = 0; inter = 0; memset(par, 0, sizeof par); break;
        case ']': case 'P': case '_': case '^': case 'X': st = 4; break;
        case '(': st = 5; inter = 0; break; case ')': st = 5; inter = 1; break;
        case '7': saved_cx = cx; saved_cy = cy; break; case '8': cx = saved_cx; cy = saved_cy; break;
        case 'D': wrapnext = 0; linefeed(); break; case 'E': cx = 0; wrapnext = 0; linefeed(); break;
        case 'M': if (cy == top) scroll_down(top, bot, 1); else if (cy > 0) cy--; break;
        case 'H': tabs[cx] = 1; break;
        case 'c': reset(); break;
        case 'Z': reply("\033[?62;4c"); break;
        case '=': case '>': break;
        }
    } else if (st == 2) {
        if (b >= '0' && b <= '9') { if (!npar) npar = 1; if (npar <= 32) par[npar - 1] = par[npar - 1] * 10 + b - '0'; }
        else if (b == ';' || b == ':') { if (!npar) npar = 1; if (npar < 32) npar++; }
        else if (b == '?') priv = 1; else if (b == '>') priv = 2; else if (b == '=' || b == '<') priv = 3;
        else if (b >= 0x20 && b <= 0x2f) inter = 1;
        else if (b >= 0x40 && b <= 0x7e) { st = 0; if (!inter) csi(b); }
        else if (b == 0x1b) st = 1;
    } else if (st == 4) {                                     /* OSC / DCS / APC: skip to BEL or ST */
        if (b == 7) st = 0; else if (b == 0x1b) st = 6;
    } else if (st == 5) {                                     /* charset designation */
        st = 0; charset[inter] = (b == '0');
    } else if (st == 6) { st = 0; }
}

/* ------------------------------------------------------------------ keyboard */
static const char kn[] = "\0\033" "1234567890-=" "\177\t" "qwertyuiop[]" "\r\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
static const char ks[] = "\0\033" "!@#$%^&*()_+" "\177\t" "QWERTYUIOP{}" "\r\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";
static int shift, ctrl, alt, caps, rep_key = -1; static long rep_next, rep_delay = 400, rep_rate = 40;
static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void out(const char *s, int n) { if (write(master, s, n) < 0) {} }
static void keyout(int k) {
    char b[16]; int n = 0; int mod = 1 + shift + 2 * alt + 4 * ctrl;
    struct { int k; char fin; } cur[] = {{103, 'A'}, {108, 'B'}, {106, 'C'}, {105, 'D'}, {102, 'H'}, {107, 'F'}};
    struct { int k; int num; } tl[] = {{111, 3}, {110, 2}, {104, 5}, {109, 6}, {63, 15}, {64, 17}, {65, 18}, {66, 19}, {67, 20}, {68, 21}, {87, 23}, {88, 24}};
    if ((k == 104 || k == 109) && shift && !ctrl && !alt) { viewoff += k == 104 ? rows / 2 : -rows / 2; if (viewoff < 0) viewoff = 0; if (viewoff > sbcount) viewoff = sbcount; markall(); return; }
    viewoff = viewoff ? (markall(), 0) : 0;
    for (int i = 0; i < 6; i++) if (cur[i].k == k) {
        if (mod > 1) n = snprintf(b, sizeof b, "\033[1;%d%c", mod, cur[i].fin);
        else n = snprintf(b, sizeof b, appcur ? "\033O%c" : "\033[%c", cur[i].fin);
        out(b, n); return;
    }
    for (int i = 0; i < 12; i++) if (tl[i].k == k) { n = mod > 1 ? snprintf(b, sizeof b, "\033[%d;%d~", tl[i].num, mod) : snprintf(b, sizeof b, "\033[%d~", tl[i].num); out(b, n); return; }
    if (k >= 59 && k <= 62) { n = mod > 1 ? snprintf(b, sizeof b, "\033[1;%d%c", mod, 'P' + k - 59) : snprintf(b, sizeof b, "\033O%c", 'P' + k - 59); out(b, n); return; }
    if (k == 96) { out("\r", 1); return; }
    if (k > 0 && k < 58) {
        char c = kn[k]; int sh = shift; if (caps && c >= 'a' && c <= 'z') sh = !sh;
        c = sh ? ks[k] : kn[k];
        if (ctrl) { char lc = c | 32; if (lc >= 'a' && lc <= 'z') c = lc - 'a' + 1; else if (c == '[') c = 27; else if (c == ' ' || c == '@') c = 0; else if (c == '\\') c = 28; else if (c == ']') c = 29; else if (c == '/') c = 31; }
        if (alt) b[n++] = 27;
        if (c || ctrl) b[n++] = c;
        out(b, n);
    }
}
static void keyev(int k, int down) {
    if (k == 42 || k == 54) { shift = down; return; }
    if (k == 29 || k == 97) { ctrl = down; return; }
    if (k == 56 || k == 100) { alt = down; return; }
    if (k == 58) { if (down) caps = !caps; return; }
    if (down) { keyout(k); rep_key = k; rep_next = now_ms() + rep_delay; } else if (rep_key == k) rep_key = -1;
}

/* ------------------------------------------------------------------ wayland */
static u32 wbuf[16384]; static int whave, quit;
static u32 g_comp, g_shm, g_shell, g_seat[4]; static int nseat, have_kb[4]; static u32 nextkb;
static int eq(const char *a, const char *b) { return !strcmp(a, b); }
static void bind_(u32 name, const char *ifc, u32 ver, u32 nid) {
    u32 b[32]; int l = strlen(ifc) + 1, w = (l + 3) / 4, n = 0;
    b[n++] = name; b[n++] = l; memset(&b[n], 0, w * 4); memcpy(&b[n], ifc, l); n += w; b[n++] = ver; b[n++] = nid; msg(2, 0, n, b);
}
static void setprop(const char *k, const char *v) {
    u32 b[64]; int n = 0, l = strlen(k) + 1, w = (l + 3) / 4;
    b[n++] = l; memset(&b[n], 0, w * 4); memcpy(&b[n], k, l); n += w;
    l = strlen(v) + 1; w = (l + 3) / 4; b[n++] = l; memset(&b[n], 0, w * 4); memcpy(&b[n], v, l); n += w; msg(8, 2, n, b);
}
static int ev(u32 id, u32 op, u32 *p) {
    if (id == 3) return 1;
    if (id == 1 && op == 0) { fprintf(stderr, "wayland error: %s\n", (const char *)&p[3]); quit = 1; }
    if (id == 2 && op == 0) {
        const char *s = (const char *)&p[2];
        if (eq(s, "wl_compositor")) g_comp = p[0]; else if (eq(s, "wl_shm")) g_shm = p[0];
        else if (eq(s, "wl_webos_shell")) g_shell = p[0]; else if (eq(s, "wl_seat") && nseat < 4) g_seat[nseat++] = p[0];
    }
    if (nseat && id >= 11 && id < 11 + (u32)nseat && op == 0 && (p[0] & 2) && !have_kb[id - 11]) { u32 k = nextkb++; msg(id, 1, 1, &k); have_kb[id - 11] = 1; }
    if (nseat && id >= 11 + (u32)nseat && id < nextkb) {
        if (op == 3) keyev(p[2], p[3] != 0);
        if (op == 5) { rep_rate = p[0] > 0 ? 1000 / p[0] : 40; rep_delay = p[1]; }
        if (op == 2) rep_key = -1;
    }
    if (id == 8 && op == 2) quit = 1;
    return 0;
}
static int wread(void) {
    ssize_t r = read(wfd, (char *)wbuf + whave, sizeof wbuf - whave); if (r <= 0) return -1;
    whave += r; int off = 0, done = 0;
    while (whave - off >= 8) {
        u32 id = wbuf[off / 4], sz = wbuf[off / 4 + 1] >> 16, op = wbuf[off / 4 + 1] & 0xffff;
        if (whave - off < (int)sz) break;
        if (ev(id, op, &wbuf[off / 4 + 2])) done = 1;
        off += sz;
    }
    memmove(wbuf, (char *)wbuf + off, whave - off); whave -= off; return done;
}


/* ------------------------------------------------------------------ test control (fifo): in <text> | shot <path> | text <path> | quit */
static int ctl_fd = -1; static char ctlbuf[4096]; static int ctllen;
static void put_utf8(FILE *f, u32 c) {
    if (c < 0x80) fputc(c, f); else if (c < 0x800) { fputc(0xc0 | c >> 6, f); fputc(0x80 | (c & 63), f); }
    else if (c < 0x10000) { fputc(0xe0 | c >> 12, f); fputc(0x80 | (c >> 6 & 63), f); fputc(0x80 | (c & 63), f); }
    else { fputc(0xf0 | c >> 18, f); fputc(0x80 | (c >> 12 & 63), f); fputc(0x80 | (c >> 6 & 63), f); fputc(0x80 | (c & 63), f); }
}
static void ctl_line(char *l) {
    if (!strncmp(l, "in ", 3)) {
        char o[2048]; int n = 0; for (char *p = l + 3; *p && n < 2000; p++) {
            if (*p != '\\') { o[n++] = *p; continue; }
            p++; if (*p == 'n') o[n++] = '\n'; else if (*p == 'r') o[n++] = '\r'; else if (*p == 't') o[n++] = '\t'; else if (*p == 'e') o[n++] = 27;
            else if (*p == '\\') o[n++] = '\\'; else if (*p == 'x' && p[1] && p[2]) { char h[3] = {p[1], p[2], 0}; o[n++] = (char)strtol(h, NULL, 16); p += 2; }
        }
        out(o, n);
    } else if (!strncmp(l, "shot ", 5)) {
        FILE *f = fopen(l + 5, "wb"); if (!f) return; fprintf(f, "P6\n%d %d\n255\n", W, H);
        u8 *row = malloc(W * 3); for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) { u32 v = px[(size_t)y * W + x]; row[x * 3] = v >> 16; row[x * 3 + 1] = v >> 8; row[x * 3 + 2] = v; } fwrite(row, 3, W, f); }
        free(row); fclose(f);
    } else if (!strncmp(l, "text ", 5)) {
        FILE *f = fopen(l + 5, "wb"); if (!f) return; fprintf(f, "# %dx%d cursor=%d,%d alt=%d sb=%d\n", cols, rows, cx, cy, in_alt, sbcount);
        for (int y = 0; y < rows; y++) { int last = -1; for (int x = 0; x < cols; x++) if (scr[y][x].cp > 32) last = x;
            { for (int x = 0; x <= last; x++) put_utf8(f, scr[y][x].cp > 32 ? scr[y][x].cp : ' '); } fputc('\n', f); }
        fclose(f);
    } else if (!strncmp(l, "quit", 4)) quit = 1;
}
static void ctl_read(void) {
    ssize_t k = read(ctl_fd, ctlbuf + ctllen, sizeof ctlbuf - ctllen - 1); if (k <= 0) return; ctllen += k;
    char *nl; while ((nl = memchr(ctlbuf, '\n', ctllen))) { *nl = 0; ctl_line(ctlbuf); int used = nl - ctlbuf + 1; memmove(ctlbuf, ctlbuf + used, ctllen - used); ctllen -= used; }
    if (ctllen >= (int)sizeof ctlbuf - 1) ctllen = 0;
}

/* ------------------------------------------------------------------ config */
/* wlterm.conf: "key = value" lines, '#' comments. Keys: shell, home, cwd, path (appended to PATH), size, font, bold, fallback (repeatable), env NAME=VALUE (repeatable).
   Precedence: command line > config file > built-in defaults. */
#define MAXENV 16
static const char *cfg_home = "/media/developer/home", *cfg_cwd, *cfg_path = "", *cfg_env[MAXENV]; static int ncfg_env;
static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *e = s + strlen(s); while (e > s && strchr(" \t\r\n", e[-1])) *--e = 0; return s; }
static void load_conf(const char *file, const char **freg, const char **fbold, const char **shell, const char **fb, int *nfb) {
    FILE *f = fopen(file, "r"); if (!f) return; char l[1024];
    while (fgets(l, sizeof l, f)) {
        char *h = strchr(l, '#'); if (h) *h = 0; char *eq = strchr(l, '='); if (!eq) continue;
        *eq = 0; char *k = trim(l), *v = trim(eq + 1); if (!*k || !*v) continue; v = strdup(v);
        if (!strcmp(k, "shell")) *shell = v; else if (!strcmp(k, "home")) cfg_home = v; else if (!strcmp(k, "cwd")) cfg_cwd = v; else if (!strcmp(k, "path")) cfg_path = v;
        else if (!strcmp(k, "size")) px_size = atoi(v); else if (!strcmp(k, "font")) *freg = v; else if (!strcmp(k, "bold")) *fbold = v;
        else if (!strcmp(k, "fallback") && *nfb < MAXFONTS - 2) fb[(*nfb)++] = v; else if (!strcmp(k, "env") && ncfg_env < MAXENV && strchr(v, '=')) cfg_env[ncfg_env++] = v;
        else fprintf(stderr, "wlterm2: ignoring unknown config key '%s'\n", k);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    const char *freg = "/media/developer/fonts/FiraCode-Regular.ttf", *fbold = "/media/developer/fonts/FiraCode-Bold.ttf", *shell = NULL;
    const char *fb[MAXFONTS]; int nfb = 0;
    { const char *conf = getenv("WLTERM_CONF"); if (!conf) conf = "/media/developer/wlterm.conf";
      for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "-c")) conf = argv[i + 1];
      load_conf(conf, &freg, &fbold, &shell, fb, &nfb); }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) freg = argv[++i]; else if (!strcmp(argv[i], "-b") && i + 1 < argc) fbold = argv[++i];
        else if (!strcmp(argv[i], "-x") && i + 1 < argc && nfb < MAXFONTS - 2) fb[nfb++] = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) px_size = atoi(argv[++i]); else if (!strcmp(argv[i], "-e") && i + 1 < argc) shell = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) i++; else if (!strcmp(argv[i], "-H") && i + 1 < argc) cfg_home = argv[++i]; else if (!strcmp(argv[i], "-d") && i + 1 < argc) cfg_cwd = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) cfg_path = argv[++i];
    }
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_DFL);
    if (!loadfont(&fonts[0], freg)) { fprintf(stderr, "cannot load font %s\n", freg); return 1; }
    nfonts = 1; have_bold = loadfont(&fonts[1], fbold); if (!have_bold) fonts[1] = fonts[0]; nfonts = 2;
    for (int i = 0; i < nfb; i++) if (loadfont(&fonts[nfonts], fb[i])) nfonts++;
    { int asc, desc, gap, adv, lsb; stbtt_GetFontVMetrics(&fonts[0].fi, &asc, &desc, &gap); stbtt_GetCodepointHMetrics(&fonts[0].fi, '0', &adv, &lsb);
      float s = fonts[0].scale; cw = (int)(adv * s + 0.5f); ch = (int)((asc - desc + gap) * s + 0.999f); baseline = (int)(asc * s + gap * s / 2 + 0.5f); }
    cols = W / cw; rows = H / ch; initpal();
    blank = (Cell){0, DEFFG, DEFBG, 0};
    mainscr = malloc(rows * sizeof(Cell *)); altscr = malloc(rows * sizeof(Cell *)); sb = calloc(SBMAX, sizeof(Cell *));
    for (int y = 0; y < rows; y++) { mainscr[y] = newrow(); altscr[y] = newrow(); }
    scr = mainscr; dirty = malloc(rows); tabs = calloc(cols, 1); for (int x = 8; x < cols; x += 8) tabs[x] = 1; top = 0; bot = rows - 1; markall();

    const char *dir = getenv("XDG_RUNTIME_DIR"); if (!dir) dir = "/tmp/xdg";
    const char *disp = getenv("WAYLAND_DISPLAY"); if (!disp) disp = "wayland-0";
    struct sockaddr_un a = {AF_UNIX}; snprintf(a.sun_path, sizeof a.sun_path, "%s/%s", dir, disp);
    wfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(wfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); return 1; }
    { u32 r = 2; msg(1, 1, 1, &r); u32 c = 3; msg(1, 0, 1, &c); }
    int r; while (!(r = wread())) ; if (r < 0 || !g_comp || !g_shm || !g_shell) { fprintf(stderr, "missing wayland globals\n"); return 1; }
    bind_(g_comp, "wl_compositor", 3, 4); bind_(g_shm, "wl_shm", 1, 5); bind_(g_shell, "wl_webos_shell", 1, 6);
    { u32 x = 7; msg(4, 0, 1, &x); } { u32 x[2] = {8, 7}; msg(6, 1, 2, x); }
    setprop("appId", "org.tvwl.wlterm"); setprop("_WEBOS_WINDOW_TYPE", "_WEBOS_WINDOW_TYPE_CARD"); setprop("instanceId", "org.tvwl.wlterm");
    size_t sz = (size_t)W * H * 4; int mf = memfd_create("wlterm2", 0); if (ftruncate(mf, sz) < 0) return 1;
    px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    { u32 pk[4] = {5, (16u << 16) | 0, 9, (u32)sz}; struct iovec iov = {pk, 16}; char cb[CMSG_SPACE(sizeof(int))] = {0};
      struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
      struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(cm), &mf, sizeof(int));
      sendmsg(wfd, &mh, MSG_NOSIGNAL); }
    { u32 x[6] = {10, 0, W, H, W * 4, 0}; msg(9, 0, 6, x); }
    for (int i = 0; i < nseat; i++) bind_(g_seat[i], "wl_seat", 4, 11 + i);
    nextkb = 11 + nseat; { u32 st_ = 3; msg(8, 1, 1, &st_); }
    for (size_t i = 0; i < (size_t)W * H; i++) px[i] = 0xff000000u | DEFBG;

    mkdir(cfg_home, 0755);
    struct winsize ws = {rows, cols, W, H}; pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1); setenv("COLORTERM", "truecolor", 1); setenv("HOME", cfg_home, 1); setenv("USER", "root", 1);
        setenv("EDITOR", "vim", 1); setenv("LANG", "C.UTF-8", 1);
        setenv("TERMINFO_DIRS", "/usr/alpine/etc/terminfo:/usr/alpine/usr/share/terminfo:/usr/share/terminfo", 1);
        setenv("SSL_CERT_FILE", "/usr/alpine/etc/ssl/certs/ca-certificates.crt", 1); setenv("GIT_EXEC_PATH", "/usr/alpine/usr/libexec/git-core", 1);
        { char pth[2048]; snprintf(pth, sizeof pth, "%s%s%s", "/usr/sbin:/usr/bin:/sbin:/bin:/usr/alpine/usr/bin:/usr/alpine/bin:/usr/alpine/usr/sbin:/usr/alpine/sbin:/media/developer/bin", *cfg_path ? ":" : "", cfg_path); setenv("PATH", pth, 1); }
        for (int i = 0; i < ncfg_env; i++) { char *e = strdup(cfg_env[i]), *q = strchr(e, '='); *q = 0; setenv(trim(e), trim(q + 1), 1); }
        if (chdir(cfg_cwd ? cfg_cwd : cfg_home) < 0 && chdir("/") < 0) _exit(127);
        const char *sh = shell ? shell : "/usr/alpine/usr/bin/fish"; setenv("SHELL", sh, 1);
        execl(sh, sh, (char *)NULL); execl("/bin/sh", "sh", (char *)NULL); _exit(127);
    }
    { const char *cp_ = getenv("WLTERM_CTL"); if (!cp_) cp_ = "/tmp/wlterm2.ctl"; unlink(cp_); if (mkfifo(cp_, 0600) == 0) ctl_fd = open(cp_, O_RDWR | O_NONBLOCK); }
    flush();
    u8 pb[8192];
    for (;;) {
        struct pollfd pf[3] = {{wfd, POLLIN, 0}, {master, POLLIN, 0}, {ctl_fd, POLLIN, 0}};
        int to = -1; if (rep_key >= 0) { to = rep_next - now_ms(); if (to < 0) to = 0; }
        poll(pf, 3, to);
        if (rep_key >= 0 && now_ms() >= rep_next) { keyout(rep_key); rep_next += rep_rate; }
        if (pf[0].revents & POLLIN) { if (wread() < 0) break; }
        if (pf[1].revents & POLLIN) {
            ssize_t k = read(master, pb, sizeof pb); if (k <= 0) break;
            if (viewoff) { viewoff = 0; markall(); }
            for (ssize_t i = 0; i < k; i++) feed(pb[i]);
        } else if (pf[1].revents & (POLLHUP | POLLERR)) break;
        if (pf[2].revents & POLLIN) ctl_read();
        if (quit) break;
        flush();
    }
    { int status = 0; pid_t w = waitpid(pid, &status, WNOHANG);
      fprintf(stderr, "wlterm2 exiting: quit=%d waitpid=%d exited=%d code=%d signaled=%d sig=%d\n", quit, (int)w, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status), WTERMSIG(status)); }
    return 0;
}
