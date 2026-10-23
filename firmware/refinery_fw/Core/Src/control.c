/* Refinery control logic. See control.h. */
#include "control.h"

#include <stddef.h>
#include <string.h>

static const ctl_settings_t defaults = {
  .temp_preset = { 120, 120 },
  .timer_s     = { 3600, 3600 },
  .noz_f       = 170,
};

static int valid_temp_preset(unsigned f) { return f == 0 || f == 120 || f == 160 || f == 175; }
static int valid_noz(unsigned f)         { return f == 160 || f == 170 || f == 180; }

void ctl_init(ctl_t *c, const ctl_settings_t *saved)
{
  memset(c, 0, sizeof *c);
  c->set = defaults;
  if (saved != NULL)
  {
    for (int t = 0; t < 2; t++)
    {
      if (valid_temp_preset(saved->temp_preset[t])) c->set.temp_preset[t] = saved->temp_preset[t];
      if (saved->timer_s[t] > 0 && saved->timer_s[t] <= TIMER_MAX_S) c->set.timer_s[t] = saved->timer_s[t];
    }
    if (valid_noz(saved->noz_f)) c->set.noz_f = saved->noz_f;
  }
  c->drain_tank = -1;
  c->changed = 1;
}

/* Bang-bang with hysteresis: on at or below setpoint - deadband, off at setpoint.
 * Also tracks out-of-tolerance time. Returns the heater command. */
static int heat(heat_loop_t *h, int active, int16_t t_f10, int sp_f10, uint32_t now, uint8_t *oot, uint8_t bit)
{
  if (!active || t_f10 == CTL_NO_READING)
  {
    h->on = 0;
    h->oot_timing = 0;
    *oot &= (uint8_t)~bit;
    return 0;
  }
  if (t_f10 <= sp_f10 - DEADBAND_F10) h->on = 1;
  else if (t_f10 >= sp_f10)           h->on = 0;

  int err = t_f10 - sp_f10;
  if (err < 0) err = -err;
  if (err > OOT_F10)
  {
    if (!h->oot_timing) { h->oot_timing = 1; h->oot_since = now; }
    if (now - h->oot_since >= OOT_MS) *oot |= bit;
  }
  else
  {
    h->oot_timing = 0;
    *oot &= (uint8_t)~bit;
  }
  return h->on;
}

static void set_tank(ctl_t *c, int t, tank_state_t s)
{
  if (c->tank[t].state != s) { c->tank[t].state = s; c->changed = 1; }
}

static void set_cent(ctl_t *c, cent_state_t s, uint32_t now)
{
  if (c->cent != s) { c->cent = s; c->cent_t = now; c->changed = 1; }
}

static void drain_phase(ctl_t *c, drain_phase_t p, uint32_t now)
{
  c->drain_phase = p;
  c->drain_t = now;
  c->changed = 1;
}

/* Fire: everything off, run the extinguisher, latch (REQ-SAFE-020..060) */
static void fire_step(ctl_t *c, uint32_t now, const ctl_inputs_t *in)
{
  if (in->flame) { if (!c->flame_raw) { c->flame_raw = 1; c->flame_since = now; } }
  else c->flame_raw = 0;

  if (c->fire == FIRE_NONE && c->flame_raw && now - c->flame_since >= FLAME_DEBOUNCE_MS)
  {
    c->fire = FIRE_EXTEND;
    c->fire_t = now;
    c->changed = 1;
    /* Suspend everything: a fire run is not resumed, the machine gets cleaned first */
    for (int t = 0; t < 2; t++)
    {
      c->tank[t].state = TANK_IDLE;
      c->tank[t].heat.on = 0;
      c->tank[t].heat.oot_timing = 0;
    }
    c->drain_tank = -1;
    c->cent = CENT_IDLE;
    c->noz.on = 0;
    c->noz.oot_timing = 0;
    c->ovr = 0;
    c->oot = 0;
  }

  uint32_t dt = now - c->fire_t;
  switch (c->fire)
  {
    case FIRE_EXTEND:
      if (dt >= FIRE_EXTEND_MS) { c->fire = FIRE_DEAD; c->fire_t = now; c->changed = 1; }
      break;
    case FIRE_DEAD:
      if (dt >= FIRE_DEAD_MS) { c->fire = FIRE_RETRACT; c->fire_t = now; c->changed = 1; }
      break;
    case FIRE_RETRACT:
      if (dt >= FIRE_RETRACT_MS) { c->fire = FIRE_LATCHED; c->fire_t = now; c->changed = 1; }
      break;
    default:
      break;
  }
}

