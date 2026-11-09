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

static uint8_t read_byte(void *u, uint16_t a, z80_read_kind_t k) {
  (void)u;
  (void)k;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(active_cpu);
  return ram[a];
}

static void write_byte(void *u, uint16_t a, uint8_t v) {
  (void)u;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(active_cpu);
  ram[a] = v;
}

static uint8_t port_in(z80 *z, uint16_t p) {
  (void)p;
  (void)z;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(z);
  return 0;
}

static void port_out(z80 *z, uint16_t p, uint8_t v) {
  (void)p;
  (void)v;
  (void)z;
  if (event_count < MAX_EVENTS)
    event_start[event_count++] = CALLBACK_BUS_CYCLE_START(z);
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

/* Pins the full per-cycle start sequence of one step: count and every
   start T must match exactly. Expected shapes come from the Undocumented
   Z80 Documented (M-cycle/T-state tables) and, for the ED block and I/O
   ops, the per-T bus traces in the SingleStepTests subset. */
static void check_seq(const char *label, const unsigned *want, unsigned n) {
#if CHECK_BUS_TIMES
  if (event_count != n) {
    printf("FAIL %s: %u bus events, want %u\n", label, event_count, n);
    ++failures;
    return;
  }
  for (unsigned i = 0; i < n; ++i) {
    if (event_start[i] != want[i]) {
      printf("FAIL %s: event %u starts at %u, want %u\n", label, i, event_start[i], want[i]);
      ++failures;
    }
  }
#else
  (void)label;
  (void)want;
  (void)n;
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
    uint8_t b[8] = {(uint8_t)op, 0, 0, 0, 0, 0, 0, 0};
    load_bytes(b, 8);
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
    uint8_t b[2] = {0xCB, (uint8_t)op};
    load_bytes(b, 2);
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
    uint8_t b[2] = {0xED, (uint8_t)op};
    load_bytes(b, 2);
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
  uint8_t b[8] = {prefix, op, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < tail_n && 2 + i < 8; ++i)
    b[2 + i] = tail[i];
  load_bytes(b, 8);
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
        uint8_t t[2] = {0x00, (uint8_t)tail};
        run_ddfd(prefix, 0xCB, t, 2, tag);
      }
    } else {
      run_ddfd(prefix, (uint8_t)op, 0, 0, tag);
    }
  }
}

static void cond_case(const char *label, uint8_t op, const uint8_t *extra, int extra_n,
                      int take_flags, uint16_t want_pc, uint16_t want_sp) {
  z80 z;
  set_base(&z);
  uint8_t b[8] = {op, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < extra_n && 1 + i < 8; ++i)
    b[1 + i] = extra[i];
  load_bytes(b, 8);
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
    static const unsigned jr_seq[] = {0, 4};
    snprintf(label, sizeof(label), "jr %02X taken", jr_cases[i].op);
    cond_case(label, jr_cases[i].op, disp_plus4, 1, jr_cases[i].taken, 0x1006, 0xF000);
    check_seq(label, jr_seq, 2);
    snprintf(label, sizeof(label), "jr %02X not taken", jr_cases[i].op);
    cond_case(label, jr_cases[i].op, disp_plus4, 1, jr_cases[i].not_taken, 0x1002, 0xF000);
    check_seq(label, jr_seq, 2);
  }
  /* DJNZ *,d: taken while B - 1 is not 0. */
  {
    z80 z;
    static const unsigned djnz_seq[] = {0, 4};
    set_base(&z);
    z.b = 2;
    uint8_t b[8] = {0x10, 0x04, 0, 0, 0, 0, 0, 0};
    load_bytes(b, 8);
    begin_step(&z);
    z80_step(&z);
    end_step("djnz taken", &z);
    check_pc("djnz taken", &z, 0x1006);
    check_seq("djnz taken", djnz_seq, 2);
  }
  {
    z80 z;
    static const unsigned djnz_seq[] = {0, 4};
    set_base(&z);
    z.b = 1;
    uint8_t b[8] = {0x10, 0x04, 0, 0, 0, 0, 0, 0};
    load_bytes(b, 8);
    begin_step(&z);
    z80_step(&z);
    end_step("djnz not taken", &z);
    check_pc("djnz not taken", &z, 0x1002);
    check_seq("djnz not taken", djnz_seq, 2);
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
    static const unsigned call_taken[] = {0, 4, 7, 10, 13};
    static const unsigned call_skipped[] = {0, 4, 7};
    static const unsigned ret_taken[] = {0, 4, 7};
    static const unsigned ret_skipped[] = {0};
    int hold = 1 << cond_branches[i].bit;
    int taken = cond_branches[i].odd ? hold : 0;
    int skipped = cond_branches[i].odd ? 0 : hold;
    snprintf(label, sizeof(label), "jp %02X taken", cond_branches[i].jp);
    cond_case(label, cond_branches[i].jp, nn, 2, taken, 0x2000, 0xF000);
    snprintf(label, sizeof(label), "jp %02X not taken", cond_branches[i].jp);
    cond_case(label, cond_branches[i].jp, nn, 2, skipped, 0x1003, 0xF000);
    snprintf(label, sizeof(label), "call %02X taken", cond_branches[i].call);
    cond_case(label, cond_branches[i].call, nn, 2, taken, 0x2000, 0xEFFE);
    check_seq(label, call_taken, 5);
    snprintf(label, sizeof(label), "call %02X not taken", cond_branches[i].call);
    cond_case(label, cond_branches[i].call, nn, 2, skipped, 0x1003, 0xF000);
    check_seq(label, call_skipped, 3);
    /* RET pops 0x2000 off the stack when taken; the stack is set after
       the base setup wipes memory. */
    snprintf(label, sizeof(label), "ret %02X taken", cond_branches[i].ret);
    {
      z80 z;
      set_base(&z);
      uint8_t b[1] = {cond_branches[i].ret};
      load_bytes(b, 1);
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
      check_seq(label, ret_taken, 3);
      if (z.sp != 0xF002) {
        printf("FAIL %s: sp is %04X, want F002\n", label, z.sp);
        ++failures;
      }
    }
    snprintf(label, sizeof(label), "ret %02X not taken", cond_branches[i].ret);
    cond_case(label, cond_branches[i].ret, 0, 0, skipped, 0x1001, 0xF000);
    check_seq(label, ret_skipped, 1);
  }
}

