/* Bus plus internal T-states match the generic timing table.
 *
 * For every opcode variant the new T counter must advance by exactly the
 * generic cost the core already charges to cyc: each taken and not-taken
 * conditional branch, DJNZ both ways, every block op repeating and ending,
 * and every interrupt acknowledge mode (NMI, IM0, IM1, IM2).
 *
 * The test never names a table value itself. After one z80_step it checks
 * the T counter delta against the cyc delta of that same step, which with
 * the default timing is the generic table entry plus the taken-branch and
 * block-repeat extras the core added. That the intended path really ran is
 * pinned separately through post-state checks (pc, sp), so a wrong setup
 * cannot silently pass as the other side of a conditional.
 *
 * While stepping, every bus callback records the cycle's start T through
 * the per-callback field; each recorded start must lie inside the step,
 * starts never go backwards, and two starts are never closer than 3 T.
 * A few sample cases additionally pin the full start sequence. No WAIT is
 * driven here (that is the WAIT-phase test's job) and no dynamic waits
 * are added, so the run is fully deterministic.
 *
 * Needs the timed-bus-cycles interface: the t_counter field in struct z80
 * (separate from the microsecond cyc) and the per-callback bus_cycle_start
 * field.
 * Compile with -DVALIDATE_DRIVER_AGAINST_CYC to check this driver against
 * the current cycle charging instead: expectations then go through cyc
 * and the start-T recording and start sequences are skipped. That is how
 * the committed test was validated before the T counter existed. A green
 * run under that flag proves the setups drive the intended paths and the
 * harness works; it proves nothing about the T counter or the start-T
 * field, which stay unchecked until the flag is off.
 */
#include <stdio.h>
#include <string.h>

#include "z80.h"
#include "z80_timing.h"

#ifdef VALIDATE_DRIVER_AGAINST_CYC
#define READ_T_COUNTER(z) ((z)->cyc)
#define CALLBACK_BUS_CYCLE_START(z) (0u)
#define CHECK_BUS_TIMES 0
#else
#define READ_T_COUNTER(z) ((z)->t_counter)
#define CALLBACK_BUS_CYCLE_START(z) ((z)->bus_cycle_start)
#define CHECK_BUS_TIMES 1
#endif

static uint8_t ram[0x10000];
static z80 *active_cpu;

#define MAX_EVENTS 128
static unsigned event_start[MAX_EVENTS];
static unsigned event_count;

static uint8_t read_byte(void *user_data, uint16_t addr, z80_read_kind_t kind) {
  (void)user_data;
  (void)kind;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(active_cpu);
  return ram[addr];
}

static void write_byte(void *user_data, uint16_t addr, uint8_t value) {
  (void)user_data;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(active_cpu);
  ram[addr] = value;
}

static uint8_t port_in(z80 *cpu, uint16_t port) {
  (void)port;
  (void)cpu;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(cpu);
  return 0;
}

static void port_out(z80 *cpu, uint16_t port, uint8_t value) {
  (void)port;
  (void)value;
  (void)cpu;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(cpu);
}

static int failures;

static void set_base(z80 *z) {
  memset(ram, 0, sizeof(ram));
  z80_init(z);
  z->read_byte = read_byte;
  z->write_byte = write_byte;
  z->port_in = port_in;
  z->port_out = port_out;
  z->userdata = 0;
  z->pc = 0x1000;
  z->sp = 0xF000;
  z->ix = 0x4000;
  z->iy = 0x5000;
  z->a = 0;
  z->b = 1;
  z->c = 1;
  z->d = 0x50;
  z->e = 0;
  z->h = 0x40;
  z->l = 0;
  z->a_ = z->b_ = z->c_ = z->d_ = z->e_ = z->f_ = z->h_ = z->l_ = 0;
  z->i = 0;
  z->r = 0;
  z->mem_ptr = 0;
  z->q = 0;
  z->sf = z->zf = z->yf = z->hf = z->xf = z->pf = z->nf = z->cf = 0;
  z->iff_delay = 0;
  z->interrupt_mode = 1;
  z->int_data = 0;
  z->iff1 = 0;
  z->iff2 = 0;
  z->halted = 0;
  z->int_pending = 0;
  z->nmi_pending = 0;
  z->im0_active = 0;
}

static unsigned t_counter_before, cyc_before;

static void begin_step(z80 *z) {
  event_count = 0;
  active_cpu = z;
  t_counter_before = READ_T_COUNTER(z);
  cyc_before = z->cyc;
}

