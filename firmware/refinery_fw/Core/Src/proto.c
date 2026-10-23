/* Pi <-> STM32 line protocol. See proto.h. */
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS  6

static uint8_t xor_sum(const char *s, size_t n)
{
  uint8_t x = 0;
  while (n--) x ^= (uint8_t)*s++;
  return x;
}

/* Wrap body as $body*HH\n */
static size_t frame(char *buf, size_t n, const char *body)
{
  int len = snprintf(buf, n, "$%s*%02X\n", body, xor_sum(body, strlen(body)));
  return (len > 0 && (size_t)len < n) ? (size_t)len : 0;
}

static int hexval(char ch)
{
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

/* Parse a decimal number that must fill the whole field */
static int num(const char *s, unsigned long *v)
{
  char *end;
  if (*s == '\0') return 0;
  *v = strtoul(s, &end, 10);
  return *end == '\0';
}

static const char *dispatch(ctl_t *c, uint32_t now, char **f, int nf)
{
  const char *verb = f[0];
  unsigned long a = 0, b = 0;

  if (strcmp(verb, "PING") == 0)    return NULL;
  if (strcmp(verb, "FIREACK") == 0) return ctl_fire_ack(c);
  if (strcmp(verb, "NOZ") == 0)
    return (nf == 2 && num(f[1], &a)) ? ctl_set_noz(c, (unsigned)a) : "usage NOZ,<F>";
  if (strcmp(verb, "OVR") == 0)
  {
    if (nf != 3) return "usage OVR,<CENT|NOZ|V1|V2|H1|H2>,<ON|OFF>";
    uint8_t bit = strcmp(f[1], "CENT") == 0 ? OVR_CENT : strcmp(f[1], "NOZ") == 0 ? OVR_NOZ
                : strcmp(f[1], "V1") == 0 ? OVR_V1 : strcmp(f[1], "V2") == 0 ? OVR_V2
                : strcmp(f[1], "H1") == 0 ? OVR_H1 : strcmp(f[1], "H2") == 0 ? OVR_H2 : 0;
    if (strcmp(f[2], "ON") == 0)  return ctl_override(c, bit, 1);
    if (strcmp(f[2], "OFF") == 0) return ctl_override(c, bit, 0);
    return "usage OVR,<CENT|NOZ|V1|V2|H1|H2>,<ON|OFF>";
  }

  /* The rest take a tank number 1 or 2 */
  if (nf < 2 || !num(f[1], &a) || (a != 1 && a != 2)) return "bad tank";
  int t = (int)a - 1;
  if (strcmp(verb, "START") == 0)  return nf == 2 ? ctl_start(c, t, now) : "usage START,<t>";
  if (strcmp(verb, "CANCEL") == 0) return nf == 2 ? ctl_cancel(c, t, now) : "usage CANCEL,<t>";
  if (strcmp(verb, "TEMP") == 0)
    return (nf == 3 && num(f[2], &b)) ? ctl_set_temp(c, t, (unsigned)b) : "usage TEMP,<t>,<F>";
  if (strcmp(verb, "TIME") == 0)
    return (nf == 3 && num(f[2], &b)) ? ctl_set_time(c, t, (uint32_t)b) : "usage TIME,<t>,<s>";
  return "unknown command";
}

size_t proto_handle_line(ctl_t *c, uint32_t now, const char *line, char *reply, size_t n)
{
  char body[PROTO_LINE_MAX];
  size_t len = strlen(line);

  /* $ ... *HH */
  if (len < 5 || line[0] != '$' || line[len - 3] != '*') return 0;
  int hi = hexval(line[len - 2]), lo = hexval(line[len - 1]);
  size_t blen = len - 4;
  if (hi < 0 || lo < 0 || blen >= sizeof body) return 0;
  if (xor_sum(line + 1, blen) != (uint8_t)(hi << 4 | lo)) return 0;
  memcpy(body, line + 1, blen);
  body[blen] = '\0';

  /* C,<id>,<verb>,<args...> */
  char *f[MAX_FIELDS + 2];
  int nf = 0;
  for (char *p = body; p != NULL && nf < MAX_FIELDS + 2; )
  {
    f[nf++] = p;
    p = strchr(p, ',');
    if (p != NULL) *p++ = '\0';
  }
  if (nf < 3 || strcmp(f[0], "C") != 0 || strlen(f[1]) > 8) return 0;

  const char *err = (nf > MAX_FIELDS + 1) ? "too many fields" : dispatch(c, now, f + 2, nf - 2);
  char ack[PROTO_LINE_MAX];
  if (err == NULL) snprintf(ack, sizeof ack, "A,%s,OK", f[1]);
  else             snprintf(ack, sizeof ack, "A,%s,ERR,%s", f[1], err);
  return frame(reply, n, ack);
}

static char *put_reading(char *p, char *end, int16_t v)
{
  int k = (v == CTL_NO_READING) ? snprintf(p, (size_t)(end - p), ",X")
                                : snprintf(p, (size_t)(end - p), ",%d", v);
  return (k > 0 && p + k < end) ? p + k : end;
}

size_t proto_status(const ctl_t *c, uint32_t now, const ctl_inputs_t *in, uint32_t seq, char *buf, size_t n)
{
  char body[PROTO_LINE_MAX + 64];
  char *p = body, *end = body + sizeof body;
  int k = snprintf(p, (size_t)(end - p), "S,%lu,%lu", (unsigned long)seq, (unsigned long)(now / 1000UL));
  p += k;
  for (int i = 0; i < 3; i++) p = put_reading(p, end, in->temp_f10[i]);
  for (int i = 0; i < 2; i++) p = put_reading(p, end, in->level_x10[i]);
  k = snprintf(p, (size_t)(end - p), ",%02X,%X,%d", c->out, in->flame, (int)c->fire);
  if (k > 0 && p + k < end) p += k;
  for (int t = 0; t < 2; t++)
  {
    k = snprintf(p, (size_t)(end - p), ",%d,%lu,%u,%lu", (int)c->tank[t].state,
                 (unsigned long)ctl_remaining_s(c, t, now), (unsigned)c->set.temp_preset[t],
                 (unsigned long)c->set.timer_s[t]);
    if (k > 0 && p + k < end) p += k;
  }
  snprintf(p, (size_t)(end - p), ",%d,%d,%u,%X,%X,%X", (int)c->cent, c->drain_tank + 1,
           (unsigned)c->set.noz_f, c->ovr, c->oot,
           (unsigned)(c->flame_off << 1));
  return frame(buf, n, body);
}