/* A drain ends only on the level sender reading empty (no time limit: a tank can take hours).
 * If the sender reads open/short the drain keeps going until the operator cancels it. */
static void end_drain(ctl_t *c, uint32_t now)
{
  set_tank(c, c->drain_tank, TANK_IDLE);
  c->drain_tank = -1;
  set_cent(c, CENT_RUNOUT, now);
}

/* Drain sequencing (REQ-CFG-010..060) */
static void drain_step(ctl_t *c, uint32_t now, const ctl_inputs_t *in)
{
  if (c->drain_tank < 0)
  {
    int pick = (c->tank[0].state == TANK_READY) ? 0 : (c->tank[1].state == TANK_READY) ? 1 : -1;
    if (pick >= 0)
    {
      c->drain_tank = (int8_t)pick;
      c->empty_timing = 0;
      set_tank(c, pick, TANK_DRAINING);
      if (c->cent == CENT_RUN || c->cent == CENT_RUNOUT)
      {
        set_cent(c, CENT_RUN, now);                  /* hand-off: keep spinning (REQ-CFG-040) */
        drain_phase(c, DRAIN_OPEN, now);
      }
      else
      {
        set_cent(c, CENT_SPINUP, now);
        drain_phase(c, DRAIN_SPINUP, now);
      }
    }
    else if (c->cent == CENT_RUNOUT && now - c->cent_t >= CENT_RUNOUT_MS)
    {
      set_cent(c, CENT_IDLE, now);                   /* nothing ready: stop and wait (REQ-CFG-050) */
    }
    return;
  }

  switch (c->drain_phase)
  {
    case DRAIN_SPINUP:
      if (now - c->drain_t >= CENT_SPINUP_MS)
      {
        set_cent(c, CENT_RUN, now);
        drain_phase(c, DRAIN_OPEN, now);
      }
      break;
    case DRAIN_OPEN:                                 /* valve travelling open */
      if (now - c->drain_t >= VALVE_TRAVEL_MS)
      {
        drain_phase(c, DRAIN_FLOW, now);
      }
      break;
    case DRAIN_FLOW:
    {
      int16_t lvl = in->level_x10[c->drain_tank];
      if (lvl != CTL_NO_READING && lvl <= EMPTY_LEVEL_X10)
      {
        if (!c->empty_timing) { c->empty_timing = 1; c->empty_since = now; }
        if (now - c->empty_since >= EMPTY_HOLD_MS) end_drain(c, now);
      }
      else c->empty_timing = 0;
      break;
    }
  }
}

void ctl_step(ctl_t *c, uint32_t now, const ctl_inputs_t *in)
{
  uint8_t out = 0;

  fire_step(c, now, in);
  if (c->fire == FIRE_EXTEND)  out = OUT_EXT;
  if (c->fire == FIRE_RETRACT) out = OUT_RET;
  if (c->fire != FIRE_NONE)
  {
    if (out != c->out) c->changed = 1;
    c->out = out;
    return;
  }

  /* Settle timers run on wall-clock time (REQ-TANK-030) */
  for (int t = 0; t < 2; t++)
  {
    if (c->tank[t].state == TANK_SETTLING && now - c->tank[t].t_start >= c->set.timer_s[t] * 1000UL)
      set_tank(c, t, TANK_READY);
  }

  drain_step(c, now, in);

  /* Tank heaters: hold setpoint while settling, waiting to drain, and draining (REQ-TANK-050);
   * off once the tank reads empty and goes idle */
  for (int t = 0; t < 2; t++)
  {
    tank_state_t s = c->tank[t].state;
    int active = (s == TANK_SETTLING || s == TANK_READY || s == TANK_DRAINING) && c->set.temp_preset[t] != 0;
    if (heat(&c->tank[t].heat, active, in->temp_f10[t], c->set.temp_preset[t] * 10, now, &c->oot,
             (uint8_t)(t == 0 ? OOT_T1 : OOT_T2)))
      out |= (uint8_t)(t == 0 ? OUT_H1 : OUT_H2);
  }

  /* Nozzle heater runs during a drain (REQ-CFG-020) */
  if (heat(&c->noz, c->drain_tank >= 0, in->temp_f10[2], c->set.noz_f * 10, now, &c->oot, OOT_NOZ))
    out |= OUT_HNOZ;

  if (c->drain_tank >= 0 && c->drain_phase != DRAIN_SPINUP)
    out |= (uint8_t)(c->drain_tank == 0 ? OUT_V1 : OUT_V2);
  if (c->cent != CENT_IDLE) out |= OUT_CENT;

  /* Manual overrides force ON; the schedule above keeps running underneath (REQ-CMD-090) */
  if (c->ovr & OVR_CENT) out |= OUT_CENT;
  if (c->ovr & OVR_NOZ)  out |= OUT_HNOZ;
  if (c->ovr & OVR_V1)   out |= OUT_V1;
  if (c->ovr & OVR_V2)   out |= OUT_V2;
  if (c->ovr & OVR_H1)   out |= OUT_H1;
  if (c->ovr & OVR_H2)   out |= OUT_H2;

  /* Hard heater cutoff: too hot or no reading = off, override or not */
  static const uint8_t hbit[3] = { OUT_H1, OUT_H2, OUT_HNOZ };
  for (int i = 0; i < 3; i++)
  {
    if (in->temp_f10[i] == CTL_NO_READING || in->temp_f10[i] >= HEATER_MAX_F10) out &= (uint8_t)~hbit[i];
  }

  if (out != c->out) c->changed = 1;
  c->out = out;
}

