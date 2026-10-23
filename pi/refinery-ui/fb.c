/* Framebuffer drawing. Everything is drawn on an 800x480 landscape canvas in RAM, then rotated
 * onto the 480x800 portrait panel in fb_flush(). (vc4-kms cannot rotate the fbdev by 90 degrees.)
 * Text uses the Terminus PSF console fonts that ship with Raspberry Pi OS. */
#include "refinery-ui.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

static px_t canvas[W * H];
static uint8_t *fbmem;
static size_t fblen;
static int fbfd = -1;
static unsigned fb_w, fb_h, fb_stride;   /* physical panel, stride in pixels */
static int rot = 90;
static int kd_set;

struct font { int w, h, bpr; unsigned n; uint8_t *glyphs; };
static struct font fonts[N_FONTS];

px_t *fb_canvas(void) { return canvas; }

int fb_open(const char *dev, int rotate)
{
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  rot = rotate;
  fbfd = open(dev, O_RDWR | O_CLOEXEC);
  if (fbfd < 0) return -1;
  if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &f)) return -1;
  if (v.bits_per_pixel != 16)
  {
    fprintf(stderr, "fb: need 16 bpp, panel is %u bpp\n", v.bits_per_pixel);
    return -1;
  }
  fb_w = v.xres;
  fb_h = v.yres;
  fb_stride = f.line_length / 2;
  if (fb_w < H || fb_h < W)
  {
    fprintf(stderr, "fb: panel %ux%u is smaller than %dx%d portrait\n", fb_w, fb_h, H, W);
    return -1;
  }
  fblen = f.smem_len;
  fbmem = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
  if (fbmem == MAP_FAILED) return -1;

  fb_power(1);   /* a previous run may have left the screen asleep */

  /* Stop the text console drawing over us (stdin is tty1 under systemd) */
  if (ioctl(0, KDSETMODE, KD_GRAPHICS) == 0) kd_set = 1;
  return 0;
}

/* Blank via the DRM fbdev emulation (DPMS off). If the driver can't, paint the panel black. */
void fb_power(int on)
{
  if (ioctl(fbfd, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN) == 0) return;
  if (!on) memset(fbmem, 0, fblen);
}

void fb_close(void)
{
  fb_power(1);
  if (kd_set) ioctl(0, KDSETMODE, KD_TEXT);
}

void fb_flush(void)
{
  px_t *dst = (px_t *)fbmem;
  /* 90: canvas (x, y) -> panel (H-1-y, x).  270: canvas (x, y) -> panel (y, W-1-x). */
  for (unsigned py = 0; py < W; py++)
  {
    px_t *row = dst + (size_t)py * fb_stride;
    if (rot == 90)
    {
      const px_t *src = canvas + py;             /* column x = py, walking y upward */
      for (unsigned px = 0; px < H; px++) row[px] = src[(size_t)(H - 1 - px) * W];
    }
    else
    {
      const px_t *src = canvas + (W - 1 - py);   /* column x = W-1-py, walking y downward */
      for (unsigned px = 0; px < H; px++) row[px] = src[(size_t)px * W];
    }
  }
}

void fill(int x, int y, int w, int h, px_t c)
{
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; j++)
  {
    px_t *p = canvas + (size_t)(y + j) * W + x;
    for (int i = 0; i < w; i++) p[i] = c;
  }
}

void outline(int x, int y, int w, int h, int t, px_t c)
{
  fill(x, y, w, t, c);
  fill(x, y + h - t, w, t, c);
  fill(x, y, t, h, c);
  fill(x + w - t, y, t, h, c);
}

/* ---- PSF1 / PSF2 fonts ---- */

static int load_psf(struct font *ft, const char *path)
{
  gzFile g = gzopen(path, "rb");
  if (!g) return -1;
  uint8_t hdr[32];
  int n = gzread(g, hdr, 4);
  if (n == 4 && hdr[0] == 0x36 && hdr[1] == 0x04)          /* PSF1: 8 px wide */
  {
    ft->w = 8;
    ft->h = hdr[3];
    ft->bpr = 1;
    ft->n = (hdr[2] & 1) ? 512 : 256;
  }
  else if (n == 4 && hdr[0] == 0x72 && hdr[1] == 0xb5 && hdr[2] == 0x4a && hdr[3] == 0x86)
  {
    if (gzread(g, hdr + 4, 28) != 28) { gzclose(g); return -1; }
    uint32_t v[8];
    memcpy(v, hdr, sizeof v);                              /* little-endian, as on the Pi */
    ft->n = v[4];
    ft->h = (int)v[6];
    ft->w = (int)v[7];
    ft->bpr = (ft->w + 7) / 8;
    if (v[2] > 32) gzseek(g, (z_off_t)v[2], SEEK_SET);     /* header size */
  }
  else { gzclose(g); return -1; }

  size_t len = (size_t)ft->n * (size_t)ft->h * (size_t)ft->bpr;
  ft->glyphs = malloc(len);
  int ok = ft->glyphs && gzread(g, ft->glyphs, (unsigned)len) == (int)len;
  gzclose(g);
  return ok ? 0 : -1;
}

int fonts_load(const char *dir)
{
  static const char *names[N_FONTS] = {
    "Lat15-TerminusBold16.psf.gz", "Lat15-TerminusBold24x12.psf.gz", "Lat15-TerminusBold32x16.psf.gz",
  };
  char path[256];
  for (int i = 0; i < N_FONTS; i++)
  {
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    if (load_psf(&fonts[i], path) != 0)
    {
      fprintf(stderr, "font: cannot load %s\n", path);
      return -1;
    }
  }
  return 0;
}

int font_h(int font) { return fonts[font].h; }
int text_w(int font, const char *s) { return (int)strlen(s) * fonts[font].w; }

/* Lat15 fonts keep ASCII at its own code points */
int text(int x, int y, int font, px_t c, const char *s)
{
  const struct font *ft = &fonts[font];
  int x0 = x;
  for (; *s; s++, x += ft->w)
  {
    unsigned ch = (unsigned char)*s;
    if (ch >= ft->n) ch = '?';
    const uint8_t *g = ft->glyphs + (size_t)ch * (size_t)(ft->h * ft->bpr);
    for (int j = 0; j < ft->h; j++)
    {
      int yy = y + j;
      if (yy < 0 || yy >= H) continue;
      const uint8_t *row = g + j * ft->bpr;
      for (int i = 0; i < ft->w; i++)
      {
        int xx = x + i;
        if (xx >= 0 && xx < W && (row[i >> 3] & (0x80 >> (i & 7)))) canvas[(size_t)yy * W + xx] = c;
      }
    }
  }
  return x - x0;
}
