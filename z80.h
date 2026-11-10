#ifndef Z80_Z80_H_
#define Z80_Z80_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "z80_timing.h"

// Half-T strobe edges: the Z80 drives and samples its bus strobes on
// its own clock edges, so every edge below lands on a half-T boundary
// (125 ns at 4 MHz, two half-T per T). Offsets are half-T counts from the
// bus cycle's start T, before any WAIT stretching. The sample T per cycle
// kind is the ACCC's (1.9 §4.4.4 p.26: opcode fetch and memory sample in
// T2, port in T3); the CLK H/L halves of diagrams A-C (p.28-29) put the
// /WAIT sample at the falling edge and the read-data latch at the rising
// edge that ends the sample T. The board clock (16 MHz, two master ticks
// per half-T) adds an offset to a cycle's start T to reach the edge's
// tick. Output-strobe windows (/MREQ, /IORQ, /RD, /WR fall and rise) are
// analogue delays, not clock edges, so they stay out of this half-T grid;
// their per-T active phases are the operation rows of diagrams A-C.
enum {
  Z80_HALF_T_PER_T = 2, // half-T (Z80 clock halves) per T-state
  Z80_STROBE_WAIT_M1 = 3, // /WAIT sample: clock fall in T2 (M1, memory)
  Z80_STROBE_WAIT_PORT = 5, // /WAIT sample: clock fall in T3 (port, auto Tw)
  Z80_STROBE_DATA_M1 = 4, // read-data latch: clock rise ending T2 (M1, memory)
  Z80_STROBE_DATA_PORT = 6 // read-data latch: clock rise ending T3 (port)
};

typedef struct z80 z80;

// Every memory read carries its access kind so the host can tell
// opcode fetches and instruction-stream operands apart from data reads
// without decoding anything. M1 covers the opcode fetch including each
// CB/ED/DD/FD prefix byte; operand covers immediate/displacement bytes
// (ld a,n, jp nn, the d of (ix+d), and both the d and the final opcode byte
// of DD CB d op, which a real Z80 reads as non-M1 cycles); im2_vector covers
// the two IM2 vector-table reads during interrupt acceptance; data covers
// every other rb/rw (HL/BC/DE/IX+d/nn operands, stack pops, ex (sp),hl,
// block ops). debug covers z80_debug_output's trace peeks, which are never
// real bus cycles and must never trip the host's read-before-write check.
typedef enum {
  Z80_READ_M1 = 0,
  Z80_READ_OPERAND = 1,
  Z80_READ_IM2_VECTOR = 2,
  Z80_READ_DATA = 3,
  Z80_READ_DEBUG = 4
} z80_read_kind_t;

// How the core charges the cycle count for an instruction. FROM_TABLE (the
// default, set by z80_init) adds the timing table's per-opcode and
// conditional costs as the instruction runs. FROM_WAIT_SLOTS charges once
// per step, at the end of the instruction, as the number of 4-T periods
// from the step's M1 sample slot to the next one; the per-opcode and
// conditional table charges are skipped, while NMI/IM1/IM2 acknowledges
// are charged from their own sampled slots; IM 0 keeps the table. The
// mode applies to a step only when the WAIT mask leaves exactly one phase
typedef enum {
  Z80_CYC_FROM_TABLE = 0,
  Z80_CYC_FROM_WAIT_SLOTS = 1
} z80_cyc_source_t;

// Kind of bus cycle requested from im0_bus (see struct z80).
typedef enum {
  Z80_IM0_INTA = 0, // follow-on opcode byte after a prefix (M1 + IORQ cycle)
  Z80_IM0_READ = 1  // operand byte (normal memory read)
} z80_im0_cycle_t;

struct z80 {
  uint8_t (*read_byte)(void*, uint16_t, z80_read_kind_t);
  void (*write_byte)(void*, uint16_t, uint8_t);
  uint8_t (*port_in)(z80*, uint16_t);
  void (*port_out)(z80*, uint16_t, uint8_t);
  // The interrupt-acknowledge cycle's bus callback, for a maskable
  // interrupt (not NMI): called once the cycle has opened, so
  // bus_cycle_start is its first T and wait_sample_t its clear WAIT
  // sample slot, and before the core uses int_data, so a host can supply
  // the acknowledged byte here. NULL (the default) skips the call.
  void (*int_ack)(z80*);
  // During an IM 0 instruction, supplies every byte after the first (which
  // comes from z80_gen_int). Z80_IM0_INTA is a follow-on opcode byte after a
  // CB/ED/DD/FD prefix (an M1/IORQ cycle); Z80_IM0_READ is an operand byte
  // (n, nn, d), which is a normal memory read. pc is the frozen
  // pre-interrupt PC: it is never advanced while the instruction runs.
  // NULL (the default): INTA reads 0xFF (nobody drives the bus); READ is an
  // ordinary memory read at the frozen PC through read_byte with
  // Z80_READ_OPERAND, which is what plain memory answers when no device
  // overrides the read.
  uint8_t (*im0_bus)(z80*, uint16_t pc, z80_im0_cycle_t cycle);
  void* userdata;

  unsigned long cyc; // cycle count: the timing table's per-opcode costs,
                     // or, in Z80_CYC_FROM_WAIT_SLOTS, one charge per step
                     // from the WAIT-sampled M1 slots (see z80_cyc_source_t)
  const z80_timing_t* timing; // NULL until z80_init() sets z80_timing_generic
  z80_cyc_source_t cyc_source; // where the cyc charge comes from (above);
                               // Z80_CYC_FROM_TABLE until z80_set_cyc_source
                               // selects the WAIT-sampled slots
  uint8_t cyc_derived_step; // internal: latched at z80_step entry: whether
                            // this step derives the cycle count from the
                            // WAIT-sampled slots. The charge sites read the
                            // latch, OR im0_active, so a mask or source
                            // change inside a step cannot split one step's
                            // charges across the two modes.

