/* Minimal HTTP/1.0 server for the web UI (REQ-CMD-010, REQ-UI-050).
 * Listens on 127.0.0.1 only; `tailscale serve` publishes it on the tailnet (HTTPS) together with
 * the ustreamer video at /video/.
 *   GET  /        the page (web/index.html, compiled in)
 *   GET  /status  JSON snapshot of the controller state
 *   POST /cmd     body = command such as "START,1"; same commands as the keypad (REQ-CMD-030)
 *   GET  /audio   live MP3 from the webcam microphone. ffmpeg runs only while someone listens;
 *                 its output is fanned out to every listener from this loop. Browsers buffer
 *                 ~32 KB before they start playing, so a new listener first gets the last few
 *                 seconds already encoded (burst on connect, like Icecast). */
#include "refinery-ui.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "index_html.h"     /* generated: static const char index_html[] */

#define MAX_CLIENTS 8
#define REQ_MAX     2048
#define CLIENT_MS   5000

static int lfd = -1;
static struct client { int fd; int audio; size_t len; int64_t t0; char req[REQ_MAX]; } cl[MAX_CLIENTS];

/* Microphone encoder (ffmpeg child) */
static const char *audio_dev = "plughw:CARD=U0x46d0x825,DEV=0";   /* Logitech C270 mic */
static pid_t enc_pid = -1;
static int enc_fd = -1;

/* Recent encoder output, kept as whole chunks (ffmpeg writes whole MP3 frames) */
#define BURST_BYTES  24576        /* ~3 s at 64 kbps */
#define BURST_CHUNKS 64
static struct { size_t len; char data[4096]; } burst[BURST_CHUNKS];
static unsigned burst_head, burst_n;  /* ring of chunks; burst_head = oldest */
static size_t burst_bytes;

static void burst_add(const char *d, size_t n)
{
  while (burst_n && (burst_n == BURST_CHUNKS || burst_bytes + n > BURST_BYTES))
  {
    burst_bytes -= burst[burst_head].len;
    burst_head = (burst_head + 1) % BURST_CHUNKS;
    burst_n--;
  }
  unsigned i = (burst_head + burst_n) % BURST_CHUNKS;
  memcpy(burst[i].data, d, n);
  burst[i].len = n;
  burst_bytes += n;
  burst_n++;
}

static void burst_send(int fd)
{
  for (unsigned k = 0; k < burst_n; k++)
  {
    unsigned i = (burst_head + k) % BURST_CHUNKS;
    if (write(fd, burst[i].data, burst[i].len) < 0) return;
  }
}

void web_audio_device(const char *dev) { audio_dev = dev; }

int web_listen_fd(void) { return lfd; }

int web_open(const char *host, int port)
{
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
  lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) { close(lfd); return lfd = -1; }
  for (int i = 0; i < MAX_CLIENTS; i++) cl[i].fd = -1;
  return lfd;
}

int web_fds(int *fds, short *events, int max)
{
  int n = 0;
  if (enc_fd >= 0 && n < max) { fds[n] = enc_fd; events[n++] = POLLIN; }
  for (int i = 0; i < MAX_CLIENTS && n < max; i++)
  {
    if (cl[i].fd < 0) continue;
    fds[n] = cl[i].fd;
    events[n++] = POLLIN;
  }
  return n;
}

static int audio_listeners(void)
{
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS; i++) n += cl[i].fd >= 0 && cl[i].audio;
  return n;
}

static void audio_stop(void)
{
  if (enc_fd >= 0) close(enc_fd);
  enc_fd = -1;
  burst_n = 0;
  burst_bytes = 0;
  if (enc_pid > 0) kill(enc_pid, SIGTERM);       /* reaped in web_tick */
}

