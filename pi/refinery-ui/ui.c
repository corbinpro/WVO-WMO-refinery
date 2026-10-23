/* Local screen: layout, focus navigation, dropdowns, timer entry (REQ-UI-010..065).
 * Look follows btop: black background, rounded boxes in per-box colours with the title set into
 * the top border, braille-dot graphs and meters, gradient-coloured readings. */
#include "refinery-ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Colours (btop "Default" theme where it has one) */
#define C_BG      RGB(0, 0, 0)
#define C_TEXT    RGB(0xcc, 0xcc, 0xcc)    /* main_fg */
#define C_TITLE   RGB(0xee, 0xee, 0xee)
#define C_DIM     RGB(0x80, 0x80, 0x80)    /* labels */
#define C_DOT     RGB(0x30, 0x30, 0x30)    /* empty braille dots, div lines */
#define C_SEL     RGB(0x6a, 0x2f, 0x2f)    /* selected_bg */
#define C_BOX_T1  RGB(0x3d, 0x7b, 0x46)    /* cpu_box */
#define C_BOX_T2  RGB(0x8a, 0x88, 0x2e)    /* mem_box */
#define C_BOX_CF  RGB(0x42, 0x3b, 0xa5)    /* net_box */
#define C_BOX_FI  RGB(0x92, 0x35, 0x35)    /* proc_box */
#define C_EDIT    RGB(70, 140, 220)        /* outline of a timer being typed */
#define C_ON      RGB(80, 210, 110)
#define C_HEAT    RGB(255, 150, 40)
#define C_WARN    RGB(255, 200, 40)
#define C_BAD     RGB(230, 60, 50)
#define C_FOCUS   RGB(255, 255, 255)

enum wkind { WK_BUTTON, WK_DROP, WK_TIMER };
enum wid {
  W_T1_TEMP, W_T1_TIMER, W_T1_START, W_T1_CANCEL, W_T1_VALVE, W_T1_HEAT,
  W_T2_TEMP, W_T2_TIMER, W_T2_START, W_T2_CANCEL, W_T2_VALVE, W_T2_HEAT,
  W_CENT, W_NOZ_SET, W_NOZ_HTR, W_CAMERA,
  N_MAIN,
  W_FIRE_ACK = N_MAIN, W_CAM_CLOSE,
  N_WIDGETS
};

struct widget { int x, y, w, h; enum wkind kind; const char *label; };

#define TX0 2        /* tank 1 box */
#define TX1 402      /* tank 2 box */
#define TW  396
#define TY  8
#define TH  288
#define CY  312      /* centrifuge box */
#define CH  160

static const struct widget widgets[N_WIDGETS] = {
  [W_T1_TEMP]   = { TX0 + 8,   TY + 160, 186, 52, WK_DROP,   "set temp" },
  [W_T1_TIMER]  = { TX0 + 202, TY + 160, 186, 52, WK_TIMER,  "timer hh:mm" },
  [W_T1_START]  = { TX0 + 8,   TY + 222, 89, 52, WK_BUTTON, "" },
  [W_T1_CANCEL] = { TX0 + 105, TY + 222, 89, 52, WK_BUTTON, "" },
  [W_T1_VALVE]  = { TX0 + 202, TY + 222, 89, 52, WK_BUTTON, "valves" },
  [W_T1_HEAT]   = { TX0 + 299, TY + 222, 89, 52, WK_BUTTON, "heater" },
  [W_T2_TEMP]   = { TX1 + 8,   TY + 160, 186, 52, WK_DROP,   "set temp" },
  [W_T2_TIMER]  = { TX1 + 202, TY + 160, 186, 52, WK_TIMER,  "timer hh:mm" },
  [W_T2_START]  = { TX1 + 8,   TY + 222, 89, 52, WK_BUTTON, "" },
  [W_T2_CANCEL] = { TX1 + 105, TY + 222, 89, 52, WK_BUTTON, "" },
  [W_T2_VALVE]  = { TX1 + 202, TY + 222, 89, 52, WK_BUTTON, "valves" },
  [W_T2_HEAT]   = { TX1 + 299, TY + 222, 89, 52, WK_BUTTON, "heater" },
  [W_CENT]      = { 10,  CY + 94, 186, 52, WK_BUTTON, "centrifuge" },
  [W_NOZ_SET]   = { 205, CY + 94, 186, 52, WK_DROP,   "nozzle target" },
  [W_NOZ_HTR]   = { 400, CY + 94, 186, 52, WK_BUTTON, "nozzle heater" },
  [W_CAMERA]    = { 595, CY + 94, 195, 52, WK_BUTTON, "" },
  [W_FIRE_ACK]  = { 250, 380, 300, 56, WK_BUTTON, "" },
  [W_CAM_CLOSE] = { 8, 416, 144, 52, WK_BUTTON, "" },
};

