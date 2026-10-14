#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "z80.h"

// MARK: simple memory interface
#define MEMORY_SIZE 0x10000
static uint8_t* memory = NULL;
static bool test_finished = 0;

static uint8_t rb(void* userdata, uint16_t addr, z80_read_kind_t kind) {
  (void)kind; // cycle-count tests ignore the access kind
  (void)userdata;
  return memory[addr];
}

static void wb(void* userdata, uint16_t addr, uint8_t val) {
  memory[addr] = val;
}

static int load_file(const char* filename, uint16_t addr) {
  FILE* f = fopen(filename, "rb");
  if (f == NULL) {
    fprintf(stderr, "error: can't open file '%s'.\n", filename);
    return 1;
  }

  // file size check:
  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);
  rewind(f);

  if (file_size + addr >= MEMORY_SIZE) {
    fprintf(stderr, "error: file %s can't fit in memory.\n", filename);
    return 1;
  }

  // copying the bytes in memory:
  size_t result = fread(&memory[addr], sizeof(uint8_t), file_size, f);
  if (result != file_size) {
    fprintf(stderr, "error: while reading file '%s'\n", filename);
    return 1;
  }

  fclose(f);
  return 0;
}

static uint8_t in(z80* const z, uint16_t port) {
  uint8_t operation = z->c;

  // print a character stored in E
  if (operation == 2) {
    printf("%c", z->e);
  }
  // print from memory at (DE) until '$' char
  else if (operation == 9) {
    uint16_t addr = (z->d << 8) | z->e;
    do {
      printf("%c", rb(z->userdata, addr++, Z80_READ_DEBUG));
    } while (rb(z->userdata, addr, Z80_READ_DEBUG) != '$');
  }

  return 0xFF;
}

static void out(z80* const z, uint16_t port, uint8_t val) {
  test_finished = 1;
}

// MARK: block output port address
// OUTI/OUTD decrement B before the port is accessed, so the port address
// carries the decremented B. (Amstrad CPC compendium, "B is decremented
// before accessing the port"; memptr_eng.txt gives MEMPTR = BC after the
// decrement, plus or minus one.)
static uint16_t last_out_port = 0;
static uint8_t last_out_val = 0;
static int out_count = 0;

static void record_out(z80* const z, uint16_t port, uint8_t val) {
  (void)z;
  last_out_port = port;
  last_out_val = val;
  out_count += 1;
}

static int check_out_port(const char* name, uint8_t opcode, uint16_t bc,
    uint16_t expected_port, uint16_t expected_memptr) {
  z80 z;
  z80_init(&z);
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = in;
  z.port_out = record_out;
  memset(memory, 0, MEMORY_SIZE);
  memory[0x0000] = 0xED;
  memory[0x0001] = opcode;
  memory[0x4000] = 0x5A;
  z.pc = 0;
  z.h = 0x40;
  z.l = 0x00;
  z.b = (uint8_t)(bc >> 8);
  z.c = (uint8_t)(bc & 0xFF);
  out_count = 0;
  z80_step(&z);
  int failed = 0;
  if (out_count != 1 || last_out_port != expected_port ||
      last_out_val != 0x5A) {
    printf("*** ERROR %s: port %04X value %02X (expected %04X value 5A)\n",
        name, last_out_port, last_out_val, expected_port);
    failed = 1;
  }
  if (z.mem_ptr != expected_memptr) {
    printf("*** ERROR %s: MEMPTR %04X (expected %04X)\n", name, z.mem_ptr,
        expected_memptr);
    failed = 1;
  }
  return failed;
}