static int audio_start(void)
{
  int p[2];
  if (enc_fd >= 0 || enc_pid > 0) return 0;     /* running, or still exiting: web_tick restarts it */
  if (pipe2(p, O_CLOEXEC) < 0) return -1;
  pid_t pid = fork();
  if (pid < 0) { close(p[0]); close(p[1]); return -1; }
  if (pid == 0)
  {
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(p[1], 1);
    dup2(null, 2);
    execlp("ffmpeg", "ffmpeg", "-nostdin", "-loglevel", "error", "-fflags", "+nobuffer",
           "-f", "alsa", "-channels", "1", "-sample_rate", "16000", "-i", audio_dev,
           "-ac", "1", "-c:a", "libmp3lame", "-b:a", "64k", "-flush_packets", "1", "-f", "mp3", "-",
           (char *)NULL);
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  enc_pid = pid;
  enc_fd = p[0];
  return 0;
}

/* Send encoder output to every listener. A listener that can't keep up skips a chunk
 * (a short glitch; MP3 decoders resync) rather than stalling the loop. */
static void audio_pump(void)
{
  char buf[4096];
  ssize_t n = read(enc_fd, buf, sizeof buf);
  if (n == 0 || (n < 0 && errno != EAGAIN)) { audio_stop(); return; }   /* ffmpeg died; next tick restarts */
  if (n < 0) return;
  burst_add(buf, (size_t)n);
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (cl[i].fd < 0 || !cl[i].audio) continue;
    if (write(cl[i].fd, buf, (size_t)n) < 0 && errno != EAGAIN) { close(cl[i].fd); cl[i].fd = -1; }
  }
}

static void close_client(struct client *c)
{
  close(c->fd);
  c->fd = -1;
  if (c->audio && audio_listeners() == 0) audio_stop();
}

