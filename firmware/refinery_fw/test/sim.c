/* Rig simulator: runs the real control.c + proto.c behind a pseudo-terminal, with fake physics,
 * so refinery-ui can be tested without the STM32.
 *   gcc -std=gnu99 -O2 -I../Core/Inc sim.c ../Core/Src/control.c ../Core/Src/proto.c -o sim -lm
 *   ./sim [speed]          prints the pty path; then: refinery-ui --serial <path>
 * speed = time multiplier (default 1; 60 makes a 1-hour settle take a minute).
 * Send SIGUSR1 to simulate a flame for 3 s. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "control.h"
#include "proto.h"

static volatile sig_atomic_t flame_req;
static void on_usr1(int s) { (void)s; flame_req = 1; }

int main(int argc, char **argv)
{
  double speed = argc > 1 ? atof(argv[1]) : 1.0;
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); return 1; }
  struct termios t;
  tcgetattr(m, &t);
  cfmakeraw(&t);
  tcsetattr(m, TCSANOW, &t);
  fcntl(m, F_SETFL, O_NONBLOCK);
  printf("%s\n", ptsname(m));
  fflush(stdout);
  signal(SIGUSR1, on_usr1);

  ctl_t c;
  ctl_init(&c, NULL);
  c.flame_off = 0;
  ctl_inputs_t in = { .temp_f10 = { 700, 680, 650 }, .level_x10 = { 850, 600 } };
  double temp[3] = { 70, 68, 65 }, level[2] = { 85, 60 };
  char rx[PROTO_LINE_MAX], line[256];
  size_t rlen = 0;
  uint32_t now = 0, tick = 0, last_status = 0, seq = 0, flame_until = 0;

  for (;;)
  {
    usleep(100000);
    uint32_t dt = (uint32_t)(100 * speed);
    now += dt;
    double s = dt / 1000.0;

    /* physics: heaters warm ~0.1 F/s, everything cools toward 65 F, open valves drain ~0.2 %/s */
    static const uint8_t hb[3] = { OUT_H1, OUT_H2, OUT_HNOZ };
    for (int i = 0; i < 3; i++)
    {
      temp[i] += ((c.out & hb[i]) ? (i == 2 ? 0.8 : 0.1) : 0) * s - (temp[i] - 65) * 0.0005 * s;
      in.temp_f10[i] = (int16_t)(temp[i] * 10);
    }
    if (c.out & OUT_V1) level[0] -= 0.2 * s;
    if (c.out & OUT_V2) level[1] -= 0.2 * s;
    for (int i = 0; i < 2; i++)
    {
      if (level[i] < -4) level[i] = -4;           /* the real senders bottom out near -4 % */
      in.level_x10[i] = (int16_t)(level[i] * 10);
    }
    if (flame_req) { flame_req = 0; flame_until = now + 3000; }
    in.flame = now < flame_until ? 1 : 0;

    char buf[256];
    ssize_t n;
    while ((n = read(m, buf, sizeof buf)) > 0)
    {
      for (ssize_t i = 0; i < n; i++)
      {
        if (buf[i] == '\n' || buf[i] == '\r')
        {
          rx[rlen] = '\0';
          size_t k = rlen ? proto_handle_line(&c, now, rx, line, sizeof line) : 0;
          if (k && write(m, line, k) < 0) { /* reader gone */ }
          if (k) fprintf(stderr, "<- %s   -> %s", rx, line);
          /* refill a tank whenever it is started, so it can be run again */
          if (strstr(rx, "START,1")) level[0] = 85;
          if (strstr(rx, "START,2")) level[1] = 85;
          rlen = 0;
        }
        else if (rlen < sizeof rx - 1) rx[rlen++] = buf[i];
      }
    }

    ctl_step(&c, now, &in);
    tick++;                                         /* status paced on real time, like the STM32 */
    if (tick - last_status >= 10 || (c.changed && tick - last_status >= 2))
    {
      c.changed = 0;
      last_status = tick;
      size_t k = proto_status(&c, now, &in, seq++, line, sizeof line);
      if (write(m, line, k) < 0) { /* no reader yet */ }
    }
  }
}