static void end_step(const char *label, z80 *z) {
  unsigned delta_t = READ_T_COUNTER(z) - t_counter_before;
  unsigned delta_cyc = z->cyc - cyc_before;
  if (delta_t != delta_cyc) {
    printf("FAIL %s: bus plus internal T is %u, generic cost is %u\n", label, delta_t, delta_cyc);
    ++failures;
  }
#if CHECK_BUS_TIMES
  if (event_count == 0) {
    printf("FAIL %s: no bus callbacks recorded\n", label);
    ++failures;
    return;
  }
  for (unsigned i = 0; i < event_count; ++i) {
    if (event_start[i] >= delta_t) {
      printf("FAIL %s: bus start T %u outside the step (total %u)\n", label, event_start[i], delta_t);
      ++failures;
      break;
    }
    if (i > 0) {
      if (event_start[i] < event_start[i - 1]) {
        printf("FAIL %s: bus start T %u before previous %u\n", label, event_start[i], event_start[i - 1]);
        ++failures;
        break;
      }
      /* Every bus cycle holds the bus for at least 3 T, so two callbacks
         can never start closer together than that. */
      if (event_start[i] - event_start[i - 1] < 3) {
        printf("FAIL %s: bus starts %u and %u less than 3 T apart\n", label, event_start[i - 1], event_start[i]);
        ++failures;
        break;
      }
    }
  }
  /* The last cycle runs at least 3 T past its start, so it must still
     end within the step. */
  if (event_count > 0 && event_start[event_count - 1] + 3 > delta_t) {
    printf("FAIL %s: last bus start T %u ends past the step (total %u)\n", label, event_start[event_count - 1], delta_t);
    ++failures;
  }
#else
  (void)label;
  (void)z;
#endif
}

static void check_pc(const char *label, z80 *z, uint16_t want) {
  if (z->pc != want) {
    printf("FAIL %s: pc is %04X, want %04X (wrong path ran)\n", label, z->pc, want);
    ++failures;
  }
}

/* Pins one step against its M-cycle row: positive entries are bus cycles
   (their length in T, one callback each, in order), negative entries are
   internal stretches with no callback. Two checks always run, even in a
   validation build without the new fields: the row lengths must sum to the
   charged generic cost, and the recorded callback count must match the
   number of bus entries (so a wrong bus/internal split fails there too).
   With the new fields present, every recorded start T must equal the
   cumulative row position as well.
   Row sources: Sean Young's Undocumented Z80 Documented M-cycle/T-state
   tables for the branch, stack, indexed and DDCB shapes; the per-T bus
   traces in the SingleStepTests subset for the ED block and I/O shapes.
   Two rows need a note. OUTI/OTIR read (HL) before touching the port, so
   in time order they are 4,5,3,4 (read at 9, OUT at 12); the Zilog
   instruction list prints 4,5,4,3, which transposes the 3 T read and the
   4 T port write against the per-T trace, so the trace wins here. The
   interrupt acknowledges have no INTA callback in this core (the first
   byte comes from z80_gen_int, follow-on bytes from im0_bus or plain
   reads), hence the negative response entries. */
static void check_trace(const char *label, z80 *z, const int *rows, unsigned nrows) {
  unsigned total = 0, nbus = 0;
  for (unsigned i = 0; i < nrows; ++i) {
    total += (unsigned)(rows[i] < 0 ? -rows[i] : rows[i]);
    if (rows[i] > 0)
      ++nbus;
  }
  unsigned delta_cyc = z->cyc - cyc_before;
  if (total != delta_cyc) {
    printf("FAIL %s: M-cycle lengths sum to %u, generic cost is %u\n", label, total, delta_cyc);
    ++failures;
  }
  if (event_count != nbus) {
    printf("FAIL %s: %u bus events, row has %u bus cycles\n", label, event_count, nbus);
    ++failures;
    return;
  }
#if CHECK_BUS_TIMES
  {
    unsigned start = 0;
    unsigned event_index = 0;
    for (unsigned i = 0; i < nrows; ++i) {
      if (rows[i] < 0) {
        start += (unsigned)-rows[i];
        continue;
      }
      if (event_start[event_index] != start) {
        printf("FAIL %s: event %u starts at %u, row gives %u\n", label, event_index, event_start[event_index], start);
        ++failures;
      }
      start += (unsigned)rows[i];
      ++event_index;
    }
  }
#else
  (void)label;
  (void)z;
  (void)rows;
  (void)nrows;
#endif
}