static int focus = W_T1_START;
static int main_focus = W_T1_START;     /* restored after the fire / camera screens */

/* Open dropdown */
static int drop_open = -1;               /* widget id or -1 */
static int drop_sel;
static const unsigned temp_opts[] = { 0, 120, 160, 175 };
static const unsigned noz_opts[]  = { 160, 170, 180 };

/* Timer entry */
static int edit_w = -1;                  /* widget being typed into, or -1 */
static char edit_digits[5];              /* up to 4 digits, filled from the right like a microwave */

/* ---- formatting ---- */

void fmt_temp(char *b, size_t n, int32_t f10)
{
  if (f10 == NO_READING) { snprintf(b, n, "-- F"); return; }
  int32_t a = f10 < 0 ? -f10 : f10;
  snprintf(b, n, "%s%d.%d F", f10 < 0 ? "-" : "", (int)(a / 10), (int)(a % 10));
}

void fmt_hms(char *b, size_t n, unsigned s)
{
  snprintf(b, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

const char *tank_state_name(unsigned s)
{
  static const char *n[] = { "idle", "settling", "ready", "draining" };
  return s < 4 ? n[s] : "?";
}

const char *cent_state_name(unsigned s)
{
  static const char *n[] = { "stopped", "spinning up", "running", "run-out" };
  return s < 4 ? n[s] : "?";
}

const char *fire_state_name(unsigned s)
{
  static const char *n[] = { "", "extinguisher firing", "extinguisher pause", "extinguisher retracting",
                             "latched" };
  return s < 5 ? n[s] : "?";
}

static void fmt_preset(char *b, size_t n, unsigned f)
{
  if (f == 0) snprintf(b, n, "no heat");
  else        snprintf(b, n, "%u F", f);
}

/* ---- screen modes ---- */

static int fire_mode(void) { return g_have_status && g_st.fire != FIRE_NONE; }

int ui_alarm(void) { return fire_mode(); }

static int widget_enabled(int w)
{
  if (fire_mode()) return w == W_FIRE_ACK;
  if (cam_on())    return w == W_CAM_CLOSE;
  return w < N_MAIN;
}

/* ---- commands ---- */

static void send_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_cmd(const char *fmt, ...)
{
  char body[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  if (!link_send(body)) snprintf(g_msg, sizeof g_msg, "Busy - try again");
}

static int widget_tank(int w) { return (w >= W_T2_TEMP && w <= W_T2_HEAT) ? 2 : 1; }

static void toggle_override(const char *name, unsigned bit)
{
  send_cmd("OVR,%s,%s", name, (g_st.ovr & bit) ? "OFF" : "ON");
}

static void activate(int w)
{
  int t = widget_tank(w);
  switch (w)
  {
    case W_T1_TEMP: case W_T2_TEMP:
      drop_open = w;
      drop_sel = 0;
      for (int i = 0; i < 4; i++) if (temp_opts[i] == g_st.tank[t - 1].preset) drop_sel = i;
      break;
    case W_NOZ_SET:
      drop_open = w;
      drop_sel = 1;
      for (int i = 0; i < 3; i++) if (noz_opts[i] == g_st.noz) drop_sel = i;
      break;
    case W_T1_TIMER: case W_T2_TIMER:
      edit_w = w;
      edit_digits[0] = '\0';
      if (!input_numlock()) snprintf(g_msg, sizeof g_msg, "Turn NumLock on to type the time");
      break;
    case W_T1_START: case W_T2_START:   send_cmd("START,%d", t); break;
    case W_T1_CANCEL: case W_T2_CANCEL: send_cmd("CANCEL,%d", t); break;
    case W_T1_VALVE: toggle_override("V1", OVR_V1); break;
    case W_T2_VALVE: toggle_override("V2", OVR_V2); break;
    case W_T1_HEAT:  toggle_override("H1", OVR_H1); break;
    case W_T2_HEAT:  toggle_override("H2", OVR_H2); break;
    case W_CENT:     toggle_override("CENT", OVR_CENT); break;
    case W_NOZ_HTR:  toggle_override("NOZ", OVR_NOZ); break;
    case W_CAMERA:
      main_focus = focus;
      cam_set(1);
      focus = W_CAM_CLOSE;
      break;
    case W_CAM_CLOSE:
      cam_set(0);
      focus = main_focus;
      break;
    case W_FIRE_ACK:
      if (g_st.fire == FIRE_LATCHED) send_cmd("FIREACK");
      else snprintf(g_msg, sizeof g_msg, "Wait for the extinguisher sequence to finish");
      break;
  }
}

static void commit_timer(void)
{
  int v = atoi(edit_digits);
  unsigned hh = (unsigned)v / 100, mm = (unsigned)v % 100;
  int t = widget_tank(edit_w);
  edit_w = -1;
  if (edit_digits[0] == '\0') return;
  if (mm > 59)            { snprintf(g_msg, sizeof g_msg, "Minutes must be 00-59"); return; }
  if (hh == 0 && mm == 0) { snprintf(g_msg, sizeof g_msg, "Timer must be at least 00:01"); return; }
  send_cmd("TIME,%d,%u", t, (hh * 60 + mm) * 60);
}

/* Nearest enabled widget in a direction: distance along the direction plus 3x the sideways offset */
static int neighbour(int from, int dx, int dy)
{
  const struct widget *a = &widgets[from];
  int ax = a->x + a->w / 2, ay = a->y + a->h / 2;
  int best = -1, best_cost = 1 << 30;
  for (int i = 0; i < N_WIDGETS; i++)
  {
    if (i == from || !widget_enabled(i)) continue;
    const struct widget *b = &widgets[i];
    int ddx = b->x + b->w / 2 - ax, ddy = b->y + b->h / 2 - ay;
    int along = dx ? ddx * dx : ddy * dy;
    int side = abs(dx ? ddy : ddx);
    if (along <= 0) continue;
    int cost = along + 3 * side;
    if (cost < best_cost) { best_cost = cost; best = i; }
  }
  return best < 0 ? from : best;
}

void ui_key(const struct keyev *ev)
{
  /* Make sure focus is on a widget that exists in the current mode */
  if (!widget_enabled(focus))
  {
    if (fire_mode()) { if (focus < N_MAIN) main_focus = focus; focus = W_FIRE_ACK; }
    else if (cam_on()) focus = W_CAM_CLOSE;
    else focus = main_focus;
    drop_open = edit_w = -1;
  }

  if (drop_open >= 0)                    /* REQ-UI-064/065: 8/2 move, 4/6 nothing, select commits */
  {
    int n = (drop_open == W_NOZ_SET) ? 3 : 4;
    if (ev->k == K_UP && drop_sel > 0) drop_sel--;
    else if (ev->k == K_DOWN && drop_sel < n - 1) drop_sel++;
    else if (ev->k == K_BACK) drop_open = -1;
    else if (ev->k == K_SELECT)
    {
      if (drop_open == W_NOZ_SET) send_cmd("NOZ,%u", noz_opts[drop_sel]);
      else send_cmd("TEMP,%d,%u", widget_tank(drop_open), temp_opts[drop_sel]);
      drop_open = -1;
    }
    return;
  }

  if (edit_w >= 0)
  {
    size_t len = strlen(edit_digits);
    if (ev->k == K_DIGIT)
    {
      if (len < 4) { edit_digits[len] = (char)('0' + ev->digit); edit_digits[len + 1] = '\0'; }
      return;
    }
    if (ev->k == K_BACK)
    {
      if (len > 0) edit_digits[len - 1] = '\0';
      else edit_w = -1;
      return;
    }
    if (ev->k == K_SELECT) { commit_timer(); return; }
    edit_w = -1;                         /* an arrow cancels the edit and moves on */
  }

  switch (ev->k)
  {
    case K_UP:     focus = neighbour(focus, 0, -1); break;
    case K_DOWN:   focus = neighbour(focus, 0, 1);  break;
    case K_LEFT:   focus = neighbour(focus, -1, 0); break;
    case K_RIGHT:  focus = neighbour(focus, 1, 0);  break;
    case K_SELECT: activate(focus); break;
    case K_BACK:   if (focus == W_CAM_CLOSE) activate(focus); break;
    case K_DIGIT:                         /* typing on a timer starts the edit directly */
      if (widgets[focus].kind == WK_TIMER)
      {
        activate(focus);
        ui_key(ev);
      }
      break;
    default: break;
  }
}

/* ---- history (for the braille graphs) ---- */

#define HIST_N     200
#define HIST_MS    10000          /* one sample every 10 s: tank graphs ~16 min, nozzle ~32 min */
#define GRAPH_LO   400            /* graph range, tenths of F */
#define GRAPH_HI   2000

static int32_t hist[3][HIST_N];
static unsigned hist_n;           /* samples taken so far; newest is hist[..][(hist_n - 1) % HIST_N] */

void ui_tick(int64_t now)
{
  static int64_t last;
  int ok = link_ok(now);
  if (!ok && hist_n == 0) return;                /* don't start the graph with a gap */
  if (hist_n && now - last < HIST_MS) return;
  last = now;
  for (int i = 0; i < 3; i++) hist[i][hist_n % HIST_N] = ok ? g_st.temp_f10[i] : NO_READING;
  hist_n++;
}

int32_t ui_hist(int series, unsigned back)
{
  if (back >= hist_n || back >= HIST_N) return NO_READING;
  return hist[series][(hist_n - 1 - back) % HIST_N];
}

/* ---- drawing primitives ---- */

static void center_text(int x, int y, int w, int font, px_t c, const char *s)
{
  text(x + (w - text_w(font, s)) / 2, y, font, c, s);
}

static int text_r(int right, int y, int font, px_t c, const char *s)   /* right-aligned; returns left x */
{
  int x = right - text_w(font, s);
  text(x, y, font, c, s);
  return x;
}

/* Blend two RGB565 colours, t = 0..256 */
static px_t mix(px_t a, px_t b, int t)
{
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (px_t)(((ar + (br - ar) * t / 256) << 11) | ((ag + (bg - ag) * t / 256) << 5) | (ab + (bb - ab) * t / 256));
}

/* Three-stop gradient, t = 0..1000 */
static px_t grad3(px_t a, px_t b, px_t c, int t)
{
  if (t < 0) t = 0;
  if (t > 1000) t = 1000;
  return t < 500 ? mix(a, b, t * 256 / 500) : mix(b, c, (t - 500) * 256 / 500);
}

/* btop temp gradient (blue -> purple -> pink) by temperature */
static px_t temp_grad(int t) { return grad3(RGB(0x48, 0x97, 0xd4), RGB(0x54, 0x74, 0xe8), RGB(0xff, 0x40, 0xb6), t); }
/* btop free gradient (greens) for tank level */
static px_t level_grad(int t) { return grad3(RGB(0x38, 0x4f, 0x21), RGB(0xb5, 0xe6, 0x85), RGB(0xdc, 0xff, 0x85), t); }

static px_t temp_color(int32_t f10)
{
  if (f10 == NO_READING) return C_DIM;
  return temp_grad((int)((f10 - GRAPH_LO) * 1000 / (GRAPH_HI - GRAPH_LO)));
}

/* Rounded box, radius 6, line thickness th */
static void rbox(int x, int y, int w, int h, int th, px_t c)
{
  enum { R = 6 };
  static const int off[R + 1] = { 6, 3, 2, 1, 1, 0, 0 };   /* inset of the arc at each step */
  for (int k = 0; k < th; k++, x++, y++, w -= 2, h -= 2)
  {
    fill(x + R, y, w - 2 * R, 1, c);
    fill(x + R, y + h - 1, w - 2 * R, 1, c);
    fill(x, y + R, 1, h - 2 * R, c);
    fill(x + w - 1, y + R, 1, h - 2 * R, c);
    for (int i = 0; i <= R; i++)
    {
      int o = off[i];
      fill(x + o, y + i, 1, 1, c);  fill(x + i, y + o, 1, 1, c);                         /* top-left */
      fill(x + w - 1 - o, y + i, 1, 1, c);  fill(x + w - 1 - i, y + o, 1, 1, c);         /* top-right */
      fill(x + o, y + h - 1 - i, 1, 1, c);  fill(x + i, y + h - 1 - o, 1, 1, c);         /* bottom-left */
      fill(x + w - 1 - o, y + h - 1 - i, 1, 1, c);  fill(x + w - 1 - i, y + h - 1 - o, 1, 1, c);
    }
  }
}

/* Text set into a border line like btop's ┐title┌ (top) or ┘title└ (bottom) */
static void border_label(int tx, int y, int top, px_t line, px_t tc, const char *s)
{
  int tw = text_w(FONT_S, s);
  fill(tx - 5, y, tw + 10, 1, C_BG);
  fill(tx - 5, top ? y : y - 4, 1, 5, line);
  fill(tx + tw + 4, top ? y : y - 4, 1, 5, line);
  text(tx, y - font_h(FONT_S) / 2, FONT_S, tc, s);
}

static void border_label_r(int right, int y, int top, px_t line, px_t tc, const char *s)
{
  border_label(right - text_w(FONT_S, s), y, top, line, tc, s);
}

/* Braille cells: 2x4 dots in an 8x16 cell, i.e. one dot every 4 px. Dots are 2x2. */
#define DOT(x, y, c) fill((x) + 1, (y) + 1, 2, 2, (c))

/* History graph, newest sample at the right edge. Setpoint (if any) as a dim dotted line. */
static void braille_graph(int x, int y, int w, int h, const int32_t *v, int32_t setpoint)
{
  int cols = w / 4, rows = h / 4;
  int sp_row = setpoint > 0 ? (int)((setpoint - GRAPH_LO) * rows / (GRAPH_HI - GRAPH_LO)) : -1;
  for (int c = 0; c < cols; c++)
  {
    int back = cols - 1 - c;                     /* samples before the newest */
    int32_t val = NO_READING;
    if ((unsigned)back < hist_n && back < HIST_N) val = v[(hist_n - 1 - (unsigned)back) % HIST_N];
    int filled = 0;
    if (val != NO_READING)
    {
      filled = (int)((val - GRAPH_LO) * rows / (GRAPH_HI - GRAPH_LO));
      if (filled < 1) filled = 1;
      if (filled > rows) filled = rows;
    }
    for (int r = 0; r < rows; r++)
    {
      int py = y + h - 4 - r * 4;
      if (r < filled) DOT(x + c * 4, py, temp_grad(r * 1000 / rows));
      else if (r == sp_row && (c & 1) == 0) DOT(x + c * 4, py, C_DIM);
    }
  }
}

/* Horizontal braille meter, one cell (4 dot rows) tall */
static void braille_meter(int x, int y, int w, int permille, px_t (*grad)(int))
{
  int cols = w / 4;
  int filled = permille <= 0 ? 0 : permille >= 1000 ? cols : (permille * cols + 500) / 1000;
  for (int c = 0; c < cols; c++)
  {
    px_t col = c < filled ? grad(c * 1000 / cols) : C_DOT;
    for (int r = 0; r < 4; r++) DOT(x + c * 4, y + r * 4, col);
  }
}

/* ---- widgets ---- */

static void draw_widget(int id, const char *value, px_t vc, int dim, px_t box)
{
  const struct widget *w = &widgets[id];
  if (id == focus)                               /* selected: btop highlight + white outline (REQ-UI-062) */
  {
    fill(w->x + 2, w->y + 2, w->w - 4, w->h - 4, C_SEL);
    rbox(w->x, w->y, w->w, w->h, 2, C_FOCUS);
  }
  else rbox(w->x, w->y, w->w, w->h, id == edit_w ? 2 : 1, id == edit_w ? C_EDIT : box);
  px_t v = dim ? C_DIM : vc;
  if (w->label[0])
  {
    text(w->x + 8, w->y + 4, FONT_S, C_DIM, w->label);
    center_text(w->x, w->y + w->h - font_h(FONT_M) - 5, w->w, FONT_M, v, value);
  }
  else center_text(w->x, w->y + (w->h - font_h(FONT_M)) / 2, w->w, FONT_M, v, value);
  if (w->kind == WK_DROP) text(w->x + w->w - 16, w->y + w->h - 22, FONT_S, C_DIM, "v");
}

static void draw_dropdown(void)
{
  const struct widget *w = &widgets[drop_open];
  int n = (drop_open == W_NOZ_SET) ? 3 : 4;
  int ih = 40, h = n * ih + 12;
  int y = w->y + w->h + 4;
  if (y + h > H) y = w->y - h - 4;               /* open upward near the bottom */
  fill(w->x - 4, y - 4, w->w + 8, h + 8, C_BG);
  rbox(w->x, y, w->w, h, 1, C_TITLE);
  for (int i = 0; i < n; i++)
  {
    char b[16];
    fmt_preset(b, sizeof b, drop_open == W_NOZ_SET ? noz_opts[i] : temp_opts[i]);
    int iy = y + 6 + i * ih;
    if (i == drop_sel)
    {
      fill(w->x + 6, iy, w->w - 12, ih - 4, C_SEL);
      rbox(w->x + 4, iy - 2, w->w - 8, ih, 2, C_FOCUS);
    }
    center_text(w->x, iy + (ih - 4 - font_h(FONT_M)) / 2, w->w, FONT_M, C_TITLE, b);
  }
}

/* ---- screens ---- */

static void draw_tank(int t, int bx, int ok)
{
  const struct status *s = &g_st;
  px_t box = t == 0 ? C_BOX_T1 : C_BOX_T2;
  int base = t == 0 ? W_T1_TEMP : W_T2_TEMP;
  unsigned hbit = t == 0 ? OVR_H1 : OVR_H2, vbit = t == 0 ? OVR_V1 : OVR_V2;
  char b[40], tm[16];

  rbox(bx, TY, TW, TH, 1, box);
  snprintf(b, sizeof b, "tank %d", t + 1);
  border_label(bx + 14, TY, 1, box, C_TITLE, b);

  unsigned st = s->tank[t].state;
  px_t sc = st == TANK_SETTLING ? C_HEAT : st == TANK_READY ? C_WARN : st == TANK_DRAINING ? C_ON : C_DIM;
  if (!ok) { snprintf(b, sizeof b, "no data"); sc = C_BAD; }
  else if (st == TANK_SETTLING) { fmt_hms(tm, sizeof tm, s->tank[t].remain_s); snprintf(b, sizeof b, "settling %s", tm); }
  else if (st == TANK_READY && s->drain != 0) snprintf(b, sizeof b, "ready - waiting");
  else snprintf(b, sizeof b, "%s", tank_state_name(st));
  border_label_r(bx + TW - 14, TY, 1, box, sc, b);

  /* Reading, setpoint, heater */
  int32_t tf = ok ? s->temp_f10[t] : NO_READING;
  int oot = ok && (s->oot & (t == 0 ? OOT_T1 : OOT_T2));
  fmt_temp(b, sizeof b, tf);
  int tw = text(bx + 12, TY + 14, FONT_L, oot ? C_WARN : temp_color(tf), b);
  if (s->tank[t].preset) snprintf(b, sizeof b, "set %u F", s->tank[t].preset);
  else snprintf(b, sizeof b, "no heat");
  text(bx + 22 + tw, TY + 28, FONT_S, C_DIM, b);
  int heating = ok && (s->out & (t == 0 ? OUT_H1 : OUT_H2));
  text_r(bx + TW - 12, TY + 16, FONT_S, heating ? C_HEAT : C_DIM, heating ? "heater on" : "heater off");
  if (oot) text_r(bx + TW - 12, TY + 32, FONT_S, C_WARN, "out of tolerance");

  /* Temperature history (dim dotted line = setpoint) */
  braille_graph(bx + 8, TY + 56, TW - 16, 64, hist[t], s->tank[t].preset * 10);

  /* Level */
  text(bx + 12, TY + 130, FONT_S, C_DIM, "level");
  int32_t lv = ok ? s->level_x10[t] : NO_READING;
  braille_meter(bx + 64, TY + 130, 244, lv == NO_READING ? 0 : lv, level_grad);
  if (lv == NO_READING) snprintf(b, sizeof b, "no sender");
  else { int32_t a = lv < 0 ? -lv : lv; snprintf(b, sizeof b, "%s%d.%d%%", lv < 0 ? "-" : "", (int)(a / 10), (int)(a % 10)); }
  text_r(bx + TW - 12, TY + 130, FONT_S, lv == NO_READING ? C_BAD : C_TEXT, b);

  /* Controls */
  fmt_preset(b, sizeof b, s->tank[t].preset);
  draw_widget(base + 0, ok ? b : "--", C_TITLE, 0, box);
  if (edit_w == base + 1)
  {
    char d[5] = "0000";
    size_t len = strlen(edit_digits);
    memcpy(d + 4 - len, edit_digits, len);
    snprintf(tm, sizeof tm, "%c%c:%c%c", d[0], d[1], d[2], d[3]);
  }
  else snprintf(tm, sizeof tm, "%02u:%02u", s->tank[t].timer_s / 3600, (s->tank[t].timer_s / 60) % 60);
  draw_widget(base + 1, ok || edit_w == base + 1 ? tm : "--", C_TITLE, 0, box);
  draw_widget(base + 2, "start", C_ON, st != TANK_IDLE, box);
  draw_widget(base + 3, "cancel", C_BAD, st == TANK_IDLE, box);
  int open = ok && (s->out & (t == 0 ? OUT_V1 : OUT_V2));
  snprintf(b, sizeof b, "%s%s", (s->ovr & vbit) ? "on" : "auto", open ? "*" : "");
  draw_widget(base + 4, b, (s->ovr & vbit) ? C_ON : C_TITLE, 0, box);
  draw_widget(base + 5, (s->ovr & hbit) ? "on" : "auto", (s->ovr & hbit) ? C_HEAT : C_TITLE, 0, box);
}

static void draw_centrifuge(int ok)
{
  const struct status *s = &g_st;
  px_t box = C_BOX_CF;
  char b[96];

  rbox(2, CY, 796, CH, 1, box);
  border_label(16, CY, 1, box, C_TITLE, "centrifuge");
  border_label_r(784, CY, 1, box, ok && s->cent != CENT_IDLE ? C_ON : (ok ? C_DIM : C_BAD),
                 ok ? cent_state_name(s->cent) : "no data");

  int32_t tn = ok ? s->temp_f10[2] : NO_READING;
  text(12, CY + 18, FONT_S, C_DIM, "nozzle");
  fmt_temp(b, sizeof b, tn);
  int x = 72 + text(72, CY + 12, FONT_M, (ok && (s->oot & OOT_NOZ)) ? C_WARN : temp_color(tn), b);
  snprintf(b, sizeof b, "target %u F", s->noz);
  text(x + 12, CY + 18, FONT_S, C_DIM, b);
  if (ok && (s->out & OUT_HNOZ)) text(x + 140, CY + 18, FONT_S, C_HEAT, "heater on");
  if (ok && s->drain) { snprintf(b, sizeof b, "draining tank %u", s->drain); text_r(786, CY + 12, FONT_M, C_ON, b); }

  braille_graph(10, CY + 44, 780, 40, hist[2], ok && s->drain ? (int32_t)s->noz * 10 : 0);

  draw_widget(W_CENT, (s->ovr & OVR_CENT) ? "on" : "auto", (s->ovr & OVR_CENT) ? C_ON : C_TITLE, 0, box);
  snprintf(b, sizeof b, "%u F", s->noz);
  draw_widget(W_NOZ_SET, ok ? b : "--", C_TITLE, 0, box);
  draw_widget(W_NOZ_HTR, (s->ovr & OVR_NOZ) ? "on" : "auto", (s->ovr & OVR_NOZ) ? C_HEAT : C_TITLE, 0, box);
  draw_widget(W_CAMERA, "camera", C_TITLE, 0, box);

  /* Bottom border: last message on the left, flags on the right */
  int y = CY + CH - 1;
  if (g_msg[0]) { snprintf(b, sizeof b, "%.48s", g_msg); border_label(16, y, 0, box, C_DIM, b); }
  int r = 784;
  if (input_numlock()) { border_label_r(r, y, 0, box, C_WARN, "num"); r -= text_w(FONT_S, "num") + 16; }
  if (!ok) border_label_r(r, y, 0, box, C_BAD, g_have_status ? "no data from controller" : "waiting for controller");
  else if (s->warn & WARN_FLAME_OFF) border_label_r(r, y, 0, box, C_WARN, "fire detection off");
}

static void draw_fire(void)
{
  fill(0, 0, W, H, RGB(40, 0, 0));
  rbox(2, 8, W - 4, H - 12, 2, C_BOX_FI);
  border_label(16, 8, 1, C_BOX_FI, C_BAD, "fire");
  border_label_r(W - 16, 8, 1, C_BOX_FI, C_WARN, fire_state_name(g_st.fire));
  center_text(0, 50, W, FONT_L, C_BAD, "FIRE DETECTED");
  center_text(0, 104, W, FONT_M, C_TEXT, "heaters, centrifuge and valves are off");
  center_text(0, 200, W, FONT_L, C_TITLE, "Replace fire extinguisher");
  center_text(0, 244, W, FONT_L, C_TITLE, "and clean machine");        /* REQ-SAFE-050 */
  draw_widget(W_FIRE_ACK, g_st.fire == FIRE_LATCHED ? "acknowledge" : "please wait", C_TITLE,
              g_st.fire != FIRE_LATCHED, C_BOX_FI);
  if (g_msg[0]) border_label(16, H - 5, 0, C_BOX_FI, C_DIM, g_msg);
}

static void draw_camera(int ok)
{
  const struct status *s = &g_st;
  char b[32];
  fill(0, 0, W, H, C_BG);
  int fw, fh;
  const px_t *f = cam_frame(&fw, &fh);
  int ox = 160 + (640 - fw) / 2, oy = (H - fh) / 2;
  if (f)
  {
    px_t *cv = fb_canvas();
    for (int y = 0; y < fh && oy + y < H; y++)
      memcpy(cv + (size_t)(oy + y) * W + ox, f + (size_t)y * (size_t)fw, (size_t)(fw < 640 ? fw : 640) * sizeof(px_t));
  }
  else center_text(160, H / 2 - 12, 640, FONT_M, C_DIM, "waiting for camera...");

  rbox(2, 8, 152, H - 12, 1, C_BOX_CF);
  border_label(14, 8, 1, C_BOX_CF, C_TITLE, "status");
  int y = 24;
  for (int t = 0; t < 2; t++)
  {
    int32_t tf = ok ? s->temp_f10[t] : NO_READING;
    snprintf(b, sizeof b, "tank %d", t + 1);
    text(12, y, FONT_S, C_DIM, b); y += 18;
    fmt_temp(b, sizeof b, tf);
    text(12, y, FONT_M, temp_color(tf), b); y += 26;
    text(12, y, FONT_S, C_TEXT, ok ? tank_state_name(s->tank[t].state) : "--"); y += 26;
  }
  int32_t tn = ok ? s->temp_f10[2] : NO_READING;
  text(12, y, FONT_S, C_DIM, "nozzle"); y += 18;
  fmt_temp(b, sizeof b, tn);
  text(12, y, FONT_M, temp_color(tn), b); y += 32;
  text(12, y, FONT_S, C_DIM, "centrifuge"); y += 18;
  text(12, y, FONT_S, ok && s->cent ? C_ON : C_TEXT, ok ? cent_state_name(s->cent) : "--");
  draw_widget(W_CAM_CLOSE, "close", C_TITLE, 0, C_BOX_CF);
}

void ui_draw(int64_t now)
{
  int ok = link_ok(now);
  if (fire_mode() && focus != W_FIRE_ACK)
  {
    if (focus < N_MAIN) main_focus = focus;
    focus = W_FIRE_ACK;
    drop_open = edit_w = -1;
  }
  else if (!fire_mode() && focus == W_FIRE_ACK) focus = cam_on() ? W_CAM_CLOSE : main_focus;
  else if (!cam_on() && focus == W_CAM_CLOSE) focus = main_focus;   /* camera closed by screen sleep */

  if (fire_mode()) draw_fire();
  else if (cam_on()) draw_camera(ok);
  else
  {
    fill(0, 0, W, H, C_BG);
    draw_tank(0, TX0, ok);
    draw_tank(1, TX1, ok);
    draw_centrifuge(ok);
    if (drop_open >= 0) draw_dropdown();
  }
  fb_flush();
}
