/* Refinery control logic: settle timers, drain/centrifuge sequencing, heater loops, fire handling.
 * Pure C, no HAL: main.c feeds it sensor readings and the time, and drives the pins from the result.
 * Requirement IDs (REQ-...) refer to the SRS. */
#ifndef CONTROL_H
#define CONTROL_H

#include <stdint.h>

#define CTL_NO_READING    INT16_MIN   /* sensor open/short: temp_f10 / level_x10 value */

/* Timing and limits (ms, tenths of a degree F, tenths of a percent) */
#define DEADBAND_F10        50        /* +-5 F (REQ-TMP-030) */
#define OOT_F10             100       /* 2x deadband */
#define OOT_MS              (5UL * 60 * 1000)
#define HEATER_MAX_F10      2200      /* hard cutoff for every heater, even under manual override */
#define CENT_SPINUP_MS      5000      /* centrifuge on before oil flows */
#define VALVE_TRAVEL_MS     20000     /* valve relay held on = open; off = valve closes itself */
#define EMPTY_LEVEL_X10     50        /* tank counts as empty at or below 5.0 % ... */
#define EMPTY_HOLD_MS       10000     /* ... held this long */
#define CENT_RUNOUT_MS      (5UL * 60 * 1000)    /* keep spinning after a drain while the valve closes and
                                         the last oil clears; must exceed VALVE_TRAVEL_MS */
#define FLAME_DEBOUNCE_MS   200
#define FIRE_EXTEND_MS      2000      /* nominal, tune against the actuator (REQ-SAFE-040) */
#define FIRE_DEAD_MS        1000      /* both relays off between extend and retract (REQ-SAFE-041) */
#define FIRE_RETRACT_MS     FIRE_EXTEND_MS
#define TIMER_MAX_S         (99UL * 3600 + 59 * 60)

/* Output bits (also the "out" field of the status line) */
enum {
  OUT_V1   = 1 << 0,   /* Tank 1 valves (upper + lower) */
  OUT_V2   = 1 << 1,
  OUT_H1   = 1 << 2,   /* Tank 1 heater */
  OUT_H2   = 1 << 3,
  OUT_HNOZ = 1 << 4,   /* nozzle heater */
  OUT_CENT = 1 << 5,   /* centrifuge motor */
  OUT_EXT  = 1 << 6,   /* fire extinguisher extend relay */
  OUT_RET  = 1 << 7,   /* fire extinguisher retract relay */
};

/* Manual override bits (REQ-CMD-060..090): set = forced ON, clear = automatic */
enum { OVR_CENT = 1 << 0, OVR_NOZ = 1 << 1, OVR_V1 = 1 << 2, OVR_V2 = 1 << 3, OVR_H1 = 1 << 4, OVR_H2 = 1 << 5 };

/* Out-of-tolerance bits (REQ-TMP-030) */
enum { OOT_T1 = 1 << 0, OOT_T2 = 1 << 1, OOT_NOZ = 1 << 2 };

typedef enum { TANK_IDLE, TANK_SETTLING, TANK_READY, TANK_DRAINING } tank_state_t;
typedef enum { CENT_IDLE, CENT_SPINUP, CENT_RUN, CENT_RUNOUT } cent_state_t;
typedef enum { DRAIN_SPINUP, DRAIN_OPEN, DRAIN_FLOW } drain_phase_t;
typedef enum { FIRE_NONE, FIRE_EXTEND, FIRE_DEAD, FIRE_RETRACT, FIRE_LATCHED } fire_state_t;

typedef struct {
  int16_t temp_f10[3];     /* T1, T2, nozzle; CTL_NO_READING if the sensor is open/short */
  int16_t level_x10[2];    /* T1, T2 fill %; CTL_NO_READING if the sender is open/short */
  uint8_t flame;           /* bit n = flame sensor n+1 sees fire */
} ctl_inputs_t;

/* Operator settings; these persist in flash */
typedef struct {
  uint16_t temp_preset[2]; /* settling temp, F: 0 (no heat), 120, 160, 175 (REQ-TANK-040) */
  uint32_t timer_s[2];     /* settling duration (REQ-TANK-020) */
  uint16_t noz_f;          /* nozzle target, F: 160, 170, 180 (REQ-CFG-020) */
} ctl_settings_t;

typedef struct {
  uint8_t  on;             /* heater currently commanded on (hysteresis memory) */
  uint8_t  oot_timing;
  uint32_t oot_since;
} heat_loop_t;

typedef struct {
  ctl_settings_t set;
  struct {
    tank_state_t state;
    uint32_t     t_start;  /* settle start */
    heat_loop_t  heat;
  } tank[2];
  int8_t        drain_tank;   /* 0 / 1, or -1 when nothing drains */
  drain_phase_t drain_phase;
  uint32_t      drain_t;      /* start of the current drain phase */
  uint8_t       empty_timing;
  uint32_t      empty_since;
  cent_state_t  cent;
  uint32_t      cent_t;
  heat_loop_t   noz;
  uint8_t       ovr;
  uint8_t       oot;
  uint8_t       flame_off;     /* flame sensors disabled in firmware: no fire detection */
  fire_state_t  fire;
  uint32_t      fire_t;
  uint8_t       flame_raw;
  uint32_t      flame_since;
  uint8_t       out;          /* output bits from the last step */
  uint8_t       changed;      /* set on any state change; the caller clears it after reporting */
} ctl_t;

void ctl_init(ctl_t *c, const ctl_settings_t *saved);   /* saved may be NULL = defaults */
void ctl_step(ctl_t *c, uint32_t now, const ctl_inputs_t *in);
uint32_t ctl_remaining_s(const ctl_t *c, int t, uint32_t now);

/* Operator commands. Return NULL on success, or a short reason on refusal. */
const char *ctl_start(ctl_t *c, int t, uint32_t now);
const char *ctl_cancel(ctl_t *c, int t, uint32_t now);
const char *ctl_set_temp(ctl_t *c, int t, unsigned f);
const char *ctl_set_time(ctl_t *c, int t, uint32_t s);
const char *ctl_set_noz(ctl_t *c, unsigned f);
const char *ctl_override(ctl_t *c, uint8_t bit, int on);
const char *ctl_fire_ack(ctl_t *c);

#endif /* CONTROL_H */