static void load_bytes(const uint8_t *b, int n) {
  memcpy(ram + 0x1000, b, (size_t)n);
}

/* Conditional base opcodes, covered both ways below instead of here. */
static int is_cond_base(uint8_t op) {
  if (op == 0x10 || op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38)
    return 1;
  switch (op) {
  case 0xC2: case 0xCA: case 0xD2: case 0xDA:
  case 0xE2: case 0xEA: case 0xF2: case 0xFA:
  case 0xC4: case 0xCC: case 0xD4: case 0xDC:
  case 0xE4: case 0xEC: case 0xF4: case 0xFC:
  case 0xC0: case 0xC8: case 0xD0: case 0xD8:
  case 0xE0: case 0xE8: case 0xF0: case 0xF8:
    return 1;
  default:
    return 0;
  }
}

static void sweep_base(void) {
  for (unsigned op = 0; op < 256; ++op) {
    if (is_cond_base((uint8_t)op))
      continue;
    z80 z;
    set_base(&z);
    uint8_t code[8] = {(uint8_t)op, 0, 0, 0, 0, 0, 0, 0};
    load_bytes(code, 8);
    char label[32];
    snprintf(label, sizeof(label), "base %02X", op);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
  }
}

static void sweep_cb(void) {
  for (unsigned op = 0; op < 256; ++op) {
    z80 z;
    set_base(&z);
    uint8_t code[2] = {0xCB, (uint8_t)op};
    load_bytes(code, 2);
    char label[32];
    snprintf(label, sizeof(label), "cb %02X", op);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
  }
}

static void sweep_ed(void) {
  for (unsigned op = 0; op < 256; ++op) {
    z80 z;
    set_base(&z);
    /* B and BC of 1: no block iteration ever repeats here. */
    z.b = 1;
    z.c = 1;
    uint8_t code[2] = {0xED, (uint8_t)op};
    load_bytes(code, 2);
    char label[32];
    snprintf(label, sizeof(label), "ed %02X", op);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
  }
}

/* One DD/FD second byte; tail holds the extra DDCB bytes when op is 0xCB. */
static void run_ddfd(uint8_t prefix, uint8_t op, const uint8_t *tail, int tail_n, const char *tag) {
  z80 z;
  set_base(&z);
  uint8_t code[8] = {prefix, op, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < tail_n && 2 + i < 8; ++i)
    code[2 + i] = tail[i];
  load_bytes(code, 8);
  char label[48];
  if (op == 0xCB)
    snprintf(label, sizeof(label), "%s cb %02X", tag, tail_n > 1 ? tail[1] : 0);
  else
    snprintf(label, sizeof(label), "%s %02X", tag, op);
  begin_step(&z);
  z80_step(&z);
  end_step(label, &z);
}

static void sweep_ddfd(uint8_t prefix, const char *tag) {
  for (unsigned op = 0; op < 256; ++op) {
    if (op == 0xCB) {
      for (unsigned tail = 0; tail < 256; ++tail) {
        uint8_t tail_bytes[2] = {0x00, (uint8_t)tail};
        run_ddfd(prefix, 0xCB, tail_bytes, 2, tag);
      }
    } else {
      run_ddfd(prefix, (uint8_t)op, 0, 0, tag);
    }
  }
}

static void cond_case(const char *label, uint8_t op, const uint8_t *extra, int extra_n,
                      int take_flags, uint16_t want_pc, uint16_t want_sp,
                      const int *rows, unsigned nrows) {
  z80 z;
  set_base(&z);
  uint8_t code[8] = {op, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < extra_n && 1 + i < 8; ++i)
    code[1 + i] = extra[i];
  load_bytes(code, 8);
  /* take_flags picks the flag setup: bit 0 sets zf, 1 sets cf, 2 sets pf,
     3 sets sf; taken means the branch condition holds. */
  z.zf = (take_flags & 1) != 0;
  z.cf = (take_flags & 2) != 0;
  z.pf = (take_flags & 4) != 0;
  z.sf = (take_flags & 8) != 0;
  begin_step(&z);
  z80_step(&z);
  end_step(label, &z);
  check_pc(label, &z, want_pc);
  check_trace(label, &z, rows, nrows);
  if (z.sp != want_sp) {
    printf("FAIL %s: sp is %04X, want %04X\n", label, z.sp, want_sp);
    ++failures;
  }
}