void web_accept(void)
{
  int fd;
  while ((fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0)
  {
    int i = 0;
    while (i < MAX_CLIENTS && cl[i].fd >= 0) i++;
    if (i == MAX_CLIENTS) { close(fd); continue; }
    cl[i].fd = fd;
    cl[i].audio = 0;
    cl[i].len = 0;
    cl[i].t0 = now_ms();
  }
}

/* Responses are small; write them in one go (a client too slow to take ~10 KB is dropped) */
static void respond(struct client *c, const char *status, const char *type, const char *body, size_t len)
{
  char hdr[256];
  int n = snprintf(hdr, sizeof hdr, "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len);
  if (write(c->fd, hdr, (size_t)n) == n && len) { if (write(c->fd, body, len) < 0) { /* dropped */ } }
  close_client(c);
}

static size_t json_escape(char *dst, size_t n, const char *s)
{
  size_t k = 0;
  for (; *s && k + 2 < n; s++)
  {
    if (*s == '"' || *s == '\\') dst[k++] = '\\';
    if ((unsigned char)*s >= ' ') dst[k++] = *s;
  }
  dst[k] = '\0';
  return k;
}

static void reading(char *b, size_t n, int32_t v)
{
  if (v == NO_READING) snprintf(b, n, "null");
  else snprintf(b, n, "%d", (int)v);
}

static void send_status(struct client *c)
{
  static char j[8192];
  const struct status *s = &g_st;
  char r[5][16], msg[200];
  for (int i = 0; i < 3; i++) reading(r[i], sizeof r[i], s->temp_f10[i]);
  for (int i = 0; i < 2; i++) reading(r[3 + i], sizeof r[3 + i], s->level_x10[i]);
  json_escape(msg, sizeof msg, g_msg);
  size_t k = (size_t)snprintf(j, sizeof j,
    "{\"link\":%d,\"have\":%d,\"msg\":\"%s\",\"seq\":%u,\"uptime\":%u,"
    "\"temp\":[%s,%s,%s],\"level\":[%s,%s],\"out\":%u,\"flame\":%u,\"fire\":%u,"
    "\"tank\":[{\"state\":%u,\"remain\":%u,\"preset\":%u,\"timer\":%u},"
    "{\"state\":%u,\"remain\":%u,\"preset\":%u,\"timer\":%u}],"
    "\"cent\":%u,\"drain\":%u,\"noz\":%u,\"ovr\":%u,\"oot\":%u,\"warn\":%u,\"acks\":[",
    link_ok(now_ms()), g_have_status, msg, s->seq, s->uptime, r[0], r[1], r[2], r[3], r[4],
    s->out, s->flame, s->fire,
    s->tank[0].state, s->tank[0].remain_s, s->tank[0].preset, s->tank[0].timer_s,
    s->tank[1].state, s->tank[1].remain_s, s->tank[1].preset, s->tank[1].timer_s,
    s->cent, s->drain, s->noz, s->ovr, s->oot, s->warn);
  for (unsigned i = 0; i < N_ACKS && i < g_ack_head && k < sizeof j - 200; i++)
  {
    const struct ack *a = &g_acks[(g_ack_head - 1 - i) % N_ACKS];
    char rs[64], cs[64];
    json_escape(rs, sizeof rs, a->reason);
    json_escape(cs, sizeof cs, a->cmd);
    k += (size_t)snprintf(j + k, sizeof j - k, "%s{\"id\":%u,\"ok\":%d,\"cmd\":\"%s\",\"reason\":\"%s\"}",
                          i ? "," : "", a->id, a->ok, cs, rs);
  }
  /* Temperature history for the web graphs, oldest first: 96 samples per tank, 192 for the nozzle */
  k += (size_t)snprintf(j + k, sizeof j - k, "],\"hist\":[");
  for (int sr = 0; sr < 3; sr++)
  {
    int n = sr == 2 ? 192 : 96;
    k += (size_t)snprintf(j + k, sizeof j - k, "%s[", sr ? "," : "");
    for (int b = n - 1; b >= 0 && k < sizeof j - 16; b--)
    {
      int32_t v = ui_hist(sr, (unsigned)b);
      k += (size_t)(v == NO_READING ? snprintf(j + k, sizeof j - k, "null%s", b ? "," : "")
                                    : snprintf(j + k, sizeof j - k, "%d%s", (int)v, b ? "," : ""));
    }
    k += (size_t)snprintf(j + k, sizeof j - k, "]");
  }
  k += (size_t)snprintf(j + k, sizeof j - k, "]}");
  respond(c, "200 OK", "application/json", j, k);
}

/* Only commands the web may send. FIREACK is local-only (REQ-SAFE-050: local button press). */
static int allowed(const char *cmd)
{
  static const char *verbs[] = { "START,", "CANCEL,", "TEMP,", "TIME,", "NOZ,", "OVR,", "PING" };
  for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++)
    if (strncmp(cmd, verbs[i], strlen(verbs[i])) == 0) return 1;
  return 0;
}

/* Value of a request header (case-insensitive name, e.g. "Host"), copied into out; "" if absent */
static void header(const char *req, const char *end, const char *name, char *out, size_t n)
{
  size_t nl = strlen(name);
  out[0] = '\0';
  for (const char *p = strstr(req, "\r\n"); p && p < end; p = strstr(p + 2, "\r\n"))
  {
    const char *h = p + 2;
    if (strncasecmp(h, name, nl) != 0 || h[nl] != ':') continue;
    h += nl + 1;
    while (*h == ' ') h++;
    size_t len = strcspn(h, "\r\n");
    if (len >= n) len = n - 1;
    memcpy(out, h, len);
    out[len] = '\0';
    return;
  }
}

/* Only answer requests addressed to this machine by name: the tailnet name (via tailscale serve)
 * or loopback. Blocks DNS rebinding from a hostile page in a tailnet browser. */
static int host_ok(const char *host)
{
  char h[128];
  snprintf(h, sizeof h, "%s", host);
  h[strcspn(h, ":")] = '\0';                   /* drop :port */
  size_t n = strlen(h);
  return strcmp(h, "127.0.0.1") == 0 || strcmp(h, "localhost") == 0 ||
         (n > 7 && strcasecmp(h + n - 7, ".ts.net") == 0);
}

static void handle(struct client *c, char *body)
{
  char host[128], xref[8];
  header(c->req, body, "Host", host, sizeof host);
  header(c->req, body, "X-Refinery", xref, sizeof xref);
  if (!host_ok(host)) { respond(c, "403 Forbidden", "text/plain", "bad host\n", 9); return; }

  char *sp = strchr(c->req, ' ');
  if (!sp) { respond(c, "400 Bad Request", "text/plain", "", 0); return; }
  *sp = '\0';
  char *path = sp + 1, *e = strchr(path, ' ');
  if (e) *e = '\0';
  path[strcspn(path, "?")] = '\0';              /* ignore query strings (cache busters) */

  if (strcmp(c->req, "GET") == 0 && strcmp(path, "/") == 0)
    respond(c, "200 OK", "text/html; charset=utf-8", index_html, sizeof index_html - 1);
  else if (strcmp(c->req, "GET") == 0 && strcmp(path, "/status") == 0)
    send_status(c);
  else if (strcmp(c->req, "GET") == 0 && strcmp(path, "/audio") == 0)
  {
    static const char hdr[] = "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n"
                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (audio_start() != 0) { respond(c, "503 Service Unavailable", "text/plain", "", 0); return; }
    if (write(c->fd, hdr, sizeof hdr - 1) != (ssize_t)(sizeof hdr - 1)) { close_client(c); return; }
    burst_send(c->fd);
    c->audio = 1;
  }
  else if (strcmp(c->req, "POST") == 0 && strcmp(path, "/cmd") == 0)
  {
    /* A custom header can't be sent cross-site without a CORS preflight, which we never approve,
     * so another web page open on a tailnet device can't drive the rig (CSRF). */
    if (strcmp(xref, "1") != 0) { respond(c, "403 Forbidden", "text/plain", "missing X-Refinery\n", 19); return; }
    char cmd[48];
    size_t n = strcspn(body, "\r\n");
    if (n >= sizeof cmd) n = sizeof cmd - 1;
    memcpy(cmd, body, n);
    cmd[n] = '\0';
    for (char *p = cmd; *p; p++)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == ',')) { cmd[0] = '\0'; break; }
    if (!allowed(cmd)) { respond(c, "403 Forbidden", "text/plain", "not allowed from the web\n", 25); return; }
    unsigned id = link_send(cmd);
    char out[32];
    int k = snprintf(out, sizeof out, "{\"id\":%u}", id);
    request_redraw();
    respond(c, id ? "200 OK" : "503 Service Unavailable", "application/json", out, (size_t)k);
  }
  else respond(c, "404 Not Found", "text/plain", "not found\n", 10);
}

