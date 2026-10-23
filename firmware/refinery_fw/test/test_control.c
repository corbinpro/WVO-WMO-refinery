/* Host-side tests for control.c and proto.c (no hardware).
 * Build and run:  make -C test   (or: gcc -std=c99 -Wall -Wextra -I../Core/Inc test_control.c
 *                                     ../Core/Src/control.c ../Core/Src/proto.c -o test_control) */
#include <stdio.h>
#include <string.h>
#include "control.h"
#include "proto.h"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static ctl_t c;
static ctl_inputs_t in;
static uint32_t now;

static void reset(void)
{
  ctl_settings_t s = { .temp_preset = { 0, 0 }, .timer_s = { 60, 60 }, .noz_f = 170 };
  ctl_init(&c, &s);
  memset(&in, 0, sizeof in);
  in.temp_f10[0] = in.temp_f10[1] = in.temp_f10[2] = 700;   /* 70.0 F */
  in.level_x10[0] = in.level_x10[1] = 800;                  /* 80 % */
  now = 1000;
  ctl_step(&c, now, &in);
}

/* Advance time in 100 ms steps, checking the extinguisher relays never overlap */
static void run(uint32_t ms)
{
  for (uint32_t t = 0; t < ms; t += 100)
  {
    now += 100;
    ctl_step(&c, now, &in);
    CHECK(!((c.out & OUT_EXT) && (c.out & OUT_RET)));
  }
}

static void test_settle_drain_stop(void)
{
  reset();
  CHECK(ctl_start(&c, 0, now) == NULL);
  CHECK(ctl_start(&c, 0, now) != NULL);              /* already running */
  run(30000);
  CHECK(c.tank[0].state == TANK_SETTLING);
  CHECK(ctl_remaining_s(&c, 0, now) == 30);
  CHECK(!(c.out & OUT_CENT));
  run(30000);
  CHECK(c.tank[0].state == TANK_DRAINING);           /* timer done, centrifuge idle -> drain */
  CHECK(c.cent == CENT_SPINUP && (c.out & OUT_CENT) && !(c.out & OUT_V1));
  CHECK(c.out & OUT_HNOZ);                           /* 70 F < 170 F */
  run(CENT_SPINUP_MS);
  CHECK(c.cent == CENT_RUN && (c.out & OUT_V1));
  run(VALVE_TRAVEL_MS + 60000);
  CHECK(c.tank[0].state == TANK_DRAINING);           /* still 80 % full */
  in.level_x10[0] = 20;
  run(EMPTY_HOLD_MS - 1000);
  CHECK(c.tank[0].state == TANK_DRAINING);
  run(2000);
  CHECK(c.tank[0].state == TANK_IDLE && !(c.out & OUT_V1));
  CHECK(c.cent == CENT_RUNOUT && (c.out & OUT_CENT) && !(c.out & OUT_HNOZ));
  run(CENT_RUNOUT_MS + 200);
  CHECK(c.cent == CENT_IDLE && c.out == 0);          /* T2 not ready -> stop (REQ-CFG-050) */
}

static void test_handoff(void)
{
  reset();
  ctl_set_time(&c, 1, 120);
  ctl_start(&c, 0, now);
  ctl_start(&c, 1, now);
  run(60000 + CENT_SPINUP_MS + VALVE_TRAVEL_MS + 100);
  CHECK(c.drain_tank == 0);
  run(60000);                                        /* T2 now READY, waiting */
  CHECK(c.tank[1].state == TANK_READY);
  in.level_x10[0] = 0;
  run(EMPTY_HOLD_MS + 200);
  CHECK(c.tank[0].state == TANK_IDLE);
  CHECK(c.drain_tank == 1 && c.cent == CENT_RUN);    /* no spin-down, straight to T2 (REQ-CFG-040) */
  CHECK(c.drain_phase == DRAIN_OPEN && (c.out & OUT_V2) && (c.out & OUT_CENT));
}

static void test_tie_tank1_first(void)
{
  reset();
  ctl_start(&c, 1, now);
  ctl_start(&c, 0, now);
  run(60100);
  CHECK(c.drain_tank == 0 && c.tank[1].state == TANK_READY);   /* REQ-CFG-060 */
}