static int run_outi_port_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  // BC=BE01: the port is BD01 (the CPC's CRTC data port address).
  r += check_out_port("outi", 0xA3, 0xBE01, 0xBD01, 0xBD02);
  r += check_out_port("outd", 0xAB, 0xBE01, 0xBD01, 0xBD00);
  r += check_out_port("otir", 0xB3, 0xBE01, 0xBD01, 0xBD02);
  r += check_out_port("otdr", 0xBB, 0xBE01, 0xBD01, 0xBD00);
  printf("*** outi port test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: block input MEMPTR
// INI/IND set MEMPTR from BC as it was before B is decremented, plus or
// minus one (memptr_eng.txt). INIR/INDR do the same on each iteration.
static int check_in_memptr(const char* name, uint8_t opcode, uint16_t bc,
    uint16_t expected_memptr) {
  z80 z;
  z80_init(&z);
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = in;
  z.port_out = out;
  memset(memory, 0, MEMORY_SIZE);
  memory[0x0000] = 0xED;
  memory[0x0001] = opcode;
  z.pc = 0;
  z.h = 0x40;
  z.l = 0x00;
  z.b = (uint8_t)(bc >> 8);
  z.c = (uint8_t)(bc & 0xFF);
  z80_step(&z);
  if (z.mem_ptr != expected_memptr) {
    printf("*** ERROR %s: MEMPTR %04X (expected %04X)\n", name, z.mem_ptr,
        expected_memptr);
    return 1;
  }
  return 0;
}

static int run_ini_memptr_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  // BC=BE01: MEMPTR is BE01 + 1 and BE01 - 1, not the decremented BD01 + 1.
  r += check_in_memptr("ini", 0xA2, 0xBE01, 0xBE02);
  r += check_in_memptr("ind", 0xAA, 0xBE01, 0xBE00);
  // One iteration of the repeating forms (B is still non-zero afterwards).
  r += check_in_memptr("inir", 0xB2, 0xBE01, 0xBE02);
  r += check_in_memptr("indr", 0xBA, 0xBE01, 0xBE00);
  printf("*** ini memptr test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: interrupt after EI
// No interrupt is accepted at the boundary right after EI, only after the
// instruction that follows it (z80-documented section 5.5). A host that
// raises an interrupt between z80_step() calls and services it at once with
// z80_check_interrupt() must get the same answer at that boundary as
// z80_step()'s own trailing check did.
static void init_ei_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0xF000;
  z->interrupt_mode = 1;
}

static uint16_t pushed_return(const z80* const z) {
  return (uint16_t)(memory[z->sp] | (memory[(uint16_t)(z->sp + 1)] << 8));
}

static int check_ei_boundary(const char* name, bool ei_on_bus) {
  z80 z;
  init_ei_cpu(&z);
  if (ei_on_bus) {
    // IM 0 with EI on the bus: the interrupt is accepted after the NOP at
    // 0000, EI runs off the bus without moving pc, and pc stays at 0001.
    z.interrupt_mode = 0;
    z.iff1 = 1;
    z.iff2 = 1;
    z80_gen_int(&z, 0xFB);
    z80_step(&z);
    if (z.pc != 0x0001 || !z.iff1 || !z.iff2) {
      printf("*** ERROR %s: after EI on the bus pc %04X iff1 %d iff2 %d"
             " (expected 0001 1 1)\n", name, z.pc, z.iff1, z.iff2);
      return 1;
    }
    z.interrupt_mode = 1;
  } else {
    memory[0x0000] = 0xFB; // ei
    z80_step(&z);
    if (z.pc != 0x0001 || !z.iff1 || !z.iff2) {
      printf("*** ERROR %s: after EI pc %04X iff1 %d iff2 %d"
             " (expected 0001 1 1)\n", name, z.pc, z.iff1, z.iff2);
      return 1;
    }
  }
  const uint16_t next = z.pc;

  // Raised at EI's boundary and checked there again: not accepted.
  z80_gen_int(&z, 0xFF);
  z80_check_interrupt(&z);
  if (z.pc != next || !z.int_pending) {
    printf("*** ERROR %s: interrupt accepted right after EI (pc %04X)\n",
        name, z.pc);
    return 1;
  }
  // Accepted once the next instruction has run.
  z80_step(&z);
  if (z.pc != 0x0038 || pushed_return(&z) != (uint16_t)(next + 1)) {
    printf("*** ERROR %s: pc %04X return %04X (expected 0038 %04X)\n", name,
        z.pc, pushed_return(&z), (uint16_t)(next + 1));
    return 1;
  }
  return 0;
}

// The EI hold-off is for maskable interrupts only: NMI is recognised at the
// end of every instruction, whatever the interrupt flip-flops say
// (z80-documented section 2.5, NMI pin), so also right after EI.
static int check_nmi_after_ei(const char* name, bool raised_during_ei) {
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  if (raised_during_ei) {
    z80_gen_nmi(&z); // taken by z80_step()'s own check at EI's end
    z80_step(&z);
  } else {
    z80_step(&z);
    z80_gen_nmi(&z);
    z80_check_interrupt(&z);
  }
  if (z.pc != 0x0066 || z.nmi_pending || pushed_return(&z) != 0x0001) {
    printf("*** ERROR %s: pc %04X return %04X nmi_pending %d"
           " (expected 0066 0001 0)\n", name, z.pc, pushed_return(&z),
        z.nmi_pending);
    return 1;
  }
  return 0;
}

// The interrupt is still pending, not accepted: pc where the program left it.
static int expect_held(const char* name, const char* where, const z80* const z,
    uint16_t pc) {
  if (z->pc != pc || !z->int_pending) {
    printf("*** ERROR %s: %s pc %04X int_pending %d (expected %04X 1)\n",
        name, where, z->pc, z->int_pending, pc);
    return 1;
  }
  return 0;
}

// The interrupt was accepted in IM 1 with `ret` as its return address, and
// the acknowledge cleared both flip-flops.
static int expect_taken(const char* name, const z80* const z, uint16_t ret) {
  if (z->pc != 0x0038 || z->int_pending || pushed_return(z) != ret ||
      z->iff1 || z->iff2) {
    printf("*** ERROR %s: pc %04X return %04X int_pending %d iff1 %d iff2 %d"
           " (expected 0038 %04X 0 0 0)\n", name, z->pc, pushed_return(z),
        z->int_pending, z->iff1, z->iff2, ret);
    return 1;
  }
  return 0;
}

// A run of EIs holds the interrupt off through all of them; it is accepted
// after the first instruction that is not an EI (z80-documented section 5.5,
// "a large sequence of EI instructions").
static int check_ei_ei(void) {
  const char* name = "ei ; ei";
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0xFB; // ei
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  z80_check_interrupt(&z);
  if (expect_held(name, "after the first EI", &z, 0x0001)) {
    return 1;
  }
  z80_step(&z);
  z80_check_interrupt(&z);
  if (expect_held(name, "after the second EI", &z, 0x0002)) {
    return 1;
  }
  z80_step(&z); // nop
  return expect_taken(name, &z, 0x0003);
}

// DI right after EI clears IFF1 before the hold-off ends, so the interrupt
// is never accepted and stays pending.
static int check_ei_di(void) {
  const char* name = "ei ; di";
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0xF3; // di
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  z80_step(&z);
  z80_check_interrupt(&z);
  if (expect_held(name, "after DI", &z, 0x0002)) {
    return 1;
  }
  z80_step(&z); // nop
  if (expect_held(name, "after the NOP", &z, 0x0003)) {
    return 1;
  }
  if (z.iff1 || z.iff2) {
    printf("*** ERROR %s: iff1 %d iff2 %d (expected 0 0)\n", name, z.iff1,
        z.iff2);
    return 1;
  }
  return 0;
}

// EI ; HALT with an interrupt pending: accepted at HALT's own boundary, and
// the return address is the instruction after HALT (z80-documented section
// 5.4, "the PC is increased before the interrupt routine is called").
static int check_ei_halt(void) {
  const char* name = "ei ; halt";
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0x76; // halt
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  if (expect_held(name, "after EI", &z, 0x0001)) {
    return 1;
  }
  z80_step(&z);
  if (z.halted) {
    printf("*** ERROR %s: still halted after the acknowledge\n", name);
    return 1;
  }
  return expect_taken(name, &z, 0x0002);
}

// DD FB is one instruction that acts as EI: no interrupt between the prefix
// and FB (z80-documented section 5.5, "including the prefixed ones"), and the
// hold-off covers the instruction after it, as for a plain EI.
static int check_prefixed_ei(void) {
  const char* name = "dd fb";
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0xFB; // ei
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  z80_check_interrupt(&z);
  if (expect_held(name, "after DD FB", &z, 0x0002)) {
    return 1;
  }
  if (!z.iff1 || !z.iff2) {
    printf("*** ERROR %s: iff1 %d iff2 %d (expected 1 1)\n", name, z.iff1,
        z.iff2);
    return 1;
  }
  z80_step(&z); // nop
  return expect_taken(name, &z, 0x0003);
}

// RETN copies IFF2 into IFF1 with no hold-off: a pending interrupt is
// accepted at RETN's own boundary, with RETN's target as return address.
// With IFF2 clear it stays disabled and the interrupt stays pending.
static int check_retn_pending(const char* name, bool iff2) {
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xED;
  memory[0x0001] = 0x45; // retn
  memory[0xF000] = 0x34; // return address 1234
  memory[0xF001] = 0x12;
  z.iff1 = 0;
  z.iff2 = iff2;
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  if (iff2) {
    return expect_taken(name, &z, 0x1234);
  }
  return expect_held(name, "after RETN", &z, 0x1234);
}

static int run_ei_delay_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  r += check_ei_boundary("ei", false);
  r += check_ei_boundary("ei on the im 0 bus", true);
  r += check_nmi_after_ei("nmi raised during ei", true);
  r += check_nmi_after_ei("nmi raised after ei", false);
  r += check_ei_ei();
  r += check_ei_di();
  r += check_ei_halt();
  r += check_prefixed_ei();
  r += check_retn_pending("retn with int pending", true);
  r += check_retn_pending("retn with iff2 clear", false);
  printf("*** ei delay test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: RETI restores IFF1
// RETI copies IFF2 into IFF1 like RETN (z80-documented sections 3.4 and
// 5.3). It matters after an NMI taken inside a maskable handler: the NMI
// clears IFF1 only, and a RETI back out of that state must re-enable.
static int check_reti_iff(const char* name, uint8_t opcode) {
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xED;
  memory[0x0001] = opcode;
  memory[0xF000] = 0x34; // return address 1234
  memory[0xF001] = 0x12;
  z.iff1 = 0;
  z.iff2 = 1;
  z80_step(&z);
  if (z.pc != 0x1234 || !z.iff1 || !z.iff2) {
    printf("*** ERROR %s: pc %04X iff1 %d iff2 %d (expected 1234 1 1)\n",
        name, z.pc, z.iff1, z.iff2);
    return 1;
  }
  return 0;
}

static int run_reti_iff_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  r += check_reti_iff("reti", 0x4D);
  r += check_reti_iff("retn", 0x45);
  printf("*** reti iff test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: Q latch
// The core keeps an 8-bit Q latch behind SCF/CCF's undocumented X/Y
// flags: after an instruction that wrote the flags Q holds F, otherwise
// 0. A CB/ED/DD/FD prefix byte clears it (so DD/FD SCF/CCF read Q 0),
// as does an accepted NMI or maskable interrupt; POP AF and EX AF,AF'
// change F without latching it.
static void init_q_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0xF000;
}

static uint8_t pack_qf(const z80* const z) {
  return (uint8_t)((z->cf << 0) | (z->nf << 1) | (z->pf << 2) |
      (z->xf << 3) | (z->hf << 4) | (z->yf << 5) | (z->zf << 6) |
      (z->sf << 7));
}

static void seed_qf(z80* const z, uint8_t f) {
  z->cf = (f >> 0) & 1;
  z->nf = (f >> 1) & 1;
  z->pf = (f >> 2) & 1;
  z->xf = (f >> 3) & 1;
  z->hf = (f >> 4) & 1;
  z->yf = (f >> 5) & 1;
  z->zf = (f >> 6) & 1;
  z->sf = (f >> 7) & 1;
}

// z80_init() clears Q; a step must overwrite it, never inherit it, so
// every check below seeds Q with garbage first.
static int check_q_init(void) {
  const char* name = "init";
  z80 z;
  z80_init(&z);
  if (z.q != 0) {
    printf("*** ERROR %s: q %02X (expected 00)\n", name, z.q);
    return 1;
  }
  return 0;
}

// ADD A,n writes the flags, so Q latches F: 0xFF + 0x01 wraps to zero
// with Z, H and C set (F 0x51).
static int check_q_after_add(void) {
  const char* name = "add a,n";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xC6; // add a,n
  memory[0x0001] = 0x01;
  z.a = 0xFF;
  z.q = 0xA5;
  z80_step(&z);
  if (pack_qf(&z) != 0x51 || z.q != 0x51) {
    printf("*** ERROR %s: q %02X f %02X (expected 51 51)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// NOP writes no flags, so Q is 0 even with F left at 0xFF.
static int check_q_after_nop(void) {
  const char* name = "nop";
  z80 z;
  init_q_cpu(&z);
  seed_qf(&z, 0xFF);
  z.q = 0xA5;
  z80_step(&z);
  if (pack_qf(&z) != 0xFF || z.q != 0) {
    printf("*** ERROR %s: q %02X f %02X (expected 00 FF)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// POP AF changes F but must not latch it: Q stays 0.
static int check_q_after_pop_af(void) {
  const char* name = "pop af";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xF1; // pop af
  memory[0xF000] = 0x18; // F
  memory[0xF001] = 0x00; // A
  z.q = 0xA5;
  z80_step(&z);
  if (z.a != 0 || pack_qf(&z) != 0x18 || z.q != 0) {
    printf("*** ERROR %s: a %02X q %02X f %02X (expected 00 00 18)\n",
        name, z.a, z.q, pack_qf(&z));
    return 1;
  }
  return 0;
}

// EX AF,AF' swaps F but must not latch it: Q stays 0.
static int check_q_after_ex_af(void) {
  const char* name = "ex af,af'";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0x08; // ex af,af'
  z.a = 0x11;
  seed_qf(&z, 0x22);
  z.a_ = 0x33;
  z.f_ = 0x44;
  z.q = 0xA5;
  z80_step(&z);
  if (z.a != 0x33 || pack_qf(&z) != 0x44 || z.q != 0) {
    printf("*** ERROR %s: a %02X q %02X f %02X (expected 33 00 44)\n",
        name, z.a, z.q, pack_qf(&z));
    return 1;
  }
  return 0;
}

// LD A,I writes the flags (S/Z from A, P from IFF2, C preserved), so Q
// latches F: I 0x80 with IFF2 set and C seeded gives F 0x85.
static int check_q_after_ld_a_i(void) {
  const char* name = "ld a,i";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xED;
  memory[0x0001] = 0x57; // ld a,i
  z.i = 0x80;
  z.iff2 = 1;
  seed_qf(&z, 0xFF);
  z.q = 0xA5;
  z80_step(&z);
  if (z.a != 0x80 || pack_qf(&z) != 0x85 || z.q != 0x85) {
    printf("*** ERROR %s: a %02X q %02X f %02X (expected 80 85 85)\n",
        name, z.a, z.q, pack_qf(&z));
    return 1;
  }
  return 0;
}

// SCF after a flag writer reads Q == F: BIT 3,B leaves F 0x18, so
// (Q ^ F) | A is 0 and SCF's X/Y come out clear (F 0x01).
static int check_scf_after_bit(void) {
  const char* name = "bit 3,b ; scf";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xCB;
  memory[0x0001] = 0x58; // bit 3,b
  memory[0x0002] = 0x37; // scf
  z.a = 0;
  seed_qf(&z, 0);
  z.b = 0x08;
  z.q = 0xA5;
  z80_step(&z);
  z80_step(&z);
  if (pack_qf(&z) != 0x01 || z.q != 0x01) {
    printf("*** ERROR %s: q %02X f %02X (expected 01 01)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// SCF after LD B,0 reads Q 0 with the same F 0x18 before it, so
// (Q ^ F) | A keeps X set (F 0x09) -- unlike the BIT case above.
static int check_scf_after_ld(void) {
  const char* name = "ld b,0 ; scf";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0x06; // ld b,n
  memory[0x0001] = 0x00;
  memory[0x0002] = 0x37; // scf
  z.a = 0;
  seed_qf(&z, 0x18);
  z.q = 0xA5;
  z80_step(&z);
  z80_step(&z);
  if (pack_qf(&z) != 0x09 || z.q != 0x09) {
    printf("*** ERROR %s: q %02X f %02X (expected 09 09)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// POP AF then SCF behaves like the LD case, not the BIT case: the
// popped F 0x18 is not latched, so SCF still sees Q 0 (F 0x09).
static int check_scf_after_pop_af(void) {
  const char* name = "pop af ; scf";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xF1; // pop af
  memory[0x0001] = 0x37; // scf
  memory[0xF000] = 0x18; // F
  memory[0xF001] = 0x00; // A
  z.a = 0x99;
  z.q = 0xA5;
  z80_step(&z);
  if (z.q != 0) {
    printf("*** ERROR %s: q %02X after pop (expected 00)\n", name, z.q);
    return 1;
  }
  z80_step(&z);
  if (pack_qf(&z) != 0x09 || z.q != 0x09) {
    printf("*** ERROR %s: q %02X f %02X (expected 09 09)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// DD SCF reads Q 0: the prefix byte clears the seeded garbage, so with
// F 0x44 (Z+P) SCF's X/Y come out clear (F 0x45). An inherited Q 0xFF
// would give (Q ^ F) | A = 0xBB and F 0x6D instead.
static int check_dd_scf_clears_q(void) {
  const char* name = "dd 37";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0x37; // scf
  z.a = 0;
  seed_qf(&z, 0x44);
  z.q = 0xFF;
  z80_step(&z);
  if (pack_qf(&z) != 0x45 || z.q != 0x45) {
    printf("*** ERROR %s: q %02X f %02X (expected 45 45)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// CCF's formula with Q 0: H takes old C (0), C sets, and X/Y come from
// (Q ^ F) | A = 0x18, so X sets (F 0x09).
static int check_ccf_after_ld(void) {
  const char* name = "ld b,0 ; ccf";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0x06; // ld b,n
  memory[0x0001] = 0x00;
  memory[0x0002] = 0x3F; // ccf
  z.a = 0;
  seed_qf(&z, 0x18);
  z.q = 0xA5;
  z80_step(&z);
  z80_step(&z);
  if (pack_qf(&z) != 0x09 || z.q != 0x09) {
    printf("*** ERROR %s: q %02X f %02X (expected 09 09)\n", name, z.q,
        pack_qf(&z));
    return 1;
  }
  return 0;
}

// Accepting a maskable interrupt drops the running instruction's flag
// state even when that instruction wrote the flags (ADD leaves F
// 0x51): Q is 0 afterwards.
static int check_q_after_int(void) {
  const char* name = "add a,n with int accepted";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xC6; // add a,n
  memory[0x0001] = 0x01;
  z.a = 0xFF;
  z.interrupt_mode = 1;
  z.iff1 = 1;
  z.iff2 = 1;
  z.q = 0xA5;
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  int r = expect_taken(name, &z, 0x0002);
  if (z.q != 0) {
    printf("*** ERROR %s: q %02X (expected 00)\n", name, z.q);
    return 1;
  }
  return r;
}

// Same for NMI: the ADD's flags don't survive into Q.
static int check_q_after_nmi(void) {
  const char* name = "add a,n with nmi accepted";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0xC6; // add a,n
  memory[0x0001] = 0x01;
  z.a = 0xFF;
  z.q = 0xA5;
  z80_gen_nmi(&z);
  z80_step(&z);
  if (z.pc != 0x0066 || z.nmi_pending || pushed_return(&z) != 0x0002 ||
      z.q != 0) {
    printf("*** ERROR %s: pc %04X return %04X q %02X"
        " (expected 0066 0002 00)\n", name, z.pc, pushed_return(&z), z.q);
    return 1;
  }
  return 0;
}

// HALT writes no flags: Q is 0 after it.
static int check_q_after_halt(void) {
  const char* name = "halt";
  z80 z;
  init_q_cpu(&z);
  memory[0x0000] = 0x76; // halt
  z.q = 0xA5;
  z80_step(&z);
  if (!z.halted || z.q != 0) {
    printf("*** ERROR %s: halted %d q %02X (expected 1 00)\n", name,
        z.halted, z.q);
    return 1;
  }
  return 0;
}

static int run_qlatch_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  r += check_q_init();
  r += check_q_after_add();
  r += check_q_after_nop();
  r += check_q_after_pop_af();
  r += check_q_after_ex_af();
  r += check_q_after_ld_a_i();
  r += check_scf_after_bit();
  r += check_scf_after_ld();
  r += check_scf_after_pop_af();
  r += check_dd_scf_clears_q();
  r += check_ccf_after_ld();
  r += check_q_after_int();
  r += check_q_after_nmi();
  r += check_q_after_halt();
  printf("*** q latch test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: test runner
static int run_test(
    z80* const z, const char* filename, unsigned long cyc_expected) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  memset(memory, 0, MEMORY_SIZE);

  if (load_file(filename, 0x100) != 0) {
    return 1;
  }
  printf("*** TEST: %s\n", filename);

  z->pc = 0x100;

  // inject "out 1,a" at 0x0000 (signal to stop the test)
  memory[0x0000] = 0xD3;
  memory[0x0001] = 0x00;

  // inject "in a,0" at 0x0005 (signal to output some characters)
  memory[0x0005] = 0xDB;
  memory[0x0006] = 0x00;
  memory[0x0007] = 0xC9;

  long nb_instructions = 0;

  test_finished = 0;
  while (!test_finished) {
    nb_instructions += 1;

    // warning: the following line will output dozens of GB of data.
    // z80_debug_output(z);

    z80_step(z);
  }

  long long diff = cyc_expected - z->cyc;
  printf("\n*** %lu instructions executed on %lu cycles"
         " (expected=%lu, diff=%lld)\n\n",
      nb_instructions, z->cyc, cyc_expected, diff);

  return cyc_expected != z->cyc;
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "outiport") == 0) {
    return run_outi_port_test();
  }
  if (argc > 1 && strcmp(argv[1], "inimemptr") == 0) {
    return run_ini_memptr_test();
  }
  if (argc > 1 && strcmp(argv[1], "eidelay") == 0) {
    return run_ei_delay_test();
  }
  if (argc > 1 && strcmp(argv[1], "retiiff") == 0) {
    return run_reti_iff_test();
  }
  if (argc > 1 && strcmp(argv[1], "qlatch") == 0) {
    return run_qlatch_test();
  }

  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }

  z80 cpu;

  // the following cycle counts have been retrieved from z80emu
  // (https://github.com/anotherlin/z80emu) for those exact roms
  //
  // An optional argv[1] runs just one of the three (they're independent,
  // so a test runner can schedule them as separate parallel processes);
  // with no argument, all three run sequentially as upstream's ./z80_tests did.
  const char* which = argc > 1 ? argv[1] : NULL;
  int r = 0;
  if (!which || strcmp(which, "prelim") == 0)
    r += run_test(&cpu, "roms/prelim.com", 8721LU);
  if (!which || strcmp(which, "zexdoc") == 0)
    r += run_test(&cpu, "roms/zexdoc.cim", 46734978649LU);
  if (!which || strcmp(which, "zexall") == 0)
    r += run_test(&cpu, "roms/zexall.cim", 46734978649LU);

  free(memory);
  if (!which) {
    r += run_outi_port_test();
    r += run_ini_memptr_test();
    r += run_ei_delay_test();
    r += run_reti_iff_test();
    r += run_qlatch_test();
  }
  return r != 0;
}
