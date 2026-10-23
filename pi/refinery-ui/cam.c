/* Local webcam view (REQ-UI-040): while it is open, fetch a JPEG snapshot from ustreamer about
 * once a second (non-blocking, inside the main poll loop) and decode it with libjpeg-turbo.
 * The camera itself stays owned by ustreamer, which also feeds the web stream. */
#include "refinery-ui.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#define CAM_PORT     8080
#define FRAME_MS     1000       /* 1 fps, matching ustreamer */
#define TIMEOUT_MS   3000
#define MAX_JPEG     (512 * 1024)
#define FW_MAX       640
#define FH_MAX       480

static int on;
static int sock = -1;
static int sent;                 /* request written */
static int64_t started, last_frame_req;
static uint8_t *buf;
static size_t blen;
static px_t frame[FW_MAX * FH_MAX];
static int fw, fh, have_frame;

int cam_on(void) { return on; }
int cam_fd(void) { return sock; }
int cam_events(void) { return sent ? POLLIN : POLLOUT; }

static void stop(void)
{
  if (sock >= 0) close(sock);
  sock = -1;
  sent = 0;
  blen = 0;
}

void cam_set(int v)
{
  on = v;
  have_frame = 0;
  if (!on) stop();
  else last_frame_req = 0;
}

const px_t *cam_frame(int *w, int *h)
{
  if (!have_frame) return NULL;
  *w = fw;
  *h = fh;
  return frame;
}

static void start(int64_t now)
{
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(CAM_PORT) };
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sock < 0) return;
  if (connect(sock, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { stop(); return; }
  started = last_frame_req = now;
  sent = 0;
  blen = 0;
}

void cam_tick(int64_t now)
{
  if (!on) return;
  if (sock >= 0 && now - started > TIMEOUT_MS) stop();
  if (sock < 0 && now - last_frame_req >= FRAME_MS) start(now);
}

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
/* The C270's MJPEG often has a few stray bytes; libjpeg copes, so don't log a warning per frame */
static void jerr_quiet(j_common_ptr c, int level) { (void)c; (void)level; }

static int decode(const uint8_t *jpg, size_t len)
{
  struct jpeg_decompress_struct d;
  struct jerr je;
  static uint8_t row[FW_MAX * 3];
  d.err = jpeg_std_error(&je.pub);
  je.pub.error_exit = jerr_exit;
  je.pub.emit_message = jerr_quiet;
  if (setjmp(je.jb)) { jpeg_destroy_decompress(&d); return 0; }
  jpeg_create_decompress(&d);
  jpeg_mem_src(&d, jpg, (unsigned long)len);
  jpeg_read_header(&d, TRUE);
  d.out_color_space = JCS_RGB;
  while (((d.image_width * d.scale_num) / d.scale_denom > FW_MAX ||
          (d.image_height * d.scale_num) / d.scale_denom > FH_MAX) && d.scale_denom < 8)
    d.scale_denom *= 2;                             /* libjpeg scales down by 1/2, 1/4, 1/8 */
  d.dct_method = JDCT_IFAST;
  jpeg_calc_output_dimensions(&d);
  /* Never write past frame[] or row[]: reject anything that still doesn't fit */
  if (d.output_width > FW_MAX || d.output_height > FH_MAX || d.out_color_components != 3)
  {
    jpeg_destroy_decompress(&d);
    return 0;
  }
  jpeg_start_decompress(&d);
  fw = (int)d.output_width;
  fh = (int)d.output_height;
  uint8_t *rp = row;
  while (d.output_scanline < d.output_height)
  {
    px_t *dst = frame + (size_t)d.output_scanline * (size_t)fw;
    jpeg_read_scanlines(&d, &rp, 1);
    for (int x = 0; x < fw; x++) dst[x] = RGB(row[3 * x], row[3 * x + 1], row[3 * x + 2]);
  }
  jpeg_finish_decompress(&d);
  jpeg_destroy_decompress(&d);
  return 1;
}

int cam_io(int64_t now)
{
  (void)now;
  if (sock < 0) return 0;
  if (!sent)
  {
    static const char req[] = "GET /snapshot HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (write(sock, req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) { stop(); return 0; }
    sent = 1;
    return 0;
  }
  if (!buf && !(buf = malloc(MAX_JPEG))) { stop(); return 0; }
  for (;;)
  {
    ssize_t n = read(sock, buf + blen, MAX_JPEG - blen);
    if (n > 0) { blen += (size_t)n; if (blen < MAX_JPEG) continue; }
    if (n < 0 && errno == EAGAIN) return 0;
    break;                                          /* EOF (HTTP/1.0 closes), error, or full */
  }
  int got = 0;
  uint8_t *body = memmem(buf, blen, "\r\n\r\n", 4);
  if (body && blen > 12 && memcmp(buf + 9, "200", 3) == 0 && decode(body + 4, blen - (size_t)(body + 4 - buf)))
    got = have_frame = 1;
  stop();
  return got;
}
