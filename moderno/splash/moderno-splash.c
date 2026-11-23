/*
 * moderno-splash: boot screen for Crankshaft moderno.
 *
 * Top 80% of the framebuffer: a PNG (logo + name), scaled to fit and centred.
 * Bottom 20%: the last boot log lines read from stdin (journalctl -f), drawn with a PSF console font.
 * Runs until systemd stops it (autoapp does so once the dashboard is on screen).
 *
 * usage: journalctl -b -f -o short-monotonic | moderno-splash splash.png font.psf
 * build: gcc -O2 -Wall -o moderno-splash moderno-splash.c -lpng
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <png.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINES 64
#define LINE_LEN 256

static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint8_t *fb, *back; /* everything is drawn into back, then copied: no flicker */
static volatile sig_atomic_t running = 1;

static void on_signal(int sig) { (void)sig; running = 0; }

static uint32_t pack(unsigned r, unsigned g, unsigned b)
{
    return ((r >> (8 - vi.red.length)) << vi.red.offset) |
           ((g >> (8 - vi.green.length)) << vi.green.offset) |
           ((b >> (8 - vi.blue.length)) << vi.blue.offset);
}

static void put(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= (int)vi.xres || y >= (int)vi.yres) return;
    uint8_t *p = back + (size_t)(y + vi.yoffset) * fi.line_length + (size_t)(x + vi.xoffset) * (vi.bits_per_pixel / 8);
    switch (vi.bits_per_pixel) {
    case 16: *(uint16_t *)p = (uint16_t)c; break;
    case 24: p[0] = c & 0xff; p[1] = (c >> 8) & 0xff; p[2] = (c >> 16) & 0xff; break;
    case 32: *(uint32_t *)p = c; break;
    }
}

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) put(x, y, c);
}

/* ---- PNG ---- */
static uint8_t *img; /* RGBA */
static int img_w, img_h;

static int load_png(const char *path)
{
    png_image pi;
    memset(&pi, 0, sizeof pi);
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&pi, path)) return -1;
    pi.format = PNG_FORMAT_RGBA;
    img = malloc(PNG_IMAGE_SIZE(pi));
    if (!img || !png_image_finish_read(&pi, NULL, img, 0, NULL)) { png_image_free(&pi); return -1; }
    img_w = (int)pi.width;
    img_h = (int)pi.height;
    return 0;
}

/* Scale into the box (bw x bh at bx,by) keeping aspect, nearest neighbour, alpha over black. */
static void draw_image(int bx, int by, int bw, int bh)
{
    if (!img) return;
    double s = (double)bw / img_w;
    if ((double)bh / img_h < s) s = (double)bh / img_h;
    int w = (int)(img_w * s), h = (int)(img_h * s);
    int ox = bx + (bw - w) / 2, oy = by + (bh - h) / 2;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = img + (size_t)((int)(y / s) < img_h ? (int)(y / s) : img_h - 1) * img_w * 4;
        for (int x = 0; x < w; x++) {
            int sx = (int)(x / s);
            if (sx >= img_w) sx = img_w - 1;
            const uint8_t *px = row + sx * 4;
            unsigned a = px[3];
            put(ox + x, oy + y, pack(px[0] * a / 255, px[1] * a / 255, px[2] * a / 255));
        }
    }
}

/* ---- PSF font (v1 and v2, with unicode table) ---- */
static uint8_t *font;
static int glyph_w = 8, glyph_h = 16, glyph_bytes, glyph_count;
static const uint8_t *glyphs;
static int ascii_map[128];

static int load_psf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    font = malloc((size_t)size);
    if (!font || fread(font, 1, (size_t)size, f) != (size_t)size) { fclose(f); return -1; }
    fclose(f);

    const uint8_t *table = NULL;
    int unicode16 = 0;
    for (int i = 0; i < 128; i++) ascii_map[i] = i;

    if (size > 4 && font[0] == 0x36 && font[1] == 0x04) { /* PSF1 */
        glyph_w = 8;
        glyph_h = font[3];
        glyph_bytes = glyph_h;
        glyph_count = (font[2] & 0x01) ? 512 : 256;
        glyphs = font + 4;
        if (font[2] & 0x02) { table = glyphs + glyph_count * glyph_bytes; unicode16 = 1; }
    } else if (size > 32 && font[0] == 0x72 && font[1] == 0xb5 && font[2] == 0x4a && font[3] == 0x86) { /* PSF2 */
        const uint32_t *h = (const uint32_t *)font;
        uint32_t headersize = h[2], flags = h[3];
        glyph_count = (int)h[4];
        glyph_bytes = (int)h[5];
        glyph_h = (int)h[6];
        glyph_w = (int)h[7];
        glyphs = font + headersize;
        if (flags & 0x01) table = glyphs + (size_t)glyph_count * glyph_bytes;
    } else {
        return -1;
    }

    if (table) { /* map ASCII code points to glyph indexes */
        const uint8_t *p = table, *end = font + size;
        for (int g = 0; g < glyph_count && p < end; g++) {
            if (unicode16) {
                while (p + 1 < end) {
                    unsigned u = p[0] | (p[1] << 8);
                    p += 2;
                    if (u == 0xffff) break;
                    if (u < 128) ascii_map[u] = g;
                }
            } else {
                int seq = 0;
                while (p < end && *p != 0xff) {
                    if (*p == 0xfe) seq = 1;
                    else if (!seq && *p < 0x80) ascii_map[*p] = g;
                    p++;
                }
                p++;
            }
        }
    }
    return 0;
}

