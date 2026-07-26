#ifndef Z80_TIMING_H
#define Z80_TIMING_H

#include <stdint.h>

// Pluggable per-opcode instruction timing. z80_t defaults to
// z80_timing_generic (bit-identical to stock superzazu/z80's hardcoded
// tables); call z80_set_timing() to switch to z80_timing_cpc for
// Amstrad CPC's 1us-quantized bus timing, or supply your own table for
// another platform.
typedef struct {
  uint8_t cyc_00[256];   // base opcode table
  uint8_t cyc_cb[256];   // CB-prefixed opcode table (full per-opcode cost)
  uint8_t cyc_ed[256];   // ED-prefixed opcode table
  uint8_t cyc_ddfd[256]; // DD/FD-prefixed opcode table (IX shown; IY identical)

  uint8_t ddfdcb_bit;    // DD/FD+CB, BIT y,(i+d)      -- full instruction cost
  uint8_t ddfdcb_other;  // DD/FD+CB, rot/res/set(i+d) -- full instruction cost

  uint8_t cond_call_taken_extra; // CALL cc,nn: added when condition true
  uint8_t cond_ret_taken_extra;  // RET cc: added when condition true
  uint8_t cond_jr_taken_extra;   // JR cc,d and DJNZ d: added when taken

  uint8_t block_repeat_extra;     // LDIR/LDDR/INIR/INDR/OTIR/OTDR:
                                   // added per non-final iteration
  uint8_t cpir_cpdr_repeat_extra; // CPIR/CPDR: added per non-final
                                   // iteration (diverges from the other
                                   // block-repeat ops on real CPC hardware)

  uint8_t nmi_ack;    // NMI acknowledge
  uint8_t int_ack_im0; // INT acknowledge, interrupt mode 0
  uint8_t int_ack_im1; // INT acknowledge, interrupt mode 1
  uint8_t int_ack_im2; // INT acknowledge, interrupt mode 2
} z80_timing_t;

extern const z80_timing_t z80_timing_generic;
extern const z80_timing_t z80_timing_cpc;

#endif