static void test_cancel_isolation(void)
{
  reset();
  ctl_set_time(&c, 1, 600);
  ctl_set_temp(&c, 1, 120);
  ctl_start(&c, 0, now);
  ctl_start(&c, 1, now);
  run(60000 + CENT_SPINUP_MS + VALVE_TRAVEL_MS + 100);
  CHECK(c.drain_tank == 0 && (c.out & OUT_V1) && (c.out & OUT_H2));
  CHECK(ctl_cancel(&c, 0, now) == NULL);
  run(100);
  CHECK(c.tank[0].state == TANK_IDLE && !(c.out & (OUT_V1 | OUT_HNOZ)));
  CHECK(c.cent == CENT_RUNOUT && (c.out & OUT_CENT));   /* spins while the valve closes */
  CHECK(c.tank[1].state == TANK_SETTLING && (c.out & OUT_H2));     /* T2 untouched */
  CHECK(ctl_remaining_s(&c, 1, now) == 600 - 85);
}

static void test_override_resume(void)
{
  reset();
  ctl_start(&c, 0, now);
  CHECK(ctl_override(&c, OVR_CENT, 1) == NULL);
  CHECK(ctl_override(&c, OVR_V2, 1) == NULL);
  run(1000);
  CHECK((c.out & OUT_CENT) && (c.out & OUT_V2));
  run(60000);                                        /* schedule keeps running underneath */
  CHECK(c.tank[0].state == TANK_DRAINING);
  ctl_override(&c, OVR_CENT, 0);
  ctl_override(&c, OVR_V2, 0);
  run(100);
  CHECK((c.out & OUT_CENT) && !(c.out & OUT_V2));    /* centrifuge still on: the drain wants it */
  ctl_cancel(&c, 0, now);
  run(CENT_RUNOUT_MS + 200);
  CHECK(c.out == 0);
}

static void test_heater_deadband_and_cutoff(void)
{
  reset();
  ctl_set_temp(&c, 0, 160);
  ctl_start(&c, 0, now);
  run(100);
  CHECK(c.out & OUT_H1);
  in.temp_f10[0] = 1580; run(100); CHECK(c.out & OUT_H1);      /* inside band, still heating up */
  in.temp_f10[0] = 1600; run(100); CHECK(!(c.out & OUT_H1));   /* at setpoint: off */
  in.temp_f10[0] = 1560; run(100); CHECK(!(c.out & OUT_H1));   /* within band: stays off */
  in.temp_f10[0] = 1550; run(100); CHECK(c.out & OUT_H1);      /* at -5 F: on */
  in.temp_f10[0] = CTL_NO_READING; run(100); CHECK(!(c.out & OUT_H1));
  ctl_override(&c, OVR_NOZ, 1);
  in.temp_f10[2] = HEATER_MAX_F10; run(100);
  CHECK(!(c.out & OUT_HNOZ));                        /* hard cutoff beats the override */
  in.temp_f10[2] = 700; run(100);
  CHECK(c.out & OUT_HNOZ);

  /* Manual tank heater: on regardless of tank state, cutoff still applies, cancel clears it */
  reset();
  CHECK(ctl_override(&c, OVR_H2, 1) == NULL);
  run(100);
  CHECK((c.out & OUT_H2) && c.tank[1].state == TANK_IDLE);
  in.temp_f10[1] = HEATER_MAX_F10; run(100);
  CHECK(!(c.out & OUT_H2));
  in.temp_f10[1] = 700;
  ctl_cancel(&c, 1, now);
  run(100);
  CHECK(!(c.out & OUT_H2) && c.ovr == 0);
}

static void test_out_of_tolerance(void)
{
  reset();
  ctl_set_temp(&c, 0, 175);
  ctl_set_time(&c, 0, 3600);
  ctl_start(&c, 0, now);
  run(OOT_MS - 1000);
  CHECK(!(c.oot & OOT_T1));
  run(2000);
  CHECK(c.oot & OOT_T1);                             /* 70 F vs 175 F for 5 min */
  in.temp_f10[0] = 1720; run(100);
  CHECK(!(c.oot & OOT_T1));
}

static void test_fire(void)
{
  reset();
  ctl_set_temp(&c, 0, 120);
  ctl_start(&c, 0, now);
  ctl_override(&c, OVR_CENT, 1);
  run(1000);
  CHECK(c.out & (OUT_H1 | OUT_CENT));
  in.flame = 0x4;
  run(100);
  CHECK(c.fire == FIRE_NONE);                        /* debounce */
  run(FLAME_DEBOUNCE_MS);
  CHECK(c.fire == FIRE_EXTEND && c.out == OUT_EXT);  /* everything else off */
  CHECK(c.tank[0].state == TANK_IDLE && c.ovr == 0);
  CHECK(ctl_start(&c, 1, now) != NULL);
  CHECK(ctl_override(&c, OVR_V1, 1) != NULL);
  run(FIRE_EXTEND_MS);
  CHECK(c.fire == FIRE_DEAD && c.out == 0);
  run(FIRE_DEAD_MS);
  CHECK(c.fire == FIRE_RETRACT && c.out == OUT_RET);
  CHECK(ctl_fire_ack(&c) != NULL);                   /* not finished yet */
  run(FIRE_RETRACT_MS);
  CHECK(c.fire == FIRE_LATCHED && c.out == 0);
  CHECK(ctl_fire_ack(&c) != NULL);                   /* flame still seen */
  in.flame = 0;
  run(60000);
  CHECK(c.fire == FIRE_LATCHED && c.out == 0);       /* latched until acknowledged */
  CHECK(ctl_fire_ack(&c) == NULL);
  CHECK(ctl_start(&c, 0, now) == NULL);
}