/* One repeat-capable block op, repeating (pc rewinds) or ending. The B
   family (INIR/INDR/OTIR/OTDR) counts B, the BC family the whole BC;
   CPIR/CPDR also need no match to repeat. */
static void block_case(const char *label, uint8_t ed_op, int repeat, int use_b) {
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
  uint8_t b[2] = {0xED, ed_op};
  load_bytes(b, 2);
  begin_step(&z);
  z80_step(&z);
  end_step(label, &z);
  check_pc(label, &z, repeat ? 0x1000 : 0x1002);
}

static void block_cases(void) {
  /* Start shapes per op, repeat then end. The BC compare family and the
     LDI family share the LDIR shape; the B I/O family repeats the same
     bus events with or without the repeat extra. Shapes match the
     SingleStepTests bus traces. */
  static const struct {
    uint8_t op; int use_b; const char *name;
    unsigned repeat_n; unsigned repeat_seq[4];
    unsigned end_n; unsigned end_seq[4];
  } ops[] = {
    {0xB0, 0, "ldir", 4, {0, 4, 8, 11}, 4, {0, 4, 8, 11}},
    {0xB8, 0, "lddr", 4, {0, 4, 8, 11}, 4, {0, 4, 8, 11}},
    {0xB1, 0, "cpir", 4, {0, 4, 8, 11}, 3, {0, 4, 8}},
    {0xB9, 0, "cpdr", 4, {0, 4, 8, 11}, 3, {0, 4, 8}},
    {0xB2, 1, "inir", 4, {0, 4, 9, 13}, 4, {0, 4, 9, 13}},
    {0xBA, 1, "indr", 4, {0, 4, 9, 13}, 4, {0, 4, 9, 13}},
    {0xB3, 1, "otir", 4, {0, 4, 9, 12}, 4, {0, 4, 9, 12}},
    {0xBB, 1, "otdr", 4, {0, 4, 9, 12}, 4, {0, 4, 9, 12}},
  };
  for (unsigned i = 0; i < 8; ++i) {
    char label[40];
    snprintf(label, sizeof(label), "%s repeat", ops[i].name);
    block_case(label, ops[i].op, 1, ops[i].use_b);
    check_seq(label, ops[i].repeat_seq, ops[i].repeat_n);
    snprintf(label, sizeof(label), "%s end", ops[i].name);
    block_case(label, ops[i].op, 0, ops[i].use_b);
    check_seq(label, ops[i].end_seq, ops[i].end_n);
  }
}

/* EX (SP),HL swaps two stack bytes: two reads then two writes. */
static void ex_sp_case(void) {
  z80 z;
  static const unsigned seq[] = {0, 4, 7, 10, 13};
  set_base(&z);
  uint8_t b[1] = {0xE3};
  load_bytes(b, 1);
  ram[0xF000] = 0x34;
  ram[0xF001] = 0x12;
  begin_step(&z);
  z80_step(&z);
  end_step("ex (sp),hl", &z);
  check_pc("ex (sp),hl", &z, 0x1001);
  check_seq("ex (sp),hl", seq, 5);
  if (z.sp != 0xF000) {
    printf("FAIL ex (sp),hl: sp is %04X, want F000\n", z.sp);
    ++failures;
  }
}