uint32_t ctl_remaining_s(const ctl_t *c, int t, uint32_t now)
{
  if (c->tank[t].state != TANK_SETTLING) return 0;
  uint32_t done = (now - c->tank[t].t_start) / 1000UL;
  return (done >= c->set.timer_s[t]) ? 0 : c->set.timer_s[t] - done;
}

/* ---- operator commands ---- */

#define CHECK_TANK(t)  do { if ((t) != 0 && (t) != 1) return "bad tank"; } while (0)

const char *ctl_start(ctl_t *c, int t, uint32_t now)
{
  CHECK_TANK(t);
  if (c->fire != FIRE_NONE) return "fire fault";
  if (c->tank[t].state != TANK_IDLE) return "tank busy";
  c->tank[t].t_start = now;
  c->tank[t].heat.on = 0;
  c->tank[t].heat.oot_timing = 0;
  set_tank(c, t, TANK_SETTLING);
  return NULL;
}

/* Cancel one tank: timer, heater, valves, and its drain (REQ-CMD-050) */
const char *ctl_cancel(ctl_t *c, int t, uint32_t now)
{
  CHECK_TANK(t);
  if (c->drain_tank == t)
  {
    c->drain_tank = -1;
    /* The valve takes VALVE_TRAVEL_MS to close and oil flows meanwhile, so the centrifuge spins
     * down through the normal run-out instead of stopping under incoming oil. */
    c->cent = CENT_RUNOUT;
    c->cent_t = now;
    c->noz.on = 0;
    c->noz.oot_timing = 0;
    c->oot &= (uint8_t)~OOT_NOZ;
  }
  c->tank[t].state = TANK_IDLE;
  c->tank[t].heat.on = 0;
  c->tank[t].heat.oot_timing = 0;
  c->oot &= (uint8_t)~(t == 0 ? OOT_T1 : OOT_T2);
  c->ovr &= (uint8_t)~(t == 0 ? (OVR_V1 | OVR_H1) : (OVR_V2 | OVR_H2));
  c->changed = 1;
  return NULL;
}

const char *ctl_set_temp(ctl_t *c, int t, unsigned f)
{
  CHECK_TANK(t);
  if (!valid_temp_preset(f)) return "temp must be 0/120/160/175";
  c->set.temp_preset[t] = (uint16_t)f;
  c->changed = 1;
  return NULL;
}

const char *ctl_set_time(ctl_t *c, int t, uint32_t s)
{
  CHECK_TANK(t);
  if (s == 0 || s > TIMER_MAX_S) return "time out of range";
  c->set.timer_s[t] = s;
  c->changed = 1;
  return NULL;
}

const char *ctl_set_noz(ctl_t *c, unsigned f)
{
  if (!valid_noz(f)) return "nozzle must be 160/170/180";
  c->set.noz_f = (uint16_t)f;
  c->changed = 1;
  return NULL;
}

const char *ctl_override(ctl_t *c, uint8_t bit, int on)
{
  if (bit != OVR_CENT && bit != OVR_NOZ && bit != OVR_V1 && bit != OVR_V2 && bit != OVR_H1 && bit != OVR_H2)
    return "bad override";
  if (on && c->fire != FIRE_NONE) return "fire fault";
  if (on) c->ovr |= bit;
  else    c->ovr &= (uint8_t)~bit;
  c->changed = 1;
  return NULL;
}

/* Clear the fire latch after the operator has serviced the machine (REQ-SAFE-050) */
const char *ctl_fire_ack(ctl_t *c)
{
  if (c->fire == FIRE_NONE) return "no fire";
  if (c->fire != FIRE_LATCHED) return "extinguisher still running";
  if (c->flame_raw) return "flame still detected";
  c->fire = FIRE_NONE;
  c->changed = 1;
  return NULL;
}
