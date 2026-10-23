/* refinery-ui: operator interface for the WVO/WMO refinery (Raspberry Pi side).
 *
 * One single-threaded poll() loop owns:
 *   - the STM32 link on /dev/serial0 (status lines in, commands out; protocol in firmware proto.h)
 *   - the USB number pad (evdev)
 *   - the HDMI screen (/dev/fb0, drawn landscape and rotated onto the portrait panel)
 *   - a small HTTP server on 127.0.0.1 for the web UI (published on the tailnet by `tailscale serve`)
 *   - webcam snapshots from ustreamer (127.0.0.1:8080) while the local camera view is open
 * The STM32 runs all control logic; this program only shows state and relays operator commands.
 */
#ifndef REFINERY_UI_H
#define REFINERY_UI_H

#include <stddef.h>
#include <stdint.h>

#define NO_READING   INT32_MIN

/* Output bits, override bits, out-of-tolerance bits: same values as firmware control.h */
enum { OUT_V1 = 1, OUT_V2 = 2, OUT_H1 = 4, OUT_H2 = 8, OUT_HNOZ = 16, OUT_CENT = 32, OUT_EXT = 64, OUT_RET = 128 };
enum { OVR_CENT = 1, OVR_NOZ = 2, OVR_V1 = 4, OVR_V2 = 8, OVR_H1 = 16, OVR_H2 = 32 };
enum { OOT_T1 = 1, OOT_T2 = 2, OOT_NOZ = 4 };
enum { WARN_FLAME_OFF = 2 };                  /* bit 0 unused (was the drain time limit) */
enum { TANK_IDLE, TANK_SETTLING, TANK_READY, TANK_DRAINING };
enum { CENT_IDLE, CENT_SPINUP, CENT_RUN, CENT_RUNOUT };
enum { FIRE_NONE, FIRE_EXTEND, FIRE_DEAD, FIRE_RETRACT, FIRE_LATCHED };

/* One $S line from the STM32 */
struct status {
  uint32_t seq, uptime;
  int32_t  temp_f10[3];      /* T1, T2, nozzle (tenths of F) or NO_READING */
  int32_t  level_x10[2];     /* T1, T2 (tenths of %) or NO_READING */
  unsigned out, flame, fire;
  struct { unsigned state, remain_s, preset, timer_s; } tank[2];
  unsigned cent, drain, noz, ovr, oot, warn;
};

/* ---- link.c: STM32 serial link ---- */
struct ack { unsigned id; int ok; char reason[48]; char cmd[48]; };
#define N_ACKS 8

extern struct status g_st;        /* latest status */
extern int      g_have_status;    /* a status line has ever arrived */
extern int64_t  g_status_ms;      /* when it arrived (monotonic ms) */
extern struct ack g_acks[N_ACKS]; /* ring of recent command results, newest at g_ack_head-1 */
extern unsigned g_ack_head;
extern char     g_msg[96];        /* one-line feedback for the operator */

int  link_open(const char *dev);              /* returns fd or -1 */
int  link_fd(void);
int  link_read(void);                         /* returns 1 if anything new arrived (status or ack) */
void link_tick(int64_t now);                  /* retries / timeouts for queued commands */
/* Queue a command body such as "START,1". Returns its id, or 0 if the queue is full. */
unsigned link_send(const char *body);
int  link_ok(int64_t now);                    /* status seen within the last few seconds */

/* ---- fb.c: framebuffer drawing (logical landscape canvas, RGB565) ---- */
#define W 800
#define H 480
typedef uint16_t px_t;
#define RGB(r, g, b) ((px_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum { FONT_S, FONT_M, FONT_L, N_FONTS };   /* 8x16, 12x24, 16x32 */

int  fb_open(const char *dev, int rotate);   /* rotate: 90 or 270 */
void fb_close(void);
px_t *fb_canvas(void);
void fb_flush(void);
void fb_power(int on);                        /* screen sleep: blank the HDMI output */
void fill(int x, int y, int w, int h, px_t c);
void outline(int x, int y, int w, int h, int t, px_t c);
int  text(int x, int y, int font, px_t c, const char *s);   /* returns width drawn */
int  text_w(int font, const char *s);
int  font_h(int font);
int  fonts_load(const char *dir);

/* ---- input.c: number pad ---- */
enum key { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SELECT, K_BACK, K_DIGIT };
struct keyev { enum key k; int digit; };
int  input_scan(void);                        /* (re)open keypads; returns count */
int  input_changed(void);                     /* input devices plugged / unplugged since the scan */
int  input_fds(int *fds, int max);
int  input_read(int fd, struct keyev *ev, int max);   /* returns number of events */
int  input_numlock(void);

/* ---- cam.c: webcam snapshots ---- */
void cam_set(int on);
int  cam_on(void);
int  cam_fd(void);                            /* socket in progress or -1 */
int  cam_events(void);                        /* poll events wanted on cam_fd */
int  cam_io(int64_t now);                     /* returns 1 if a new frame was decoded */
void cam_tick(int64_t now);
const px_t *cam_frame(int *w, int *h);        /* latest decoded frame, or NULL */

/* ---- ui.c: local screen ---- */
void ui_key(const struct keyev *ev);
void ui_draw(int64_t now);
void ui_tick(int64_t now);                    /* samples the temperature history */
int32_t ui_hist(int series, unsigned back);   /* temp history (0 = newest), NO_READING if none */
int  ui_alarm(void);                          /* fire fault showing: keep the screen awake */

/* ---- web.c: HTTP server ---- */
int  web_open(const char *host, int port);
int  web_listen_fd(void);
int  web_fds(int *fds, short *events, int max);
void web_accept(void);
void web_io(int fd, short revents);
void web_tick(int64_t now);
void web_audio_device(const char *dev);

/* ---- main.c ---- */
int64_t now_ms(void);
void request_redraw(void);

/* Shared formatting helpers (ui.c) */
void fmt_temp(char *b, size_t n, int32_t f10);
void fmt_hms(char *b, size_t n, unsigned s);
const char *tank_state_name(unsigned s);
const char *cent_state_name(unsigned s);
const char *fire_state_name(unsigned s);

#endif /* REFINERY_UI_H */
