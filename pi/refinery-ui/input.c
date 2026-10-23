/* USB number pad via evdev.
 * NumLock off: 8/2/4/6 navigate, 5 and Enter select (REQ-UI-060). NumLock on: the digit keys type
 * numbers (timer entry). The dedicated arrow keys always navigate; Backspace is "back / delete". */
#include "refinery-ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEV 4
#define BIT(arr, b) ((arr[(b) / 8] >> ((b) % 8)) & 1)

static int devs[MAX_DEV];
static int ndev;
static int numlock;

int input_numlock(void) { return numlock; }

static void set_led(int fd, int on)
{
  struct input_event ev[2];
  memset(ev, 0, sizeof ev);
  ev[0].type = EV_LED; ev[0].code = LED_NUML; ev[0].value = on;
  ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
  if (write(fd, ev, sizeof ev) < 0) { /* LED is cosmetic */ }
}

/* Fingerprint of the /dev/input/event* names, to notice plug / unplug */
static unsigned long dev_list_hash(void)
{
  unsigned long h = 5381;
  DIR *d = opendir("/dev/input");
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL)
  {
    if (strncmp(e->d_name, "event", 5) != 0) continue;
    unsigned long k = 0;
    for (const char *p = e->d_name; *p; p++) k = k * 33 + (unsigned char)*p;
    h += k * 2654435761UL;                   /* order-independent */
  }
  closedir(d);
  return h;
}

static unsigned long scanned_hash;

/* A keyboard was plugged in or unplugged since the last scan */
int input_changed(void) { return dev_list_hash() != scanned_hash; }

/* Open every keyboard-like device that has keypad keys or arrows */
int input_scan(void)
{
  for (int i = 0; i < ndev; i++) close(devs[i]);
  ndev = 0;
  scanned_hash = dev_list_hash();
  DIR *d = opendir("/dev/input");
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && ndev < MAX_DEV)
  {
    if (strncmp(e->d_name, "event", 5) != 0) continue;
    char path[300];
    snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    uint8_t keys[KEY_MAX / 8 + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 ||
        !(BIT(keys, KEY_KP8) || BIT(keys, KEY_UP)) || !BIT(keys, KEY_ENTER))
    {
      close(fd);
      continue;
    }
    ioctl(fd, EVIOCGRAB, 1);                       /* keys belong to us, not the console */
    uint8_t leds[LED_MAX / 8 + 1] = {0};
    if (ioctl(fd, EVIOCGLED(sizeof leds), leds) >= 0 && ndev == 0) numlock = BIT(leds, LED_NUML);
    char name[64] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    fprintf(stderr, "input: %s (%s)\n", path, name);
    devs[ndev++] = fd;
  }
  closedir(d);
  return ndev;
}

int input_fds(int *fds, int max)
{
  int n = 0;
  for (int i = 0; i < ndev && n < max; i++) fds[n++] = devs[i];
  return n;
}

static void drop(int fd)
{
  for (int i = 0; i < ndev; i++)
  {
    if (devs[i] != fd) continue;
    close(fd);
    devs[i] = devs[--ndev];
    return;
  }
}

static int kp_digit(int code)
{
  static const int kp[10] = { KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4,
                              KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9 };
  for (int i = 0; i < 10; i++) if (code == kp[i]) return i;
  return -1;
}

static int top_digit(int code)
{
  if (code == KEY_0) return 0;
  if (code >= KEY_1 && code <= KEY_9) return code - KEY_1 + 1;
  return -1;
}

int input_read(int fd, struct keyev *out, int max)
{
  struct input_event ev[32];
  ssize_t n = read(fd, ev, sizeof ev);
  if (n < 0)
  {
    if (errno != EAGAIN) drop(fd);                 /* unplugged; main loop rescans */
    return 0;
  }
  int k = 0;
  for (size_t i = 0; i < (size_t)n / sizeof ev[0] && k < max; i++)
  {
    if (ev[i].type != EV_KEY || ev[i].value == 0) continue;     /* press (1) or repeat (2) */
    int code = ev[i].code, rep = ev[i].value == 2;
    struct keyev ke = { K_NONE, 0 };

    if (code == KEY_NUMLOCK)
    {
      if (rep) continue;
      numlock = !numlock;
      for (int d = 0; d < ndev; d++) set_led(devs[d], numlock);
      request_redraw();
      continue;
    }
    int dg = kp_digit(code);
    if (dg < 0) dg = top_digit(code);
    if (dg >= 0 && (numlock || kp_digit(code) < 0)) { ke.k = K_DIGIT; ke.digit = dg; }
    else if (code == KEY_UP || code == KEY_KP8)      ke.k = K_UP;
    else if (code == KEY_DOWN || code == KEY_KP2)    ke.k = K_DOWN;
    else if (code == KEY_LEFT || code == KEY_KP4)    ke.k = K_LEFT;
    else if (code == KEY_RIGHT || code == KEY_KP6)   ke.k = K_RIGHT;
    else if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_KP5) ke.k = rep ? K_NONE : K_SELECT;
    else if (code == KEY_BACKSPACE || code == KEY_ESC || code == KEY_DELETE) ke.k = K_BACK;
    if (ke.k != K_NONE) out[k++] = ke;
  }
  return k;
}