static void test_drain_no_time_limit(void)
{
  reset();
  ctl_set_temp(&c, 0, 120);
  in.temp_f10[0] = 1000;                             /* below setpoint: heater wants on */
  in.level_x10[0] = CTL_NO_READING;                  /* sender unplugged: drain must not end on its own */
  ctl_start(&c, 0, now);
  run(60000 + CENT_SPINUP_MS + VALVE_TRAVEL_MS + 100);
  CHECK(c.tank[0].state == TANK_DRAINING && (c.out & OUT_H1));     /* heater stays on while draining */
  run(3UL * 60 * 60 * 1000);                         /* 3 hours */
  CHECK(c.tank[0].state == TANK_DRAINING && (c.out & OUT_V1) && (c.out & OUT_CENT));
  in.level_x10[0] = 20;
  run(EMPTY_HOLD_MS + 200);
  CHECK(c.tank[0].state == TANK_IDLE && !(c.out & (OUT_V1 | OUT_H1)));   /* empty: valve + heater off */
}

/* Build a checksummed command line */
static const char *cmd(const char *body)
{
  static char line[PROTO_LINE_MAX];
  unsigned x = 0;
  for (const char *p = body; *p; p++) x ^= (unsigned char)*p;
  snprintf(line, sizeof line, "$%s*%02X", body, x);
  return line;
}

static void test_proto(void)
{
  char r[PROTO_LINE_MAX];
  reset();
  CHECK(proto_handle_line(&c, now, cmd("C,7,PING"), r, sizeof r) > 0);
  CHECK(strncmp(r, "$A,7,OK*", 8) == 0);
  CHECK(proto_handle_line(&c, now, "$C,8,PING*00", r, sizeof r) == 0);   /* bad checksum */
  CHECK(proto_handle_line(&c, now, "C,8,PING", r, sizeof r) == 0);
  proto_handle_line(&c, now, cmd("C,9,TEMP,2,160"), r, sizeof r);
  CHECK(strncmp(r, "$A,9,OK", 7) == 0 && c.set.temp_preset[1] == 160);
  proto_handle_line(&c, now, cmd("C,10,TEMP,2,150"), r, sizeof r);
  CHECK(strncmp(r, "$A,10,ERR,", 10) == 0);
  proto_handle_line(&c, now, cmd("C,11,TIME,1,5400"), r, sizeof r);
  CHECK(c.set.timer_s[0] == 5400);
  proto_handle_line(&c, now, cmd("C,12,START,3"), r, sizeof r);
  CHECK(strstr(r, "ERR,bad tank") != NULL);
  proto_handle_line(&c, now, cmd("C,13,OVR,V2,ON"), r, sizeof r);
  CHECK(c.ovr == OVR_V2);
  proto_handle_line(&c, now, cmd("C,16,OVR,H1,ON"), r, sizeof r);
  CHECK(c.ovr == (OVR_V2 | OVR_H1));
  proto_handle_line(&c, now, cmd("C,17,OVR,H1,OFF"), r, sizeof r);
  proto_handle_line(&c, now, cmd("C,14,NOZ,180"), r, sizeof r);
  CHECK(c.set.noz_f == 180);
  proto_handle_line(&c, now, cmd("C,15,START,1"), r, sizeof r);
  CHECK(c.tank[0].state == TANK_SETTLING);

  char s[256];
  in.temp_f10[2] = CTL_NO_READING;
  size_t n = proto_status(&c, now, &in, 42, s, sizeof s);
  printf("status: %s", s);
  CHECK(n > 0 && s[n - 1] == '\n' && strncmp(s, "$S,42,1,700,700,X,800,800,", 26) == 0);
  CHECK(strstr(s, ",1,5400,0,5400,0,0,160,60,0,0,180,8,0,0*") != NULL);
}

int main(void)
{
  test_settle_drain_stop();
  test_handoff();
  test_tie_tank1_first();
  test_cancel_isolation();
  test_override_resume();
  test_heater_deadband_and_cutoff();
  test_out_of_tolerance();
  test_fire();
  test_drain_no_time_limit();
  test_proto();
  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