static void draw_text(int x, int y, const char *s, int scale, uint32_t c, int max_chars)
{
    if (!glyphs) return;
    int row_bytes = (glyph_w + 7) / 8;
    for (int i = 0; s[i] && i < max_chars; i++) {
        unsigned ch = (unsigned char)s[i];
        int g = ch < 128 ? ascii_map[ch] : ascii_map['?'];
        if (g < 0 || g >= glyph_count) continue;
        const uint8_t *bits = glyphs + (size_t)g * glyph_bytes;
        for (int gy = 0; gy < glyph_h; gy++)
            for (int gx = 0; gx < glyph_w; gx++)
                if (bits[gy * row_bytes + gx / 8] & (0x80 >> (gx % 8)))
                    fill(x + (i * glyph_w + gx) * scale, y + gy * scale, scale, scale, c);
    }
}

/* ---- main ---- */
static char lines[MAX_LINES][LINE_LEN];
static int line_count;

static void push_line(const char *s)
{
    if (line_count == MAX_LINES) {
        memmove(lines[0], lines[1], sizeof lines[0] * (MAX_LINES - 1));
        line_count--;
    }
    snprintf(lines[line_count++], LINE_LEN, "%s", s);
}

/* The kernel can swap the framebuffer during boot (firmware fb replaced by the GPU driver's one,
 * about 4-5 s in): writes to the old mapping then show nothing. So it is reopened whenever it changes. */
static int fd = -1;

static int same_fb(void)
{
    int f = open("/dev/fb0", O_RDWR);
    if (f < 0) return 1; /* in the middle of the swap: keep the old one for now */
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo x;
    int same = 1;
    if (!ioctl(f, FBIOGET_VSCREENINFO, &v) && !ioctl(f, FBIOGET_FSCREENINFO, &x))
        same = x.smem_start == fi.smem_start && x.smem_len == fi.smem_len && !strncmp(x.id, fi.id, sizeof x.id) &&
               v.xres == vi.xres && v.yres == vi.yres && v.bits_per_pixel == vi.bits_per_pixel;
    close(f);
    return same;
}

static int open_fb(void)
{
    if (fb && fb != MAP_FAILED) munmap(fb, fi.smem_len);
    if (fd >= 0) close(fd);
    free(back);
    fb = NULL;
    back = NULL;
    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi)) return -1;
    fb = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    back = calloc(1, fi.smem_len);
    if (fb == MAP_FAILED || !back) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s splash.png font.psf\n", argv[0]); return 2; }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (load_png(argv[1])) fprintf(stderr, "moderno-splash: cannot read %s\n", argv[1]);
    if (load_psf(argv[2])) fprintf(stderr, "moderno-splash: cannot read %s\n", argv[2]);
    if (open_fb()) { perror("fb0"); return 1; } /* systemd restarts us */

    char buf[LINE_LEN], *cur = buf;
    int eof = 0, layout = 1, ticks = 0;
    int W = 0, H = 0, top_h = 0, scale = 1, margin = 12, line_h = 18, visible = 1, max_chars = 1;
    uint32_t black = 0, dim = 0, bright = 0, rule = 0;
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    const time_t started = time(NULL);
    while (running) {
        if (time(NULL) - started > 150) break; /* safety net if the dashboard never comes up */
        if (++ticks >= 5) { /* about once a second */
            ticks = 0;
            if (!same_fb()) {
                fprintf(stderr, "moderno-splash: framebuffer changed, reopening\n");
                if (open_fb()) { usleep(200000); continue; }
                layout = 1;
            }
        }
        if (layout) {
            W = (int)vi.xres;
            H = (int)vi.yres;
            top_h = H * 8 / 10;
            scale = H >= 900 ? 2 : 1;
            margin = 12 * scale;
            line_h = glyph_h * scale + 2;
            visible = (H - top_h - margin) / line_h > 0 ? (H - top_h - margin) / line_h : 1;
            max_chars = (W - 2 * margin) / (glyph_w * scale);
            black = pack(0, 0, 0);
            dim = pack(120, 128, 140);
            bright = pack(220, 224, 232);
            rule = pack(40, 44, 52);
            fill(0, 0, W, H, black);
            draw_image(0, 0, W, top_h);
            fill(margin, top_h, W - 2 * margin, 1, rule);
            layout = 0;
        }

        fill(0, top_h + 1, W, H - top_h - 1, black);
        int first = line_count > visible ? line_count - visible : 0;
        for (int i = first; i < line_count; i++)
            draw_text(margin, top_h + margin / 2 + (i - first) * line_h, lines[i], scale,
                      i == line_count - 1 ? bright : dim, max_chars);
        /* every pass (<=200 ms): console text and early X frames must not show through */
        size_t start = (size_t)vi.yoffset * fi.line_length;
        memcpy(fb + start, back + start, (size_t)H * fi.line_length);

        if (eof) { usleep(200000); continue; }
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (poll(&p, 1, 200) > 0) {
            ssize_t n;
            char c;
            while ((n = read(0, &c, 1)) == 1) {
                if (c == '\n' || cur - buf == LINE_LEN - 1) {
                    *cur = 0;
                    push_line(buf);
                    cur = buf;
                } else if (c != '\r') {
                    *cur++ = (c == '\t') ? ' ' : c;
                }
            }
            if (n == 0) eof = 1; /* journalctl went away: keep the screen, stop reading */
        }
    }
    return 0;
}
