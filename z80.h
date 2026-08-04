#ifndef Z80_Z80_H_
#define Z80_Z80_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "z80_timing.h"

typedef struct z80 z80;
struct z80 {
  uint8_t (*read_byte)(void*, uint16_t);
  void (*write_byte)(void*, uint16_t, uint8_t);
  uint8_t (*port_in)(z80*, uint16_t);
  void (*port_out)(z80*, uint16_t, uint8_t);
  // Called from inside process_interrupts() the instant a maskable interrupt
  // (not NMI) is actually accepted -- i.e. when the real Z80's /IORQ+/M1
  // interrupt-acknowledge cycle would fire. NULL (the default) skips the
  // call entirely; a CPC host wires this to let the Gate Array react to
  // acceptance itself (its automatic bit-5 counter clear), separate from
  // whatever device generated the interrupt via z80_gen_int().
  void (*int_ack)(z80*);
  void* userdata;

  unsigned long cyc; // cycle count (t-states)
  const z80_timing_t* timing; // NULL until z80_init() sets z80_timing_generic

  uint16_t pc, sp, ix, iy; // special purpose registers
  uint16_t mem_ptr; // "wz" register
  uint8_t a, b, c, d, e, h, l; // main registers
  uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
  uint8_t i, r; // interrupt vector, memory refresh

  // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
  bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

  uint8_t iff_delay;
  uint8_t interrupt_mode;
  uint8_t int_data;
  bool iff1 : 1, iff2 : 1;
  bool halted : 1;
  bool int_pending : 1, nmi_pending : 1;
};

void z80_init(z80* const z);
void z80_step(z80* const z);
void z80_debug_output(z80* const z);
void z80_gen_nmi(z80* const z);
void z80_gen_int(z80* const z, uint8_t data);
void z80_set_timing(z80* const z, const z80_timing_t* timing);

// Services any pending NMI/interrupt immediately, without executing another
// instruction first. z80_step() already calls this internally as its
// trailing step, so callers only need this if they generate an interrupt
// (z80_gen_int()/z80_gen_nmi()) *between* z80_step() calls and want it
// serviced right away instead of waiting for the next z80_step() to reach
// its own trailing call.
void z80_check_interrupt(z80* const z);

#endif // Z80_Z80_H_