static void cond_cases(void) {
  /* JR cc,d with d = 4: taken lands at 0x1006, not taken at 0x1002. */
  const uint8_t disp_plus4[1] = {0x04};
  static const struct { uint8_t op; int taken; int not_taken; } jr_cases[] = {
    {0x20, 0, 1}, /* nz: taken with zf clear */
    {0x28, 1, 0}, /* z: taken with zf set */
    {0x30, 0, 2}, /* nc: taken with cf clear */
    {0x38, 2, 0}, /* c: taken with cf set */
  };
  for (unsigned i = 0; i < 4; ++i) {
    char label[40];
    /* JR: 4,3 taken plus 5 internal; not taken 4,3. */
    static const int jr_taken[] = {4, 3, -5};
    static const int jr_not[] = {4, 3};
    snprintf(label, sizeof(label), "jr %02X taken", jr_cases[i].op);
    cond_case(label, jr_cases[i].op, disp_plus4, 1, jr_cases[i].taken, 0x1006, 0xF000,
              jr_taken, 3);
    snprintf(label, sizeof(label), "jr %02X not taken", jr_cases[i].op);
    cond_case(label, jr_cases[i].op, disp_plus4, 1, jr_cases[i].not_taken, 0x1002, 0xF000,
              jr_not, 2);
  }
  /* DJNZ *,d: 5 T fetch with the B decrement inside; taken adds 5
     internal, not taken 5,3. */
  {
    z80 z;
    static const int djnz_taken[] = {5, 3, -5};
    set_base(&z);
    z.b = 2;
    uint8_t code[8] = {0x10, 0x04, 0, 0, 0, 0, 0, 0};
    load_bytes(code, 8);
    begin_step(&z);
    z80_step(&z);
    end_step("djnz taken", &z);
    check_pc("djnz taken", &z, 0x1006);
    check_trace("djnz taken", &z, djnz_taken, 3);
  }
  {
    z80 z;
    static const int djnz_not[] = {5, 3};
    set_base(&z);
    z.b = 1;
    uint8_t code[8] = {0x10, 0x04, 0, 0, 0, 0, 0, 0};
    load_bytes(code, 8);
    begin_step(&z);
    z80_step(&z);
    end_step("djnz not taken", &z);
    check_pc("djnz not taken", &z, 0x1002);
    check_trace("djnz not taken", &z, djnz_not, 2);
  }
  /* JP/CALL/RET cc with nn = 0x2000. taken holds the flag bit for the
     odd conditions (z, c, pe, m) and clears it for the even ones
     (nz, nc, po, p). */
  const uint8_t nn[2] = {0x00, 0x20};
  static const struct { uint8_t jp, call, ret; int bit; int odd; } cond_branches[] = {
    {0xC2, 0xC4, 0xC0, 0, 0}, {0xCA, 0xCC, 0xC8, 0, 1},
    {0xD2, 0xD4, 0xD0, 1, 0}, {0xDA, 0xDC, 0xD8, 1, 1},
    {0xE2, 0xE4, 0xE0, 2, 0}, {0xEA, 0xEC, 0xE8, 2, 1},
    {0xF2, 0xF4, 0xF0, 3, 0}, {0xFA, 0xFC, 0xF8, 3, 1},
  };
  for (unsigned i = 0; i < 8; ++i) {
    char label[40];
    /* JP: 4,3,3 either way. CALL: taken 4,3,4,3,3, not taken 4,3,3.
       RET: taken 5,3,3, not taken 5. */
    static const int jp_row[] = {4, 3, 3};
    static const int call_taken[] = {4, 3, 4, 3, 3};
    static const int call_not[] = {4, 3, 3};
    static const int ret_taken[] = {5, 3, 3};
    static const int ret_not[] = {5};
    int hold = 1 << cond_branches[i].bit;
    int taken = cond_branches[i].odd ? hold : 0;
    int skipped = cond_branches[i].odd ? 0 : hold;
    snprintf(label, sizeof(label), "jp %02X taken", cond_branches[i].jp);
    cond_case(label, cond_branches[i].jp, nn, 2, taken, 0x2000, 0xF000, jp_row, 3);
    snprintf(label, sizeof(label), "jp %02X not taken", cond_branches[i].jp);
    cond_case(label, cond_branches[i].jp, nn, 2, skipped, 0x1003, 0xF000, jp_row, 3);
    snprintf(label, sizeof(label), "call %02X taken", cond_branches[i].call);
    cond_case(label, cond_branches[i].call, nn, 2, taken, 0x2000, 0xEFFE, call_taken, 5);
    snprintf(label, sizeof(label), "call %02X not taken", cond_branches[i].call);
    cond_case(label, cond_branches[i].call, nn, 2, skipped, 0x1003, 0xF000, call_not, 3);
    /* RET pops 0x2000 off the stack when taken; the stack is set after
       the base setup wipes memory. */
    snprintf(label, sizeof(label), "ret %02X taken", cond_branches[i].ret);
    {
      z80 z;
      set_base(&z);
      uint8_t code[1] = {cond_branches[i].ret};
      load_bytes(code, 1);
      ram[0xF000] = 0x00;
      ram[0xF001] = 0x20;
      z.sf = z.zf = z.yf = z.hf = z.xf = z.pf = z.nf = z.cf = 0;
      z.zf = (taken & 1) != 0;
      z.cf = (taken & 2) != 0;
      z.pf = (taken & 4) != 0;
      z.sf = (taken & 8) != 0;
      begin_step(&z);
      z80_step(&z);
      end_step(label, &z);
      check_pc(label, &z, 0x2000);
      check_trace(label, &z, ret_taken, 3);
      if (z.sp != 0xF002) {
        printf("FAIL %s: sp is %04X, want F002\n", label, z.sp);
        ++failures;
      }
    }
    snprintf(label, sizeof(label), "ret %02X not taken", cond_branches[i].ret);
    cond_case(label, cond_branches[i].ret, 0, 0, skipped, 0x1001, 0xF000, ret_not, 1);
  }
}

