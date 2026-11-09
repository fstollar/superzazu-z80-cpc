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
 * the per-callback field; each recorded start must lie inside the step.
 * No WAIT is driven here (that is the T2 test's job) and no dynamic waits
 * are added, so the run is fully deterministic.
 *
 * Needs the timed-bus-cycles interface: the T counter in struct z80
 * (separate from the microsecond cyc) and the per-callback start-T field.
 * Compile with -DT1_VALIDATE_DRIVER to check this driver against the
 * current cycle charging instead: expectations then go through cyc and
 * the start-T recording is skipped. That is how the committed test was
 * validated before the T counter existed.
 */
#include <stdio.h>
#include <string.h>

#include "z80.h"
#include "z80_timing.h"

#ifdef T1_VALIDATE_DRIVER
#define CUR_T(z) ((z)->cyc)
#define CB_T_NOW(z) (0u)
#define CHECK_BUS_TIMES 0
#else
#define CUR_T(z) ((z)->t)
#define CB_T_NOW(z) ((z)->bus_t)
#define CHECK_BUS_TIMES 1
#endif

static uint8_t ram[0x10000];
static z80 *g_z;

#define MAX_EV 128
static unsigned ev_t[MAX_EV];
static unsigned ev_n;

static uint8_t read_byte(void *u, uint16_t a, z80_read_kind_t k) {
  (void)u;
  (void)k;
  if (ev_n < MAX_EV)
    ev_t[ev_n++] = CB_T_NOW(g_z);
  return ram[a];
}

static void write_byte(void *u, uint16_t a, uint8_t v) {
  (void)u;
  if (ev_n < MAX_EV)
    ev_t[ev_n++] = CB_T_NOW(g_z);
  ram[a] = v;
}

static uint8_t port_in(z80 *z, uint16_t p) {
  (void)p;
  (void)z;
  if (ev_n < MAX_EV)
    ev_t[ev_n++] = CB_T_NOW(z);
  return 0;
}

static void port_out(z80 *z, uint16_t p, uint8_t v) {
  (void)p;
  (void)v;
  (void)z;
  if (ev_n < MAX_EV)
    ev_t[ev_n++] = CB_T_NOW(z);
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

static unsigned t_before, c_before;

static void begin_step(z80 *z) {
  ev_n = 0;
  g_z = z;
  t_before = CUR_T(z);
  c_before = z->cyc;
}

static void end_step(const char *label, z80 *z) {
  unsigned dt = CUR_T(z) - t_before;
  unsigned dc = z->cyc - c_before;
  if (dt != dc) {
    printf("FAIL %s: bus plus internal T is %u, generic cost is %u\n", label, dt, dc);
    ++failures;
  }
#if CHECK_BUS_TIMES
  if (ev_n == 0) {
    printf("FAIL %s: no bus callbacks recorded\n", label);
    ++failures;
    return;
  }
  for (unsigned i = 0; i < ev_n; ++i) {
    if (ev_t[i] >= dt) {
      printf("FAIL %s: bus start T %u outside the step (total %u)\n", label, ev_t[i], dt);
      ++failures;
      break;
    }
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
  const uint8_t d4[1] = {0x04};
  static const struct { uint8_t op; int taken; int not_taken; } jrs[] = {
    {0x20, 0, 1}, /* nz: taken with zf clear */
    {0x28, 1, 0}, /* z: taken with zf set */
    {0x30, 0, 2}, /* nc: taken with cf clear */
    {0x38, 2, 0}, /* c: taken with cf set */
  };
  for (unsigned i = 0; i < 4; ++i) {
    char label[40];
    snprintf(label, sizeof(label), "jr %02X taken", jrs[i].op);
    cond_case(label, jrs[i].op, d4, 1, jrs[i].taken, 0x1006, 0xF000);
    snprintf(label, sizeof(label), "jr %02X not taken", jrs[i].op);
    cond_case(label, jrs[i].op, d4, 1, jrs[i].not_taken, 0x1002, 0xF000);
  }
  /* DJNZ *,d: taken while B - 1 is not 0. */
  {
    z80 z;
    set_base(&z);
    z.b = 2;
    uint8_t b[8] = {0x10, 0x04, 0, 0, 0, 0, 0, 0};
    load_bytes(b, 8);
    begin_step(&z);
    z80_step(&z);
    end_step("djnz taken", &z);
    check_pc("djnz taken", &z, 0x1006);
  }
  {
    z80 z;
    set_base(&z);
    z.b = 1;
    uint8_t b[8] = {0x10, 0x04, 0, 0, 0, 0, 0, 0};
    load_bytes(b, 8);
    begin_step(&z);
    z80_step(&z);
    end_step("djnz not taken", &z);
    check_pc("djnz not taken", &z, 0x1002);
  }
  /* JP/CALL/RET cc with nn = 0x2000. taken holds the flag bit for the
     odd conditions (z, c, pe, m) and clears it for the even ones
     (nz, nc, po, p). */
  const uint8_t nn[2] = {0x00, 0x20};
  static const struct { uint8_t jp, call, ret; int bit; int odd; } ccs[] = {
    {0xC2, 0xC4, 0xC0, 0, 0}, {0xCA, 0xCC, 0xC8, 0, 1},
    {0xD2, 0xD4, 0xD0, 1, 0}, {0xDA, 0xDC, 0xD8, 1, 1},
    {0xE2, 0xE4, 0xE0, 2, 0}, {0xEA, 0xEC, 0xE8, 2, 1},
    {0xF2, 0xF4, 0xF0, 3, 0}, {0xFA, 0xFC, 0xF8, 3, 1},
  };
  for (unsigned i = 0; i < 8; ++i) {
    char label[40];
    int hold = 1 << ccs[i].bit;
    int taken = ccs[i].odd ? hold : 0;
    int skipped = ccs[i].odd ? 0 : hold;
    snprintf(label, sizeof(label), "jp %02X taken", ccs[i].jp);
    cond_case(label, ccs[i].jp, nn, 2, taken, 0x2000, 0xF000);
    snprintf(label, sizeof(label), "jp %02X not taken", ccs[i].jp);
    cond_case(label, ccs[i].jp, nn, 2, skipped, 0x1003, 0xF000);
    snprintf(label, sizeof(label), "call %02X taken", ccs[i].call);
    cond_case(label, ccs[i].call, nn, 2, taken, 0x2000, 0xEFFE);
    snprintf(label, sizeof(label), "call %02X not taken", ccs[i].call);
    cond_case(label, ccs[i].call, nn, 2, skipped, 0x1003, 0xF000);
    /* RET pops 0x2000 off the stack when taken; the stack is set after
       the base setup wipes memory. */
    snprintf(label, sizeof(label), "ret %02X taken", ccs[i].ret);
    {
      z80 z;
      set_base(&z);
      uint8_t b[1] = {ccs[i].ret};
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
      if (z.sp != 0xF002) {
        printf("FAIL %s: sp is %04X, want F002\n", label, z.sp);
        ++failures;
      }
    }
    snprintf(label, sizeof(label), "ret %02X not taken", ccs[i].ret);
    cond_case(label, ccs[i].ret, 0, 0, skipped, 0x1001, 0xF000);
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
  static const struct { uint8_t op; int use_b; const char *name; } ops[] = {
    {0xB0, 0, "ldir"}, {0xB8, 0, "lddr"},
    {0xB1, 0, "cpir"}, {0xB9, 0, "cpdr"},
    {0xB2, 1, "inir"}, {0xBA, 1, "indr"},
    {0xB3, 1, "otir"}, {0xBB, 1, "otdr"},
  };
  for (unsigned i = 0; i < 8; ++i) {
    char label[40];
    snprintf(label, sizeof(label), "%s repeat", ops[i].name);
    block_case(label, ops[i].op, 1, ops[i].use_b);
    snprintf(label, sizeof(label), "%s end", ops[i].name);
    block_case(label, ops[i].op, 0, ops[i].use_b);
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
  ack_case("nmi", 0, 0x0066);
  ack_case("im0", 1, 0x1001);
  ack_case("im1", 2, 0x0038);
  ack_case("im2", 3, 0x5678);
}

int main(void) {
  sweep_base();
  sweep_cb();
  sweep_ed();
  sweep_ddfd(0xDD, "dd");
  sweep_ddfd(0xFD, "fd");
  cond_cases();
  block_cases();
  ack_cases();
  if (failures == 0)
    printf("tstate: all cases pass\n");
  else
    printf("tstate: %d failures\n", failures);
  return failures != 0;
}
