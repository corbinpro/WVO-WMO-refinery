/* Pi <-> STM32 line protocol over USART1. Pure C, no HAL.
 *
 * Every line is  $<body>*HH\n  where HH = XOR of the body bytes, two hex digits (NMEA style).
 * Lines with a missing or wrong checksum are ignored.
 *
 * Pi -> STM32   $C,<id>,<verb>[,<args>...]
 *   START,<t>  CANCEL,<t>  TEMP,<t>,<0|120|160|175>  TIME,<t>,<seconds>  NOZ,<160|170|180>
 *   OVR,<CENT|NOZ|V1|V2|H1|H2>,<ON|OFF>  FIREACK  PING          (t = 1 or 2)
 * STM32 -> Pi   $A,<id>,OK   or   $A,<id>,ERR,<reason>
 *               $S,<seq>,<uptime_s>,<t1>,<t2>,<tnoz>,<l1>,<l2>,<out>,<flame>,<fire>,
 *                  <s1>,<rem1>,<preset1>,<timer1>,<s2>,<rem2>,<preset2>,<timer2>,
 *                  <cent>,<drain>,<noz>,<ovr>,<oot>,<warn>
 *   temps: tenths of F; levels: tenths of %; X = no reading (sensor open/short)
 *   out/flame/ovr/oot/warn: hex bit masks (see control.h; warn bit0 = unused (was drain time limit),
 *     bit1 = flame sensors disabled in firmware)
 *   fire: 0 none, 1 extend, 2 dead time, 3 retract, 4 latched
 *   s: 0 idle, 1 settling, 2 ready, 3 draining; rem/timer: seconds; preset: F (0 = no heat)
 *   cent: 0 idle, 1 spin-up, 2 run, 3 run-out; drain: 0 none, else tank number
 */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>
#include <stdint.h>
#include "control.h"

#define PROTO_LINE_MAX  128

/* Handle one received line (without the newline). Writes the ack line into reply and returns its
 * length, or returns 0 if the line was ignored. */
size_t proto_handle_line(ctl_t *c, uint32_t now, const char *line, char *reply, size_t n);

/* Build one status line. Returns its length. */
size_t proto_status(const ctl_t *c, uint32_t now, const ctl_inputs_t *in, uint32_t seq, char *buf, size_t n);

#endif /* PROTO_H */