/* One repeat-capable block op, repeating (pc rewinds) or ending. The B
   family (INIR/INDR/OTIR/OTDR) counts B, the BC family the whole BC;
   CPIR/CPDR also need no match to repeat. */
static void block_case(const char *label, uint8_t ed_op, int repeat, int use_b,
                       const int *rows, unsigned nrows) {
  z80 z;
  set_base(&z);
  if (use_b) {
    z.b = repeat ? 2 : 1;
  } else {
    z.b = 0;
    z.c = repeat ? 2 : 1;
  }
  if (repeat && (ed_op == 0xB1 || ed_op == 0xB9))
    ram[0x4000] = 1; /* A reads an unequal byte, so no match ends it */
  uint8_t code[2] = {0xED, ed_op};
  load_bytes(code, 2);
  begin_step(&z);
  z80_step(&z);
  end_step(label, &z);
  check_pc(label, &z, repeat ? 0x1000 : 0x1002);
  check_trace(label, &z, rows, nrows);
}

static void block_cases(void) {
  /* M-cycle rows, repeat then end. The BC memory family is 4,4,3,5 with
     a 5 T repeat tail; CPIR/CPDR compare without writing, so their tail
     and end rows mark the last 5 T internal and repeat with 3 bus
     events. The B I/O family is 4,5,4,3 for INI/IND (port read at 9,
     (HL) write at 13) and 4,5,3,4 for OUTI/OUTD ((HL) read at 9, port
     write at 12), each with a 5 T internal repeat tail. All shapes match
     the SingleStepTests bus traces. */
  static const int bc_repeat[] = {4, 4, 3, 5, -5};
  static const int bc_end[] = {4, 4, 3, 5};
  static const int cp_repeat[] = {4, 4, 3, -5, -5};
  static const int cp_end[] = {4, 4, 3, -5};
  static const int in_repeat[] = {4, 5, 4, 3, -5};
  static const int in_end[] = {4, 5, 4, 3};
  static const int out_repeat[] = {4, 5, 3, 4, -5};
  static const int out_end[] = {4, 5, 3, 4};
  static const struct {
    uint8_t op; int use_b; const char *name;
    const int *repeat_rows; unsigned repeat_n;
    const int *end_rows; unsigned end_n;
  } ops[] = {
    {0xB0, 0, "ldir", bc_repeat, 5, bc_end, 4},
    {0xB8, 0, "lddr", bc_repeat, 5, bc_end, 4},
    {0xB1, 0, "cpir", cp_repeat, 5, cp_end, 4},
    {0xB9, 0, "cpdr", cp_repeat, 5, cp_end, 4},
    {0xB2, 1, "inir", in_repeat, 5, in_end, 4},
    {0xBA, 1, "indr", in_repeat, 5, in_end, 4},
    {0xB3, 1, "otir", out_repeat, 5, out_end, 4},
    {0xBB, 1, "otdr", out_repeat, 5, out_end, 4},
  };
  for (unsigned i = 0; i < 8; ++i) {
    char label[40];
    snprintf(label, sizeof(label), "%s repeat", ops[i].name);
    block_case(label, ops[i].op, 1, ops[i].use_b, ops[i].repeat_rows, ops[i].repeat_n);
    snprintf(label, sizeof(label), "%s end", ops[i].name);
    block_case(label, ops[i].op, 0, ops[i].use_b, ops[i].end_rows, ops[i].end_n);
  }
}

