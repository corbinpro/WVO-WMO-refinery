/* STM32 serial link: parse $S status and $A acks, send $C commands one at a time. */
#include "refinery-ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define LINE_MAX_   160
#define QUEUE_LEN   16
#define ACK_WAIT_MS 1000   /* resend once if no ack by then, give up after the second wait */
#define LINK_OK_MS  5000

struct status g_st;
int      g_have_status;
int64_t  g_status_ms;
struct ack g_acks[N_ACKS];
unsigned g_ack_head;
char     g_msg[96];

static int fd = -1;
static char rbuf[LINE_MAX_];
static size_t rlen;

static struct { unsigned id; char body[48]; } queue[QUEUE_LEN];
static unsigned q_head, q_tail;          /* q_tail = command in flight (if sent) */
static unsigned next_id = 1;
static int64_t  sent_at;
static int      sent_tries;              /* 0 = head not sent yet */

int link_fd(void) { return fd; }

int link_open(const char *dev)
{
  fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return -1;
  struct termios t;
  if (tcgetattr(fd, &t) == 0)
  {
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
  }
  tcflush(fd, TCIOFLUSH);
  return fd;
}

static unsigned xor_sum(const char *s, size_t n)
{
  unsigned x = 0;
  while (n--) x ^= (unsigned char)*s++;
  return x;
}

/* Value of a reading field: "X" = no reading */
static int32_t reading(const char *s)
{
  return (s[0] == 'X') ? NO_READING : (int32_t)strtol(s, NULL, 10);
}

static int parse_status(char **f, int nf)
{
  if (nf != 25) return 0;
  struct status s;
  int i = 1;
  s.seq = (uint32_t)strtoul(f[i++], NULL, 10);
  s.uptime = (uint32_t)strtoul(f[i++], NULL, 10);
  for (int k = 0; k < 3; k++) s.temp_f10[k] = reading(f[i++]);
  for (int k = 0; k < 2; k++) s.level_x10[k] = reading(f[i++]);
  s.out = (unsigned)strtoul(f[i++], NULL, 16);
  s.flame = (unsigned)strtoul(f[i++], NULL, 16);
  s.fire = (unsigned)strtoul(f[i++], NULL, 10);
  for (int t = 0; t < 2; t++)
  {
    s.tank[t].state    = (unsigned)strtoul(f[i++], NULL, 10);
    s.tank[t].remain_s = (unsigned)strtoul(f[i++], NULL, 10);
    s.tank[t].preset   = (unsigned)strtoul(f[i++], NULL, 10);
    s.tank[t].timer_s  = (unsigned)strtoul(f[i++], NULL, 10);
  }
  s.cent  = (unsigned)strtoul(f[i++], NULL, 10);
  s.drain = (unsigned)strtoul(f[i++], NULL, 10);
  s.noz   = (unsigned)strtoul(f[i++], NULL, 10);
  s.ovr   = (unsigned)strtoul(f[i++], NULL, 16);
  s.oot   = (unsigned)strtoul(f[i++], NULL, 16);
  s.warn  = (unsigned)strtoul(f[i++], NULL, 16);
  g_st = s;
  g_have_status = 1;
  g_status_ms = now_ms();
  return 1;
}

static void record_ack(unsigned id, int ok, const char *reason)
{
  struct ack *a = &g_acks[g_ack_head % N_ACKS];
  memset(a, 0, sizeof *a);
  a->id = id;
  a->ok = ok;
  snprintf(a->reason, sizeof a->reason, "%s", reason);
  if (q_tail != q_head && queue[q_tail % QUEUE_LEN].id == id)
    snprintf(a->cmd, sizeof a->cmd, "%s", queue[q_tail % QUEUE_LEN].body);
  g_ack_head++;
  if (ok) snprintf(g_msg, sizeof g_msg, "OK: %s", a->cmd);
  else    snprintf(g_msg, sizeof g_msg, "%s: %s", a->cmd, reason);
}

static int parse_ack(char **f, int nf)
{
  if (nf < 3) return 0;
  unsigned id = (unsigned)strtoul(f[1], NULL, 10);
  int ok = strcmp(f[2], "OK") == 0;
  record_ack(id, ok, ok ? "" : (nf > 3 ? f[3] : "error"));
  if (q_tail != q_head && queue[q_tail % QUEUE_LEN].id == id)
  {
    q_tail++;
    sent_tries = 0;
  }
  return 1;
}

static int handle_line(char *line)
{
  size_t len = strlen(line);
  if (len < 5 || line[0] != '$' || line[len - 3] != '*') return 0;
  if (xor_sum(line + 1, len - 4) != (unsigned)strtoul(line + len - 2, NULL, 16)) return 0;
  line[len - 3] = '\0';

  char *f[32];
  int nf = 0;
  for (char *p = line + 1; p != NULL && nf < 32; )
  {
    f[nf++] = p;
    p = strchr(p, ',');
    if (p != NULL) *p++ = '\0';
  }
  if (strcmp(f[0], "S") == 0) return parse_status(f, nf);
  if (strcmp(f[0], "A") == 0) return parse_ack(f, nf);
  return 0;
}

int link_read(void)
{
  char buf[512];
  int changed = 0;
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0)
  {
    for (ssize_t i = 0; i < n; i++)
    {
      char c = buf[i];
      if (c == '\n' || c == '\r')
      {
        rbuf[rlen] = '\0';
        if (rlen > 0) changed |= handle_line(rbuf);
        rlen = 0;
      }
      else if (c == '$') { rbuf[0] = c; rlen = 1; }
      else if (rlen < sizeof rbuf - 1) rbuf[rlen++] = c;
    }
  }
  return changed;
}

static void transmit_head(int64_t now)
{
  char line[80], body[64];
  snprintf(body, sizeof body, "C,%u,%s", queue[q_tail % QUEUE_LEN].id, queue[q_tail % QUEUE_LEN].body);
  int n = snprintf(line, sizeof line, "$%s*%02X\n", body, xor_sum(body, strlen(body)));
  if (write(fd, line, (size_t)n) != n && errno != EAGAIN) perror("serial write");
  sent_at = now;
  sent_tries++;
}

void link_tick(int64_t now)
{
  if (q_tail == q_head) return;
  if (sent_tries == 0) { transmit_head(now); return; }
  if (now - sent_at < ACK_WAIT_MS) return;
  if (sent_tries < 2) { transmit_head(now); return; }
  record_ack(queue[q_tail % QUEUE_LEN].id, 0, "no response from controller");
  q_tail++;
  sent_tries = 0;
  request_redraw();
}

unsigned link_send(const char *body)
{
  if (q_head - q_tail >= QUEUE_LEN) return 0;
  unsigned id = next_id++;
  if (next_id > 99999) next_id = 1;
  queue[q_head % QUEUE_LEN].id = id;
  snprintf(queue[q_head % QUEUE_LEN].body, sizeof queue[0].body, "%s", body);
  q_head++;
  snprintf(g_msg, sizeof g_msg, "Sent %s ...", body);
  link_tick(now_ms());
  return id;
}

int link_ok(int64_t now)
{
  return g_have_status && now - g_status_ms < LINK_OK_MS;
}