void web_io(int fd, short revents)
{
  if (fd == enc_fd) { audio_pump(); return; }
  struct client *c = NULL;
  for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd == fd) c = &cl[i];
  if (!c) return;
  if (revents & (POLLERR | POLLHUP | POLLNVAL)) { close_client(c); return; }
  if (c->audio)                                  /* listener sent something or hung up */
  {
    char junk[256];
    ssize_t k = read(fd, junk, sizeof junk);
    if (k == 0 || (k < 0 && errno != EAGAIN)) close_client(c);
    return;
  }
  ssize_t n = read(fd, c->req + c->len, REQ_MAX - 1 - c->len);
  if (n <= 0) { if (n == 0 || errno != EAGAIN) close_client(c); return; }
  c->len += (size_t)n;
  c->req[c->len] = '\0';

  char *end = strstr(c->req, "\r\n\r\n");
  if (!end)
  {
    if (c->len >= REQ_MAX - 1) respond(c, "413 Too Large", "text/plain", "", 0);
    return;
  }
  char *body = end + 4;
  size_t want = 0;
  char *cl_hdr = strcasestr(c->req, "\r\nContent-Length:");
  if (cl_hdr && cl_hdr < end) want = strtoul(cl_hdr + 17, NULL, 10);
  if ((size_t)(c->req + c->len - body) < want && c->len < REQ_MAX - 1) return;   /* body still coming */
  handle(c, body);
}

void web_tick(int64_t now)
{
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (cl[i].fd >= 0 && !cl[i].audio && now - cl[i].t0 > CLIENT_MS) close_client(&cl[i]);

  /* Reap a finished encoder; restart it if listeners remain (e.g. ffmpeg crashed) */
  if (enc_pid > 0 && waitpid(enc_pid, NULL, WNOHANG) == enc_pid)
  {
    enc_pid = -1;
    if (enc_fd >= 0) { close(enc_fd); enc_fd = -1; }
  }
  if (enc_pid < 0 && enc_fd < 0 && audio_listeners() > 0) audio_start();
}