/* EX (SP),HL: 4,3,4,3,5, two stack reads then two writes. */
static void ex_sp_case(void) {
  z80 z;
  static const int rows[] = {4, 3, 4, 3, 5};
  set_base(&z);
  uint8_t code[1] = {0xE3};
  load_bytes(code, 1);
  ram[0xF000] = 0x34;
  ram[0xF001] = 0x12;
  begin_step(&z);
  z80_step(&z);
  end_step("ex (sp),hl", &z);
  check_pc("ex (sp),hl", &z, 0x1001);
  check_trace("ex (sp),hl", &z, rows, 5);
  if (z.sp != 0xF000) {
    printf("FAIL ex (sp),hl: sp is %04X, want F000\n", z.sp);
    ++failures;
  }
}

/* IN r,(C) and OUT (C),r for every register: 4,4,4, port at 8. */
static void in_out_c_cases(void) {
  for (unsigned r = 0; r < 8; ++r) {
    z80 z;
    static const int rows[] = {4, 4, 4};
    char label[40];
    snprintf(label, sizeof(label), "in %u,(c)", r);
    set_base(&z);
    uint8_t code_in[2] = {0xED, (uint8_t)(0x40 + 8 * r)};
    load_bytes(code_in, 2);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
    check_trace(label, &z, rows, 3);
    snprintf(label, sizeof(label), "out (c),%u", r);
    set_base(&z);
    uint8_t code_out[2] = {0xED, (uint8_t)(0x41 + 8 * r)};
    load_bytes(code_out, 2);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
    check_trace(label, &z, rows, 3);
  }
}

/* DDCB: displacement and op bytes are 3 T reads at 8 and 11; BIT stops
   after the (IX+d) read at 16, other forms write at 20. */
static void ddcb_cases(void) {
  static const int bit_rows[] = {4, 4, 3, 5, 4};
  static const int other_rows[] = {4, 4, 3, 5, 4, 3};
  z80 z;
  set_base(&z);
  uint8_t code_bit[4] = {0xDD, 0xCB, 0x05, 0x46};
  load_bytes(code_bit, 4);
  begin_step(&z);
  z80_step(&z);
  end_step("ddcb bit", &z);
  check_pc("ddcb bit", &z, 0x1004);
  check_trace("ddcb bit", &z, bit_rows, 5);
  set_base(&z);
  uint8_t code_rlc[4] = {0xDD, 0xCB, 0x05, 0x06};
  load_bytes(code_rlc, 4);
  begin_step(&z);
  z80_step(&z);
  end_step("ddcb rlc", &z);
  check_pc("ddcb rlc", &z, 0x1004);
  check_trace("ddcb rlc", &z, other_rows, 6);
}

/* LD r,(IX+d) and ALU (IX+d) read the data byte after the 5 internal
   displacement-add T, so the read sits at 16, not 11. */
static void ix_d_read_cases(void) {
  static const int rows[] = {4, 4, 3, -5, 3};
  z80 z;
  set_base(&z);
  uint8_t code_ld[3] = {0xDD, 0x46, 0x05};
  load_bytes(code_ld, 3);
  begin_step(&z);
  z80_step(&z);
  end_step("ld b,(ix+5)", &z);
  check_pc("ld b,(ix+5)", &z, 0x1003);
  check_trace("ld b,(ix+5)", &z, rows, 5);
  set_base(&z);
  uint8_t code_add[3] = {0xDD, 0x86, 0x05};
  load_bytes(code_add, 3);
  begin_step(&z);
  z80_step(&z);
  end_step("add a,(ix+5)", &z);
  check_pc("add a,(ix+5)", &z, 0x1003);
  check_trace("add a,(ix+5)", &z, rows, 5);
}
/* LD (IX+d),n reads its n byte at 11, like the DDCB op byte. */
static void ix_d_n_case(void) {
  z80 z;
  static const int rows[] = {4, 4, 3, 5, 3};
  set_base(&z);
  uint8_t code[4] = {0xDD, 0x36, 0x05, 0x42};
  load_bytes(code, 4);
  begin_step(&z);
  z80_step(&z);
  end_step("ld (ix+5),n", &z);
  check_pc("ld (ix+5),n", &z, 0x1004);
  check_trace("ld (ix+5),n", &z, rows, 5);
  if (ram[0x4005] != 0x42) {
    printf("FAIL ld (ix+5),n: (IX+5) is %02X, want 42\n", ram[0x4005]);
    ++failures;
  }
}