/* IN r,(C) and OUT (C),r for every register: two fetches, port at 8. */
static void in_out_c_cases(void) {
  for (unsigned r = 0; r < 8; ++r) {
    z80 z;
    static const unsigned seq[] = {0, 4, 8};
    char label[40];
    snprintf(label, sizeof(label), "in %u,(c)", r);
    set_base(&z);
    uint8_t b[2] = {0xED, (uint8_t)(0x40 + 8 * r)};
    load_bytes(b, 2);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
    check_seq(label, seq, 3);
    snprintf(label, sizeof(label), "out (c),%u", r);
    set_base(&z);
    uint8_t c[2] = {0xED, (uint8_t)(0x41 + 8 * r)};
    load_bytes(c, 2);
    begin_step(&z);
    z80_step(&z);
    end_step(label, &z);
    check_seq(label, seq, 3);
  }
}

/* DDCB: displacement and op bytes are 3 T reads at 8 and 11; BIT stops
   after the (IX+d) read at 14, other forms write at 17. */
static void ddcb_cases(void) {
  static const unsigned bit_seq[] = {0, 4, 8, 11, 14};
  static const unsigned other_seq[] = {0, 4, 8, 11, 14, 17};
  z80 z;
  set_base(&z);
  uint8_t b[4] = {0xDD, 0xCB, 0x05, 0x46};
  load_bytes(b, 4);
  begin_step(&z);
  z80_step(&z);
  end_step("ddcb bit", &z);
  check_pc("ddcb bit", &z, 0x1004);
  check_seq("ddcb bit", bit_seq, 5);
  set_base(&z);
  uint8_t c[4] = {0xDD, 0xCB, 0x05, 0x06};
  load_bytes(c, 4);
  begin_step(&z);
  z80_step(&z);
  end_step("ddcb rlc", &z);
  check_pc("ddcb rlc", &z, 0x1004);
  check_seq("ddcb rlc", other_seq, 6);
}

/* LD (IX+d),n reads its n byte at 11, like the DDCB op byte. */
static void ix_d_n_case(void) {
  z80 z;
  static const unsigned seq[] = {0, 4, 8, 11, 14};
  set_base(&z);
  uint8_t b[4] = {0xDD, 0x36, 0x05, 0x42};
  load_bytes(b, 4);
  begin_step(&z);
  z80_step(&z);
  end_step("ld (ix+5),n", &z);
  check_pc("ld (ix+5),n", &z, 0x1004);
  check_seq("ld (ix+5),n", seq, 5);
  if (ram[0x4005] != 0x42) {
    printf("FAIL ld (ix+5),n: (IX+5) is %02X, want 42\n", ram[0x4005]);
    ++failures;
  }
}

/* kind: 0 = NMI, 1 = IM0, 2 = IM1, 3 = IM2. A NOP runs first, then the
   acknowledge; the IM0 byte is a NOP so nothing else executes. */
static void ack_case(const char *label, int kind, uint16_t want_pc) {
  z80 z;
  set_base(&z);
  z.iff1 = 1;
  uint8_t b[1] = {0x00};
  load_bytes(b, 1);
  if (kind == 0) {
    z80_gen_nmi(&z);
  } else {
    z.interrupt_mode = (uint8_t)(kind == 1 ? 0 : kind == 2 ? 1 : 2);
    if (kind == 3) {
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
  if (z.iff1) {
    printf("FAIL %s: iff1 still set after acknowledge\n", label);
    ++failures;
  }
}

static void ack_cases(void) {
  static const unsigned nmi_seq[] = {0, 9, 12};
  static const unsigned im0_seq[] = {0};
  static const unsigned im1_seq[] = {0, 11, 14};
  static const unsigned im2_seq[] = {0, 11, 14, 17, 20};
  ack_case("nmi", 0, 0x0066);
  check_seq("nmi", nmi_seq, 3);
  ack_case("im0", 1, 0x1001);
  check_seq("im0", im0_seq, 1);
  ack_case("im1", 2, 0x0038);
  check_seq("im1", im1_seq, 3);
  ack_case("im2", 3, 0x5678);
  check_seq("im2", im2_seq, 6);
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
  ack_cases();
  if (failures == 0)
    printf("tstate: all cases pass\n");
  else
    printf("tstate: %d failures\n", failures);
  return failures != 0;
}
