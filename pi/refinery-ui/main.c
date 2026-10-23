/* refinery-ui main loop. See refinery-ui.h for the overview.
 *
 * usage: refinery-ui [--serial DEV] [--fb DEV] [--rotate 90|270] [--fonts DIR]
 *                    [--http HOST:PORT] [--sleep MINUTES] [--audio ALSA_DEV] [--no-screen]
 * --sleep: screen powers off after this long without a key press (0 = never, default 10);
 *          any key wakes it (that key does nothing else); a fire alarm wakes it too.
 */
#include "refinery-ui.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS        250     /* housekeeping: link timeouts, camera timing, link-lost banner */
#define RESCAN_MS      2000    /* look for keypad changes / reopen the serial port this often */
#define REDRAW_MIN_MS  40      /* coalesce bursts of key presses into one redraw */

static volatile sig_atomic_t quit;
static int redraw = 1;

int64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void request_redraw(void) { redraw = 1; }

static void on_signal(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
  const char *serial = "/dev/serial0", *fbdev = "/dev/fb0", *fontdir = "/usr/share/consolefonts";
  char host[64] = "127.0.0.1";
  int port = 8000, rotate = 270, screen = 1;
  int64_t sleep_ms = 10 * 60 * 1000;

  for (int i = 1; i < argc; i++)
  {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--serial") && v)      { serial = v; i++; }
    else if (!strcmp(a, "--fb") && v)     { fbdev = v; i++; }
    else if (!strcmp(a, "--rotate") && v) { rotate = atoi(v); i++; }
    else if (!strcmp(a, "--fonts") && v)  { fontdir = v; i++; }
    else if (!strcmp(a, "--http") && v)
    {
      const char *c = strrchr(v, ':');
      if (c && (size_t)(c - v) < sizeof host) { memcpy(host, v, (size_t)(c - v)); host[c - v] = '\0'; port = atoi(c + 1); }
      i++;
    }
    else if (!strcmp(a, "--sleep") && v)  { sleep_ms = (int64_t)atoi(v) * 60 * 1000; i++; }
    else if (!strcmp(a, "--audio") && v)  { web_audio_device(v); i++; }
    else if (!strcmp(a, "--no-screen")) screen = 0;
    else { fprintf(stderr, "usage: %s [--serial DEV] [--fb DEV] [--rotate 90|270] [--fonts DIR] "
                           "[--http HOST:PORT] [--sleep MIN] [--audio DEV] [--no-screen]\n", argv[0]); return 2; }
  }
  if (rotate != 90 && rotate != 270) rotate = 270;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stderr, NULL, _IOLBF, 0);

  if (screen && (fonts_load(fontdir) != 0 || fb_open(fbdev, rotate) != 0))
  {
    fprintf(stderr, "screen: cannot open %s: %s\n", fbdev, strerror(errno));
    return 1;
  }
  if (web_open(host, port) < 0) { fprintf(stderr, "http: cannot listen on %s:%d\n", host, port); return 1; }
  fprintf(stderr, "http: listening on %s:%d\n", host, port);
  if (link_open(serial) < 0) fprintf(stderr, "serial: cannot open %s: %s (retrying)\n", serial, strerror(errno));
  if (screen) input_scan();
  snprintf(g_msg, sizeof g_msg, "Ready");

  int64_t last_tick = 0, last_scan = now_ms(), last_draw = 0, last_key = now_ms();
  int asleep = 0;
  while (!quit)
  {
    struct pollfd p[40];
    int n = 0, i_link = -1, i_web = -1, i_cam = -1, i_in = -1, n_in = 0, i_cl = -1, n_cl = 0;
    if (link_fd() >= 0) { i_link = n; p[n++] = (struct pollfd){ link_fd(), POLLIN, 0 }; }
    i_web = n; p[n++] = (struct pollfd){ web_listen_fd(), POLLIN, 0 };
    if (cam_fd() >= 0) { i_cam = n; p[n++] = (struct pollfd){ cam_fd(), (short)cam_events(), 0 }; }
    if (screen)
    {
      int fds[4];
      n_in = input_fds(fds, 4);
      i_in = n;
      for (int k = 0; k < n_in; k++) p[n++] = (struct pollfd){ fds[k], POLLIN, 0 };
    }
    {
      int fds[12];
      short ev[12];
      n_cl = web_fds(fds, ev, 12);
      i_cl = n;
      for (int k = 0; k < n_cl; k++) p[n++] = (struct pollfd){ fds[k], ev[k], 0 };
    }

    int64_t now = now_ms();
    int timeout = TICK_MS - (int)(now - last_tick);
    if (redraw && screen) { int w = REDRAW_MIN_MS - (int)(now - last_draw); timeout = w < timeout ? w : timeout; }
    if (timeout < 0) timeout = 0;
    if (poll(p, (nfds_t)n, timeout) < 0 && errno != EINTR) { perror("poll"); break; }
    now = now_ms();

    if (i_link >= 0 && p[i_link].revents)
    {
      if (link_read()) redraw = 1;
      if (p[i_link].revents & (POLLERR | POLLHUP | POLLNVAL)) { close(link_fd()); link_open(serial); }
    }
    if (p[i_web].revents & POLLIN) web_accept();
    for (int k = 0; k < n_cl; k++)
      if (p[i_cl + k].revents) web_io(p[i_cl + k].fd, p[i_cl + k].revents);
    if (i_cam >= 0 && p[i_cam].revents && cam_io(now)) redraw = 1;
    for (int k = 0; k < n_in; k++)
    {
      if (!p[i_in + k].revents) continue;
      struct keyev ev[16];
      int m = input_read(p[i_in + k].fd, ev, 16);
      if (m == 0) continue;
      last_key = now;
      if (asleep) { asleep = 0; fb_power(1); redraw = 1; continue; }   /* waking key is swallowed */
      for (int e = 0; e < m; e++) ui_key(&ev[e]);
      redraw = 1;
    }

    if (now - last_tick >= TICK_MS)
    {
      static int was_ok = -1;
      last_tick = now;
      link_tick(now);
      ui_tick(now);
      cam_tick(now);
      web_tick(now);
      int ok = link_ok(now);
      if (ok != was_ok) { was_ok = ok; redraw = 1; }
      if (screen && asleep && ui_alarm()) { asleep = 0; last_key = now; fb_power(1); redraw = 1; }
      if (screen && !asleep && sleep_ms > 0 && now - last_key >= sleep_ms && !ui_alarm())
      {
        asleep = 1;
        cam_set(0);                       /* stop fetching snapshots nobody can see */
        fb_power(0);
      }
    }
    if (now - last_scan >= RESCAN_MS)
    {
      last_scan = now;
      if (screen && input_changed())
      {
        input_scan();                     /* keypad replugged: reopen it, and count it as activity */
        last_key = now;
        if (asleep) { asleep = 0; fb_power(1); }
        redraw = 1;
      }
      if (link_fd() < 0) link_open(serial);
    }
    if (asleep) redraw = 0;               /* nothing to draw on a dark screen */
    if (screen && redraw && now - last_draw >= REDRAW_MIN_MS)
    {
      redraw = 0;
      last_draw = now;
      ui_draw(now);
    }
  }
  if (screen) fb_close();
  return 0;
}