/* IM 0 with a prefix byte: the diverted fetch, the first INTA, the
   follow-on INTA carrying the prefixed opcode, then the off-bus body. */
static uint8_t im0_follow;
static uint8_t im0_bus_override(z80 *z, uint16_t pc, z80_im0_cycle_t cycle) {
  (void)z;
  (void)pc;
  (void)cycle;
  return im0_follow;
}

static void im0_prefix_cases(void) {
  static const int rows[] = {4, -6, -6};
  z80 z;
  set_base(&z);
  z.im0_bus = im0_bus_override;
  z.interrupt_mode = 0;
  z.iff1 = 1;
  im0_follow = 0x00;
  uint8_t code_cb[1] = {0x00};
  load_bytes(code_cb, 1);
  z80_gen_int(&z, 0xCB);
  begin_step(&z);
  z80_step(&z);
  end_step("im0 cb 00", &z);
  check_pc("im0 cb 00", &z, 0x1001);
  check_trace("im0 cb 00", &z, rows, 3);
  set_base(&z);
  z.im0_bus = im0_bus_override;
  z.interrupt_mode = 0;
  z.iff1 = 1;
  im0_follow = 0x44;
  uint8_t code_ed[1] = {0x00};
  load_bytes(code_ed, 1);
  z80_gen_int(&z, 0xED);
  begin_step(&z);
  z80_step(&z);
  end_step("im0 ed 44", &z);
  check_pc("im0 ed 44", &z, 0x1001);
  check_trace("im0 ed 44", &z, rows, 3);
  if (z.iff1) {
    printf("FAIL im0 prefix: iff1 still set after acknowledge\n");
    ++failures;
  }
}

/* The T path never reads the timing table (F3 cover): the same steps run
   under the CPC microsecond table must still advance the generic T count.
   Only totals and PCs are pinned here; callbacks and starts are identical
   on either table and covered above. */
static void cpc_t_check(const char *label, z80 *z, unsigned want_t,
                         uint16_t want_pc) {
  unsigned before = READ_T_COUNTER(z);
  z80_step(z);
  unsigned delta = READ_T_COUNTER(z) - before;
  if (delta != want_t) {
    printf("FAIL %s: T is %u under the CPC table, generic cost is %u\n", label, delta, want_t);
    ++failures;
  }
  check_pc(label, z, want_pc);
}