  unsigned long t_counter; // T-state counter: every bus cycle and every
                           // internal stretch advances it. Without a WAIT
                           // mask one step's bus plus internal T equals the
                           // generic table cost the core charges to cyc
                           // above; a mask stretches bus cycles, and in
                           // Z80_CYC_FROM_WAIT_SLOTS cyc comes from the
                           // WAIT-sampled slots instead.
                           // A derived cyc charge spans M1 sample slot to
                           // M1 sample slot within one step only; absolute
                           // T is never converted.
  unsigned long bus_cycle_start; // absolute T of the bus cycle now running;
                                  // valid inside each bus callback (read,
                                  // write, port, im0 follow-on).
  uint8_t periodic_wait_mask; // WAIT pin waveform: a 4-bit periodic mask
                              // over the T counter, a set bit asserting WAIT
                              // at that T. The core stretches each bus cycle
                              // at its sample T out to the next clear slot;
                              // mask 0 (the default) stretches nothing, and a
                              // mask with no clear slot is ignored. The
                              // board owns the pattern.
  unsigned long wait_sample_t; // clear sample slot of the running bus
                               // cycle: the sample T advanced past any
                               // asserted bits. A dynamic hold re-applies
                               // the mask from this slot plus the held T.
  unsigned long int_sample_point; // last T of the stepped instruction:
                                  // the T at which the core samples INT,
                                  // recorded by z80_step before any
                                  // acknowledge. A host ticks the board up
                                  // to this T before checking interrupts.
  unsigned long step_m1_sample_tstate; // WAIT sample slot of the step's
                                       // first M1 fetch: the first slot at
                                       // or after the entry T counter the
                                       // mask leaves free (entry T plus one
                                       // when the mask stretches nothing).
                                       // Set once by z80_step on both
                                       // cycle-count sources and left alone
                                       // after, so a trailing acknowledge
                                       // still sees the instruction's own.
  bool port_high_byte_from_a; // set while a port callback runs for the
                              // two shapes whose port address takes its
                              // high byte from A (IN A,(n), OUT (n),A);
                              // clear for every other bus cycle.

  uint16_t pc, sp, ix, iy; // special purpose registers
  uint16_t mem_ptr; // "wz" register
  // Q latch behind SCF/CCF's undocumented X/Y flags: the F of the last
  // flag-writing instruction, or 0 after anything else. A CB, ED, DD or
  // FD prefix byte clears it, as does an accepted NMI or maskable
  // interrupt; POP AF and EX AF,AF' change F without latching it.
  uint8_t q;
  uint8_t flags_written; // internal: set when the running step writes the flags
  uint8_t a, b, c, d, e, h, l; // main registers
  uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
  uint8_t i, r; // interrupt vector, memory refresh

  // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
  bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

  // Set by EI: no interrupt is accepted at the boundary right after it.
  // Cleared when z80_step() starts the next instruction.
  uint8_t iff_delay;
  uint8_t interrupt_mode;
  uint8_t int_data;
  bool iff1 : 1, iff2 : 1;
  bool halted : 1;
  bool int_pending : 1, nmi_pending : 1;
  uint8_t im0_active; // internal: set while an IM 0 instruction runs off the bus
  // When set, z80_step() skips its trailing interrupt check after recording
  // int_sample_point (its Q latch still runs first): the host brings its INT
  // and NMI inputs up to that T and then calls z80_check_interrupt() itself,
  // which latches Q for an IM0 acknowledge run there the same way.
  // Clear (the default): z80_step() checks at once, as before.
  bool int_check_by_host;
};

// One /IORQ or /WR strobe window of the running bus cycle, in half-T
// counts (two per T-state): the strobe falls at begin and rises at end.
// Read inside a bus callback; the edges follow bus_cycle_start and
// wait_sample_t of the cycle then running.
typedef struct {
  unsigned long begin_half_tstates;
  unsigned long end_half_tstates;
} z80_strobe_window_t;

// The running cycle's /IORQ window: twice the cycle start T plus two to
// twice the WAIT sample slot plus three.
z80_strobe_window_t z80_iorq_window(const z80* const z);
// The running cycle's /WR window: twice the cycle start T plus three to
// twice the WAIT sample slot plus three.
z80_strobe_window_t z80_write_window(const z80* const z);

void z80_init(z80* const z);
void z80_step(z80* const z);
void z80_debug_output(z80* const z);
void z80_gen_nmi(z80* const z);
void z80_gen_int(z80* const z, uint8_t data);
void z80_set_timing(z80* const z, const z80_timing_t* timing);
void z80_set_cyc_source(z80* const z, z80_cyc_source_t source);

// Adds n T-states to the running bus cycle from inside one of its
// callbacks, for a device that holds WAIT itself. Only the T counter
// moves; the microsecond count is untouched.
void z80_add_wait_states(z80* const z, unsigned n);

// Services any pending NMI/interrupt immediately, without executing another
// instruction first. The host calls this after z80_step() when
// int_check_by_host is set, once its INT and NMI inputs reflect the level at
// int_sample_point; otherwise z80_step() already checks at its end, so other
// callers need this only if they generate an interrupt
// (z80_gen_int()/z80_gen_nmi()) *between* z80_step() calls and want it
// serviced right away instead of waiting for the next step's check. Right
// after EI nothing is accepted, the same as in z80_step()'s own check: the
// request stays pending until the instruction after EI has run.
void z80_check_interrupt(z80* const z);

#endif // Z80_Z80_H_