static void cpc_table_cases(void) {
  z80 z;
  /* DJNZ taken: 13 T. */
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.b = 2;
  uint8_t code_djnz[2] = {0x10, 0x04};
  load_bytes(code_djnz, 2);
  begin_step(&z);
  cpc_t_check("cpc djnz taken", &z, 13, 0x1006);
  /* JR taken: 12 T. */
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.zf = 0;
  uint8_t code_jr[2] = {0x20, 0x04};
  load_bytes(code_jr, 2);
  begin_step(&z);
  cpc_t_check("cpc jr nz taken", &z, 12, 0x1006);
  /* JR NC taken: 12 T. */
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.cf = 0;
  uint8_t code_jrnc[2] = {0x30, 0x04};
  load_bytes(code_jrnc, 2);
  begin_step(&z);
  cpc_t_check("cpc jr nc taken", &z, 12, 0x1006);
  /* LDIR repeating: 21 T. */
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.b = 0;
  z.c = 2;
  uint8_t code_ldir[2] = {0xED, 0xB0};
  load_bytes(code_ldir, 2);
  begin_step(&z);
  cpc_t_check("cpc ldir repeat", &z, 21, 0x1000);
  /* CPIR repeating: 21 T. */
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.b = 0;
  z.c = 2;
  ram[0x4000] = 1;
  uint8_t code_cpir[2] = {0xED, 0xB1};
  load_bytes(code_cpir, 2);
  begin_step(&z);
  cpc_t_check("cpc cpir repeat", &z, 21, 0x1000);
  /* Acknowledges: NMI 15, IM0 10, IM1 17, IM2 23 T. */
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.iff1 = 1;
  uint8_t code_nop[1] = {0x00};
  load_bytes(code_nop, 1);
  z80_gen_nmi(&z);
  begin_step(&z);
  cpc_t_check("cpc nmi", &z, 15, 0x0066);
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.iff1 = 1;
  z.interrupt_mode = 0;
  load_bytes(code_nop, 1);
  z80_gen_int(&z, 0x00);
  begin_step(&z);
  cpc_t_check("cpc im0", &z, 10, 0x1001);
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.iff1 = 1;
  z.interrupt_mode = 1;
  load_bytes(code_nop, 1);
  z80_gen_int(&z, 0x00);
  begin_step(&z);
  cpc_t_check("cpc im1", &z, 17, 0x0038);
  set_base(&z);
  z.timing = &z80_timing_cpc;
  z.iff1 = 1;
  z.interrupt_mode = 2;
  z.i = 0x12;
  ram[0x1234] = 0x78;
  ram[0x1235] = 0x56;
  load_bytes(code_nop, 1);
  z80_gen_int(&z, 0x34);
  begin_step(&z);
  cpc_t_check("cpc im2", &z, 23, 0x5678);
  if (z.iff1) {
    printf("FAIL cpc acks: iff1 still set after acknowledge\n");
    ++failures;
  }
}

/* The acknowledge modes the test drives: NMI, IM0, IM1, IM2. A NOP runs
   first, then the acknowledge; the IM0 byte is a NOP so nothing else
   executes. Rows start with the NOP's 4 T fetch; the acknowledge
   response itself emits no callback (the first byte comes from
   z80_gen_int), then come the stack pushes and, for IM2, the two
   vector-table reads. */
enum ack_kind { ACK_NMI, ACK_IM0, ACK_IM1, ACK_IM2 };

static void ack_case(const char *label, enum ack_kind kind, uint16_t want_pc,
                     const int *rows, unsigned nrows) {
  z80 z;
  set_base(&z);
  z.iff1 = 1;
  uint8_t code[1] = {0x00};
  load_bytes(code, 1);
  if (kind == ACK_NMI) {
    z80_gen_nmi(&z);
  } else {
    z.interrupt_mode = (uint8_t)(kind == ACK_IM0 ? 0 : kind == ACK_IM1 ? 1 : 2);
    if (kind == ACK_IM2) {
      z.i = 0x12;
      ram[0x1234] = 0x78;
      ram[0x1235] = 0x56;
      z80_gen_int(&z, 0x34);
    } else {
      z80_gen_int(&z, 0x00);
    }
  }
  begin_step(&z);
  z80_step(&z);
  end_step(label, &z);
  check_pc(label, &z, want_pc);
  check_trace(label, &z, rows, nrows);
  if (z.iff1) {
    printf("FAIL %s: iff1 still set after acknowledge\n", label);
    ++failures;
  }
}

static void ack_cases(void) {
  /* Totals match Young §5: NMI 4+11, IM0 4+2+4, IM1 4+13, IM2 4+19. */
  static const int nmi_rows[] = {4, -5, 3, 3};
  static const int im0_rows[] = {4, -6};
  static const int im1_rows[] = {4, -7, 3, 3};
  static const int im2_rows[] = {4, -7, 3, 3, 3, 3};
  ack_case("nmi", ACK_NMI, 0x0066, nmi_rows, 4);
  ack_case("im0", ACK_IM0, 0x1001, im0_rows, 2);
  ack_case("im1", ACK_IM1, 0x0038, im1_rows, 4);
  ack_case("im2", ACK_IM2, 0x5678, im2_rows, 6);
}

int main(void) {
  sweep_base();
  sweep_cb();
  sweep_ed();
  sweep_ddfd(0xDD, "dd");
  sweep_ddfd(0xFD, "fd");
  cond_cases();
  block_cases();
  ex_sp_case();
  in_out_c_cases();
  ddcb_cases();
  ix_d_n_case();
  ix_d_read_cases();
  im0_prefix_cases();
  cpc_table_cases();
  ack_cases();
  if (failures == 0)
    printf("tstate: all cases pass\n");
  else
    printf("tstate: %d failures\n", failures);
  return failures != 0;
}
