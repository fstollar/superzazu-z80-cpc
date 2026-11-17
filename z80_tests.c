// fork()/alarm()/waitpid() for the no-free-phase WAIT test below: the POSIX
// declarations stay hidden under strict C99 without the feature macro, so it
// comes before every system include.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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
// decrement, plus or minus one.) OTIR/OTDR do the same on their last
// iteration; an iteration that repeats sets MEMPTR to PC + 1 instead.
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
  // The ED prefix is its own step; run to the end of the instruction.
  do {
    z80_step(&z);
  } while (z.pending_prefix != Z80_PREFIX_NONE);
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
  // One repeating iteration of OTIR/OTDR (B is still non-zero afterwards):
  // MEMPTR is PC + 1 of the rewound instruction.
  r += check_out_port("otir", 0xB3, 0xBE01, 0xBD01, 0x0001);
  r += check_out_port("otdr", 0xBB, 0xBE01, 0xBD01, 0x0001);
  // The last iteration (B=1 becomes 0) behaves as OUTI/OUTD.
  r += check_out_port("otir last", 0xB3, 0x0101, 0x0001, 0x0002);
  r += check_out_port("otdr last", 0xBB, 0x0101, 0x0001, 0x0000);
  printf("*** outi port test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: block input MEMPTR
// INI/IND set MEMPTR from BC as it was before B is decremented, plus or
// minus one (memptr_eng.txt). INIR/INDR do the same on their last
// iteration; an iteration that repeats sets MEMPTR to PC + 1 instead.
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
  // The ED prefix is its own step; run to the end of the instruction.
  do {
    z80_step(&z);
  } while (z.pending_prefix != Z80_PREFIX_NONE);
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
  // One repeating iteration (B is still non-zero afterwards): PC + 1.
  r += check_in_memptr("inir", 0xB2, 0xBE01, 0x0001);
  r += check_in_memptr("indr", 0xBA, 0xBE01, 0x0001);
  // The last iteration (B=1 becomes 0) behaves as INI/IND.
  r += check_in_memptr("inir last", 0xB2, 0x0101, 0x0102);
  r += check_in_memptr("indr last", 0xBA, 0x0101, 0x0100);
  printf("*** ini memptr test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: MEMPTR of single instructions
// Runs one instruction at address 0 and checks MEMPTR afterwards. MEMPTR
// starts at 1234 so that instructions which leave it alone, or only step
// it by one, can be told apart. memptr_eng.txt describes each rule.
static int check_memptr(const char* name, const uint8_t* code, int len,
    uint8_t a, uint16_t bc, uint16_t de, uint16_t hl, uint16_t ix,
    uint8_t at_hl, uint16_t expected_memptr) {
  z80 z;
  z80_init(&z);
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = in;
  z.port_out = out;
  memset(memory, 0, MEMORY_SIZE);
  memcpy(memory, code, (size_t)len);
  memory[hl] = at_hl;
  z.pc = 0;
  z.a = a;
  z.b = (uint8_t)(bc >> 8);
  z.c = (uint8_t)(bc & 0xFF);
  z.d = (uint8_t)(de >> 8);
  z.e = (uint8_t)(de & 0xFF);
  z.h = (uint8_t)(hl >> 8);
  z.l = (uint8_t)(hl & 0xFF);
  z.ix = ix;
  z.mem_ptr = 0x1234;
  // Prefix bytes are their own steps; run to the end of the instruction.
  // (Repeat cases rewind the pc and finish in the same opcode step, so the
  // loop ends with pending NONE there too.)
  do {
    z80_step(&z);
  } while (z.pending_prefix != Z80_PREFIX_NONE);
  if (z.mem_ptr != expected_memptr) {
    printf("*** ERROR %s: MEMPTR %04X (expected %04X)\n", name, z.mem_ptr,
        expected_memptr);
    return 1;
  }
  return 0;
}

static int run_memptr_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  static const uint8_t cpir[] = {0xED, 0xB1};
  static const uint8_t cpdr[] = {0xED, 0xB9};
  static const uint8_t ldir[] = {0xED, 0xB0};
  static const uint8_t lddr[] = {0xED, 0xB8};
  static const uint8_t in_a_n[] = {0xDB, 0xFF};
  static const uint8_t in_a_fe[] = {0xDB, 0xFE};
  static const uint8_t jr[] = {0x18, 0x05};
  static const uint8_t jp_ix[] = {0xDD, 0xE9};
  static const uint8_t ld_nn_ix[] = {0xDD, 0x22, 0x00, 0x60};
  static const uint8_t ld_ix_nn[] = {0xDD, 0x2A, 0x00, 0x60};
  static const uint8_t out_c_b[] = {0xED, 0x41};
  static const uint8_t in_b_c[] = {0xED, 0x40};
  int r = 0;
  // CPIR/CPDR: a repeating iteration sets PC + 1; the last one (BC reaches
  // 0, or A matches) behaves as CPI/CPD, MEMPTR + 1 or - 1.
  r += check_memptr("cpir repeat", cpir, 2, 0x01, 0x0002, 0, 0x4000, 0, 0x00,
      0x0001);
  r += check_memptr("cpir last bc", cpir, 2, 0x01, 0x0001, 0, 0x4000, 0, 0x00,
      0x1235);
  r += check_memptr("cpir last match", cpir, 2, 0x01, 0x0005, 0, 0x4000, 0,
      0x01, 0x1235);
  r += check_memptr("cpdr repeat", cpdr, 2, 0x01, 0x0002, 0, 0x4000, 0, 0x00,
      0x0001);
  r += check_memptr("cpdr last bc", cpdr, 2, 0x01, 0x0001, 0, 0x4000, 0, 0x00,
      0x1233);
  r += check_memptr("cpdr last match", cpdr, 2, 0x01, 0x0005, 0, 0x4000, 0,
      0x01, 0x1233);
  // LDIR/LDDR: PC + 1 on a repeating iteration, unchanged on the last one.
  r += check_memptr("ldir repeat", ldir, 2, 0, 0x0002, 0x5000, 0x4000, 0, 0,
      0x0001);
  r += check_memptr("ldir last", ldir, 2, 0, 0x0001, 0x5000, 0x4000, 0, 0,
      0x1234);
  r += check_memptr("lddr last", lddr, 2, 0, 0x0001, 0x5000, 0x4000, 0, 0,
      0x1234);
  // IN A,(n): (A << 8 | n) + 1, A taken before the read; n=FF carries.
  // The input value (FF from in()) must not be used: n=FE tells them apart.
  r += check_memptr("in a,(n)", in_a_n, 2, 0x12, 0, 0, 0x4000, 0, 0, 0x1300);
  r += check_memptr("in a,(fe)", in_a_fe, 2, 0x12, 0, 0, 0x4000, 0, 0,
      0x12FF);
  // JR e: the jump target.
  r += check_memptr("jr", jr, 2, 0, 0, 0, 0x4000, 0, 0, 0x0007);
  // JP (IX) leaves MEMPTR alone.
  r += check_memptr("jp (ix)", jp_ix, 2, 0, 0, 0, 0x4000, 0x5678, 0, 0x1234);
  // LD (nn),IX and LD IX,(nn): nn + 1.
  r += check_memptr("ld (nn),ix", ld_nn_ix, 4, 0, 0, 0, 0x4000, 0, 0, 0x6001);
  r += check_memptr("ld ix,(nn)", ld_ix_nn, 4, 0, 0, 0, 0x4000, 0, 0, 0x6001);
  // OUT (C),r and IN r,(C): BC + 1, with BC taken before IN B writes B.
  r += check_memptr("out (c),b", out_c_b, 2, 0, 0x12FF, 0, 0x4000, 0, 0,
      0x1300);
  r += check_memptr("in b,(c)", in_b_c, 2, 0, 0x12FF, 0, 0x4000, 0, 0, 0x1300);
  printf("*** memptr test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: block-instruction repeat X/Y flags
// pack_qf/seed_qf live with the Q-latch suite further down; declared here
// so this suite can seed F and read it back as a byte.
static uint8_t pack_qf(const z80* const z);
static void seed_qf(z80* const z, uint8_t f);
// Runs one step of a block instruction at `addr` and checks the full F
// byte. On a repeating iteration (BC - 1 != 0, and no match for CPIR/CPDR)
// X/Y come from the rewound PC: bit 11 feeds X, bit 13 feeds Y, where the
// PC points back at the ED byte. The last iteration (BC reaches 0, or a
// match) keeps the LDI/CPI formula. Everything besides X/Y is what
// ldi()/cpi() compute: S/Z/C keep their seeded value, H and N are cleared
// (LDI) or come from the subtraction (CPI), PV is set while BC is nonzero
// afterwards. F is seeded with 0 so an untouched flag reads back 0.
static int check_blockxy(const char* name, const uint8_t* code, uint16_t addr,
    uint8_t a, uint16_t bc, uint16_t de, uint16_t hl,
    uint8_t at_hl, uint16_t expected_pc, uint8_t expected_f) {
  z80 z;
  z80_init(&z);
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = in;
  z.port_out = out;
  memset(memory, 0, MEMORY_SIZE);
  memcpy(&memory[addr], code, 2);
  memory[hl] = at_hl;
  z.pc = addr;
  z.a = a;
  z.b = (uint8_t)(bc >> 8);
  z.c = (uint8_t)(bc & 0xFF);
  z.d = (uint8_t)(de >> 8);
  z.e = (uint8_t)(de & 0xFF);
  z.h = (uint8_t)(hl >> 8);
  z.l = (uint8_t)(hl & 0xFF);
  seed_qf(&z, 0);
  // Prefix bytes are their own steps; run to the end of the instruction.
  // (A repeat rewinds the pc and finishes in the same opcode step, so the
  // loop ends with pending NONE there too.)
  do {
    z80_step(&z);
  } while (z.pending_prefix != Z80_PREFIX_NONE);
  int failed = 0;
  if (z.pc != expected_pc) {
    printf("*** ERROR %s: PC %04X (expected %04X)\n", name, z.pc,
        expected_pc);
    failed = 1;
  }
  const uint8_t f = pack_qf(&z);
  if (f != expected_f) {
    printf("*** ERROR %s: F %02X (expected %02X)\n", name, f, expected_f);
    failed = 1;
  }
  return failed;
}

static int run_blockxy_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  static const uint8_t ldir[] = {0xED, 0xB0};
  static const uint8_t lddr[] = {0xED, 0xB8};
  static const uint8_t cpir[] = {0xED, 0xB1};
  static const uint8_t cpdr[] = {0xED, 0xB9};
  int r = 0;
  // LDIR repeats: the LDI formula gives X = bit 3 and Y = bit 1 of
  // (HL) + A, which the PC rule overrides. A = (HL) = FF makes the
  // formula read 1/1, A = (HL) = 00 makes it read 0/0.
  r += check_blockxy("ldir repeat xy00", ldir, 0x0000, 0xFF, 2, 0x5000,
      0x4000, 0xFF, 0x0000, 0x04);
  r += check_blockxy("ldir repeat xy10", ldir, 0x0800, 0x00, 2, 0x5000,
      0x4000, 0x00, 0x0800, 0x0C);
  r += check_blockxy("ldir repeat xy01", ldir, 0x2000, 0x00, 2, 0x5000,
      0x4000, 0x00, 0x2000, 0x24);
  r += check_blockxy("ldir repeat xy11", ldir, 0x2800, 0x00, 2, 0x5000,
      0x4000, 0x00, 0x2800, 0x2C);
  // LDIR last (BC reaches 0): the LDI formula stands. A = (HL) = 00 gives
  // formula X/Y = 0/0 while the PC rule at 0x2800 would give 1/1.
  r += check_blockxy("ldir last", ldir, 0x2800, 0x00, 1, 0x5000, 0x4000,
      0x00, 0x2802, 0x00);
  // LDDR repeat overrides the same way; LDDR last keeps the formula. The
  // 0x0000 repeat uses A = (HL) = FF so the formula reads 1/1 against the
  // PC rule's 0/0.
  r += check_blockxy("lddr repeat xy11", lddr, 0x2800, 0x00, 2, 0x5000,
      0x4000, 0x00, 0x2800, 0x2C);
  r += check_blockxy("lddr repeat xy00", lddr, 0x0000, 0xFF, 2, 0x5000,
      0x4000, 0xFF, 0x0000, 0x04);
  r += check_blockxy("lddr last", lddr, 0x0000, 0xFF, 1, 0x5000, 0x4000,
      0xFF, 0x0002, 0x28);
  // CPIR repeats: A = 01 over (HL) = 00 misses with formula X/Y = 0/0;
  // A = 00 over (HL) = F7 misses with formula X/Y = 1/0 (the subtraction
  // half-borrows, and X/Y read result - H). N, PV and the half-borrow stay
  // set, the rest is 0.
  r += check_blockxy("cpir repeat xy11", cpir, 0x2800, 0x01, 2, 0, 0x4000,
      0x00, 0x2800, 0x2E);
  r += check_blockxy("cpir repeat xy00", cpir, 0x0000, 0x00, 2, 0, 0x4000,
      0xF7, 0x0000, 0x16);
  // CPIR last by BC or by match: the CPI formula stands (here 0/0), PV
  // stays set on a match while BC is still nonzero afterwards.
  r += check_blockxy("cpir last bc", cpir, 0x2800, 0x01, 1, 0, 0x4000,
      0x00, 0x2802, 0x02);
  r += check_blockxy("cpir last match", cpir, 0x0000, 0x05, 2, 0, 0x4000,
      0x05, 0x0002, 0x46);
  // The same match at 0x2800: the formula still gives X/Y 0/0, while
  // reading bits 11/13 of the PC on this last iteration would give 1/1.
  r += check_blockxy("cpir last match 2800", cpir, 0x2800, 0x05, 2, 0, 0x4000,
      0x05, 0x2802, 0x46);
  // CPDR repeat overrides the same way; CPDR last by BC or match keeps
  // the formula. The 0x0000 repeat uses A = 00 over (HL) = F7 so the
  // formula reads 1/0 against the PC rule's 0/0; the last-bc case uses
  // A = 08 over (HL) = 00 so the formula reads 1/0 (F 0x0A) against the
  // PC rule's 0/0 (F 0x02). The last-match case at 0x0000 cannot differ:
  // a match forces result 0 with no half-borrow, so the formula is 0/0
  // exactly like the PC rule there.
  r += check_blockxy("cpdr repeat xy11", cpdr, 0x2800, 0x01, 2, 0, 0x4000,
      0x00, 0x2800, 0x2E);
  r += check_blockxy("cpdr repeat xy00", cpdr, 0x0000, 0x00, 2, 0, 0x4000,
      0xF7, 0x0000, 0x16);
  r += check_blockxy("cpdr last bc", cpdr, 0x0000, 0x08, 1, 0, 0x4000,
      0x00, 0x0002, 0x0A);
  r += check_blockxy("cpdr last match", cpdr, 0x2800, 0x05, 2, 0, 0x4000,
      0x05, 0x2802, 0x46);
  // At 0x07FE the pre-rewind PC is 0x0800, so X/Y from PC+2 would read 1/0.
  r += check_blockxy("ldir repeat 07fe", ldir, 0x07FE, 0x00, 2, 0x5000,
      0x4000, 0x00, 0x07FE, 0x04);
  // At 0x27FF MEMPTR lands on 0x2800, so X/Y from PC+1 (MEMPTR) would
  // read 1/1.
  r += check_blockxy("ldir repeat 27ff", ldir, 0x27FF, 0x00, 2, 0x5000,
      0x4000, 0x00, 0x27FF, 0x24);
  printf("*** block x/y test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: I/O block instruction flags
// IN r,(C) reports X/Y from the value read (bits 3 and 5). INI/IND/OUTI/
// OUTD set S/Z/X/Y from the decremented B, N from bit 7 of the port or
// memory byte, H and C when a 9-bit sum k overflows past 255, and PV from
// the parity of (k & 7) xor the decremented B, where k pairs the data byte
// with the neighbouring register: L after the HL step for the OUT pair,
// C + 1 for INI, C - 1 for IND. A repeating INIR/INDR/OTIR/OTDR iteration
// instead reports X/Y from bits 11/13 of the rewound PC and adjusts H/PV
// for the rewind (the extra end-of-instruction states the Z80Decoder notes
// describe for interrupted block instructions); the last iteration, where
// B reaches 0, keeps the plain single-step result. Every expected F byte
// below is the final state of the named SingleStepTests vector, replayed
// register-for-register (initial B/C/HL, the byte at HL, the port value).
static uint8_t ioflags_port_val = 0;
static int ioflags_port_count = 0;

static uint8_t ioflags_in(z80* const z, uint16_t port) {
  (void)z;
  (void)port;
  ioflags_port_count += 1;
  return ioflags_port_val;
}

static void ioflags_out(z80* const z, uint16_t port, uint8_t val) {
  (void)z;
  (void)port;
  (void)val;
  ioflags_port_count += 1;
}

// Runs one I/O block step at `addr` and checks the full F byte, the PC,
// and that exactly one port access happened. The data byte comes from the
// port for the IN pair (ioflags_port_val) and from memory at HL for the
// OUT pair (at_hl).
static int check_ioflag(const char* name, const uint8_t* code, uint16_t addr,
    uint8_t b, uint8_t c, uint16_t hl, uint8_t at_hl, uint8_t port_val,
    uint16_t expected_pc, uint8_t expected_f) {
  z80 z;
  z80_init(&z);
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = ioflags_in;
  z.port_out = ioflags_out;
  memset(memory, 0, MEMORY_SIZE);
  memcpy(&memory[addr], code, 2);
  memory[hl] = at_hl;
  z.pc = addr;
  z.b = b;
  z.c = c;
  z.h = (uint8_t)(hl >> 8);
  z.l = (uint8_t)(hl & 0xFF);
  ioflags_port_val = port_val;
  ioflags_port_count = 0;
  seed_qf(&z, 0);
  // Prefix bytes are their own steps; run to the end of the instruction.
  // (A repeat rewinds the pc and finishes in the same opcode step, so the
  // loop ends with pending NONE there too.)
  do {
    z80_step(&z);
  } while (z.pending_prefix != Z80_PREFIX_NONE);
  int failed = 0;
  if (z.pc != expected_pc) {
    printf("*** ERROR %s: PC %04X (expected %04X)\n", name, z.pc,
        expected_pc);
    failed = 1;
  }
  const uint8_t f = pack_qf(&z);
  if (f != expected_f) {
    printf("*** ERROR %s: F %02X (expected %02X)\n", name, f, expected_f);
    failed = 1;
  }
  if (ioflags_port_count != 1) {
    printf("*** ERROR %s: %d port accesses (expected 1)\n", name,
        ioflags_port_count);
    failed = 1;
  }
  return failed;
}

static int run_ioflags_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  static const uint8_t in_b_c[] = {0xED, 0x40};
  static const uint8_t ini[] = {0xED, 0xA2};
  static const uint8_t inir[] = {0xED, 0xB2};
  static const uint8_t ind[] = {0xED, 0xAA};
  static const uint8_t outi[] = {0xED, 0xA3};
  static const uint8_t otir[] = {0xED, 0xB3};
  static const uint8_t outd[] = {0xED, 0xAB};
  static const uint8_t otdr[] = {0xED, 0xBB};
  int r = 0;
  // IN B,(C): X/Y read bits 3/5 of the port value (here 239 = EF). The
  // M5 mutant (no X/Y) reports 0/0 and reads F A0.
  r += check_ioflag("in b,(c) xy", in_b_c, 0xB139, 181, 218, 0x6AEE, 0,
      239, 0xB13B, 0xA8); // SingleStepTests "ED 40 0002"
  // INI: k = data + (C + 1); H/C set past 255, PV from (k & 7) ^ B.
  r += check_ioflag("ini k over", ini, 0x5CB7, 64, 237, 0x72D5, 0, 117,
      0x5CB9, 0x3D); // SingleStepTests "ED A2 0001", k = 355
  r += check_ioflag("ini k within", ini, 0xD3A9, 102, 98, 0x550F, 0, 91,
      0xD3AB, 0x24); // SingleStepTests "ED A2 0000", k = 190
  // INI with data + C == 255: the M2 mutant (C instead of C + 1 in k)
  // misses the carry and reads H/C 0/0 (F 87).
  r += check_ioflag("ini c plus one", ini, 0x8315, 131, 17, 0xB73D, 0, 238,
      0x8317, 0x97); // SingleStepTests "ED A2 0342", k = 256
  // IND: k = data + (C - 1).
  r += check_ioflag("ind k over", ind, 0x0251, 41, 156, 0xF2B0, 0, 198,
      0x0253, 0x3B); // SingleStepTests "ED AA 0003", k = 353
  r += check_ioflag("ind k within", ind, 0x90BE, 250, 136, 0xAAB0, 0, 41,
      0x90C0, 0xAC); // SingleStepTests "ED AA 0000", k = 176
  // OUTI: k = data + L after the HL increment.
  r += check_ioflag("outi k over", outi, 0xCA85, 216, 10, 0x0590, 242, 0,
      0xCA87, 0x97); // SingleStepTests "ED A3 0000", k = 387
  r += check_ioflag("outi k within", outi, 0x9301, 56, 143, 0xD340, 156, 0,
      0x9303, 0x22); // SingleStepTests "ED A3 0001", k = 221
  // OUTI with data + L == 255 before the increment: the M1 mutant (the
  // old L in k) misses the carry and reads H/C 0/0 (F 07).
  r += check_ioflag("outi incremented l", outi, 0xDEFF, 121, 199, 0xF151,
      174, 0, 0xDF01, 0x3F); // SingleStepTests "ED A3 00E6", k = 256
  // OUTD: k = data + L after the HL decrement.
  r += check_ioflag("outd k over", outd, 0xF722, 221, 88, 0xC9FB, 192, 0,
      0xF724, 0x9F); // SingleStepTests "ED AB 0001", k = 442
  r += check_ioflag("outd k within", outd, 0xBA1C, 217, 147, 0xDE11, 208, 0,
      0xBA1E, 0x8E); // SingleStepTests "ED AB 0000", k = 224
  // INIR repeats: X/Y from bits 11/13 of the rewound PC, H/PV adjusted
  // for the rewind, in every carry/data-bit-7 combination. The M3 mutant
  // (no H/PV change) keeps the plain single-step H/PV here.
  r += check_ioflag("inir repeat carry data80", inir, 0x5F7E, 255, 216,
      0xA02C, 0, 172, 0x5F7E, 0x8B); // SingleStepTests "ED B2 0007"
  r += check_ioflag("inir repeat carry dataclear", inir, 0x7BD7, 109, 157,
      0x13B2, 0, 118, 0x7BD7, 0x29); // SingleStepTests "ED B2 0000"
  r += check_ioflag("inir repeat nocarry data80", inir, 0x361D, 50, 55,
      0x42E9, 0, 154, 0x361D, 0x22); // SingleStepTests "ED B2 0001"
  r += check_ioflag("inir repeat nocarry dataclear", inir, 0x8B51, 213, 22,
      0x4A2E, 0, 26, 0x8B51, 0x8C); // SingleStepTests "ED B2 0003"
  // At 0x07FF the pre-rewind PC is 0x0801, so X/Y from the next PC instead
  // of the rewound one (the M4 mutant) would read 1/0.
  r += check_ioflag("inir repeat next-pc xy", inir, 0x07FF, 143, 41, 0x50E6,
      0, 86, 0x07FF, 0x84); // SingleStepTests "ED B2 0370"
  // INIR last (B reaches 0): the plain INI result stands.
  r += check_ioflag("inir last", inir, 0xB829, 1, 216, 0x3336, 0, 198,
      0xB82B, 0x53); // SingleStepTests "ED B2 004B"
  // OTDR repeats: the same four combinations for the OUT pair.
  r += check_ioflag("otdr repeat carry data80", otdr, 0xDD5F, 225, 160,
      0x125C, 213, 0, 0xDD5F, 0x9F); // SingleStepTests "ED BB 0000"
  r += check_ioflag("otdr repeat carry dataclear", otdr, 0x7D6C, 252, 106,
      0xD6AA, 125, 0, 0x7D6C, 0xAD); // SingleStepTests "ED BB 0003"
  r += check_ioflag("otdr repeat nocarry data80", otdr, 0x52BF, 207, 184,
      0xF013, 162, 0, 0x52BF, 0x86); // SingleStepTests "ED BB 0001"
  r += check_ioflag("otdr repeat nocarry dataclear", otdr, 0x0BE5, 20, 165,
      0xD9AE, 39, 0, 0x0BE5, 0x0C); // SingleStepTests "ED BB 0004"
  // At 0x6FFE the pre-rewind PC is 0x7000, so X/Y from the next PC (the
  // M4 mutant) would read 0/1 instead of 1/1.
  r += check_ioflag("otdr repeat next-pc xy", otdr, 0x6FFE, 137, 186,
      0x2BAB, 133, 0, 0x6FFE, 0xAF); // SingleStepTests "ED BB 019A"
  // OTDR last (B reaches 0): the plain OUTD result stands. The corpus
  // holds no OTDR vector with B reaching 0, so this replays "ED AB 0037"
  // (an OUTD vector with initial B 1) under the OTDR opcode: outd() runs
  // once and the repeat branch is not taken.
  r += check_ioflag("otdr last", otdr, 0x9213, 1, 121, 0xBCD8, 40, 0,
      0x9215, 0x40);
  printf("*** io block flags test: %s\n", r == 0 ? "OK" : "FAILED");
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

// DD FB acts as EI, one prefix step at a time: no interrupt between the
// prefix and FB (z80-documented section 5.5, "including the prefixed ones"),
// and the hold-off covers the instruction after it, as for a plain EI.
static int check_prefixed_ei(void) {
  const char* name = "dd fb";
  z80 z;
  init_ei_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0xFB; // ei
  z80_gen_int(&z, 0xFF);
  z80_step(&z); // DD prefix step: the interrupt stays held
  z80_check_interrupt(&z);
  if (expect_held(name, "after the DD step", &z, 0x0001)) {
    return 1;
  }
  z80_step(&z); // FB step: EI sets the hold-off
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
  z80_step(&z); // ED prefix step
  z80_gen_int(&z, 0xFF);
  z80_step(&z); // RETN itself
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
  z80_step(&z); // ED prefix step
  z80_step(&z); // RETI/RETN itself
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

// MARK: stray DD/FD prefix and R
// Every prefix byte is its own step with its own R increment, so a stray
// DD/FD prefix keeps its increment even though the next prefix drops it:
// LD A,R behind one stray prefix reads R0 + 3 (DD, ED, ED op) and behind
// two R0 + 4; LD R,A leaves R = A in both cases (z80-documented
// section 6.1).
static void init_stray_r_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  z80_set_timing(z, &z80_timing_cpc);
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0xF000;
  z->r = 0x10;
  z->a = 0x50;
}

static int check_ld_a_r_after_stray(const char* name, int strays,
    uint8_t exp_a, uint8_t exp_r) {
  z80 z;
  init_stray_r_cpu(&z);
  for (int i = 0; i < strays; i++) {
    memory[i] = 0xDD;
  }
  memory[strays] = 0xED;
  memory[strays + 1] = 0x5F; // ld a,r
  for (int i = 0; i < strays + 2; i++) {
    z80_step(&z);
  }
  if (z.a != exp_a || z.r != exp_r || z.pc != (uint16_t)(strays + 2)) {
    printf("*** ERROR %s: a %02X r %02X pc %04X (expected %02X %02X %04X)\n",
        name, z.a, z.r, z.pc, exp_a, exp_r, strays + 2);
    return 1;
  }
  return 0;
}

static int check_ld_r_a_after_stray(const char* name, int strays) {
  z80 z;
  init_stray_r_cpu(&z);
  for (int i = 0; i < strays; i++) {
    memory[i] = 0xDD;
  }
  memory[strays] = 0xED;
  memory[strays + 1] = 0x4F; // ld r,a
  for (int i = 0; i < strays + 2; i++) {
    z80_step(&z);
  }
  if (z.r != 0x50 || z.a != 0x50 || z.pc != (uint16_t)(strays + 2)) {
    printf("*** ERROR %s: a %02X r %02X pc %04X (expected 50 50 %04X)\n",
        name, z.a, z.r, z.pc, strays + 2);
    return 1;
  }
  return 0;
}

static int run_stray_r_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  r += check_ld_a_r_after_stray("dd ed 5f", 1, 0x13, 0x13);
  r += check_ld_a_r_after_stray("dd dd ed 5f", 2, 0x14, 0x14);
  r += check_ld_r_a_after_stray("dd ed 4f", 1);
  r += check_ld_r_a_after_stray("dd dd ed 4f", 2);
  printf("*** stray prefix R test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: long DD/FD prefix chains
// A run of DD/FD prefixes runs one step per byte: each prefix is its own
// 1 µs fetch with its own R increment, and every step between the first
// prefix and the opcode carries the pending prefix with no interrupt
// accepted in between. All cycle counts below use the CPC table so the µs
// numbers read directly.
static void init_chain_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  z80_set_timing(z, &z80_timing_cpc);
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0xF000;
}

// 1000 mixed DD/FD prefixes and LD IX/IY,nn run one step per byte: pc past
// the whole chain, the index register of the last prefix, one µs per prefix
// plus the 3 µs of the LD step (its 4 µs table charge minus the prefix µs),
// one R increment per fetch, MEMPTR untouched.
static int check_long_chain(void) {
  const char* name = "1000 mixed prefixes";
  z80 z;
  init_chain_cpu(&z);
  for (int i = 0; i < 1000; i++) {
    memory[i] = (i & 1) ? 0xFD : 0xDD; // the last prefix is FD
  }
  memory[1000] = 0x21; // ld iz,**
  memory[1001] = 0x34;
  memory[1002] = 0x12;
  z.r = 0xFF;
  const unsigned long cyc0 = z.cyc;
  int steps = 0;
  do {
    z80_step(&z);
    steps += 1;
  } while (z.pending_prefix != Z80_PREFIX_NONE && steps < 2000);
  if (steps != 1001 || z.pc != 1003 || z.iy != 0x1234 || z.ix != 0 ||
      z.cyc - cyc0 != 1003 || z.r != 0xE8 || z.mem_ptr != 0) {
    printf("*** ERROR %s: steps %d pc %04X ix %04X iy %04X cyc %lu r %02X"
        " memptr %04X (expected 1001 03EB 0000 1234 +1003 E8 0000)\n",
        name, steps, z.pc, z.ix, z.iy, z.cyc - cyc0, z.r, z.mem_ptr);
    return 1;
  }
  return 0;
}

// IM 0 follow-on bytes after a prefix arrive over the acknowledge bus with
// pc frozen: each bus byte is its own step, with no wrap bound. The first
// step runs the NOP at 0 (pc to 1), then the acknowledge takes the first
// prefix byte off int_data; each follow-on step fetches one byte over INTA
// until LD iz,nn ends the instruction.
static unsigned im0_intas;
static int im0_prefixes_left;
static int im0_reads;
static int im0_acks;

static uint8_t im0_chain_bus(z80* const bus_z, uint16_t bus_pc,
    z80_im0_cycle_t cycle) {
  (void)bus_z;
  (void)bus_pc;
  if (cycle == Z80_IM0_INTA) {
    im0_intas++;
    if (im0_prefixes_left > 0) {
      im0_prefixes_left--;
      return 0xDD;
    }
    return 0x21; // ld iz,**
  }
  im0_reads++;
  if (im0_reads == 1) {
    return 0x34;
  }
  if (im0_reads == 2) {
    return 0x12;
  }
  return 0x00;
}

static void im0_count_ack(z80* const ack_z) {
  (void)ack_z;
  im0_acks++;
}

static int check_im0_chain(void) {
  const char* name = "im0 prefix chain";
  z80 z;
  init_chain_cpu(&z);
  z.interrupt_mode = 0;
  z.iff1 = 1;
  z.iff2 = 1;
  z.im0_bus = im0_chain_bus;
  z.int_ack = im0_count_ack;
  im0_intas = 0;
  im0_prefixes_left = 999; // with int_data below: 1000 prefixes
  im0_reads = 0;
  im0_acks = 0;
  z.r = 0x10;
  z80_gen_int(&z, 0xDD); // first prefix byte
  const unsigned long cyc0 = z.cyc;
  int steps = 0;
  do {
    z80_step(&z);
    steps += 1;
  } while (z.pending_prefix != Z80_PREFIX_NONE && steps < 2000);
  // R: seed 10h + 1 (the NOP) + 1 (the acknowledge prefix) + 999 (bus
  // prefixes) + 1 (LD iz,nn). cyc: 3 for the first step (NOP + acknowledge
  // + prefix), 2 per bus prefix step (INTA fetch + prefix), 4 for the last
  // (INTA fetch + LD iz,nn minus the prefix µs).
  if (steps != 1001 || z.ix != 0x1234 || z.iy != 0 || z.r != 0x7A ||
      im0_intas != 1000 || im0_acks != 1 || z.pc != 1 ||
      z.cyc - cyc0 != 2005 || z.im0_active != 0 || z.int_pending) {
    printf("*** ERROR %s: steps %d ix %04X iy %04X r %02X intas %u acks %d"
        " pc %04X cyc %lu active %d pending %d"
        " (expected 1001 1234 0000 7A 1000 1 0001 +2005 0 0)\n",
        name, steps, z.ix, z.iy, z.r, im0_intas, im0_acks, z.pc,
        z.cyc - cyc0, z.im0_active, z.int_pending);
    return 1;
  }
  return 0;
}

static int run_chain_long_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  const int r = check_long_chain();
  printf("*** long prefix chain test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

static int run_chain_im0_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  const int r = check_im0_chain();
  printf("*** im0 prefix chain test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: every prefix is its own step
// Each CB/ED/DD/FD prefix byte is one z80_step(): one M1 fetch, R + 1 and
// 1 us on the CPC table. The next step decodes the following byte by the
// pending prefix. No INT or NMI is accepted after a prefix; EI holds off
// INT only (NMI still goes through). All cycle counts below use the CPC
// table so the us numbers read directly.
static void init_prefix_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  z80_set_timing(z, &z80_timing_cpc);
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0xF000;
  z->r = 0x10;
}

// No interrupt after a prefix exactly when a prefix is pending.
static int check_accept_matches_prefix(const char* name, const char* where,
    const z80* const z) {
  const int pending = z->pending_prefix != Z80_PREFIX_NONE;
  const int none = z->int_accept == Z80_ACCEPT_NONE;
  if (pending != none) {
    printf("*** ERROR %s %s: pending %02X accept %u"
        " (NONE exactly while a prefix is pending)\n",
        name, where, z->pending_prefix, z->int_accept);
    return 1;
  }
  return 0;
}

// T1, one prefix: DD 21 34 12 costs 1 + 3 us, R + 1 per step.
static int check_prefix_single(void) {
  const char* name = "single DD prefix";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0x21; // ld ix,**
  memory[0x0002] = 0x34;
  memory[0x0003] = 0x12;
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_DD ||
      z.int_accept != Z80_ACCEPT_NONE || z.cyc != 1 || z.r != 0x11 ||
      z.ix != 0) {
    printf("*** ERROR %s after the prefix: pc %04X pending %02X accept %u"
        " cyc %lu r %02X ix %04X (expected 0001 DD NONE 1 11 0000)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r, z.ix);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the prefix", &z);
  z80_step(&z);
  if (z.pc != 4 || z.pending_prefix != Z80_PREFIX_NONE ||
      z.int_accept != Z80_ACCEPT_ALL || z.cyc != 4 || z.r != 0x12 ||
      z.ix != 0x1234) {
    printf("*** ERROR %s after the opcode: pc %04X pending %02X accept %u"
        " cyc %lu r %02X ix %04X (expected 0004 NONE ALL 4 12 1234)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r, z.ix);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the opcode", &z);
  return r;
}

// T2, chain: DD FD DD 21 34 12. The last prefix wins, so IX is loaded.
static int check_prefix_chain(void) {
  const char* name = "DD FD DD chain";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0xFD;
  memory[0x0002] = 0xDD;
  memory[0x0003] = 0x21; // ld iz,**
  memory[0x0004] = 0x34;
  memory[0x0005] = 0x12;
  static const uint8_t want_prefix[3] = {
    Z80_PREFIX_DD, Z80_PREFIX_FD, Z80_PREFIX_DD
  };
  int r = 0;
  for (int step = 0; step < 3; step++) {
    z80_step(&z);
    if (z.pc != (uint16_t)(step + 1) ||
        z.pending_prefix != want_prefix[step] ||
        z.int_accept != Z80_ACCEPT_NONE || z.cyc != (unsigned long)(step + 1) ||
        z.r != (uint8_t)(0x11 + step)) {
      printf("*** ERROR %s step %d: pc %04X pending %02X accept %u cyc %lu"
          " r %02X (expected %04X %02X NONE %d %02X)\n", name, step + 1, z.pc,
          z.pending_prefix, z.int_accept, z.cyc, z.r, step + 1,
          want_prefix[step], step + 1, 0x11 + step);
      r = 1;
    }
    r += check_accept_matches_prefix(name, "on a prefix step", &z);
  }
  z80_step(&z);
  if (z.pc != 6 || z.pending_prefix != Z80_PREFIX_NONE ||
      z.int_accept != Z80_ACCEPT_ALL || z.cyc != 6 || z.r != 0x14 ||
      z.ix != 0x1234 || z.iy != 0) {
    printf("*** ERROR %s after the opcode: pc %04X pending %02X accept %u"
        " cyc %lu r %02X ix %04X iy %04X"
        " (expected 0006 NONE ALL 6 14 1234 0000)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r, z.ix, z.iy);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the opcode", &z);
  return r;
}

// T3, CB and ED: CB 47 costs 1 + 1; LDIR with BC = 2 costs 1 + 5 with PC
// back, then 1 + 4.
static int check_prefix_cb(void) {
  const char* name = "CB prefix";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xCB;
  memory[0x0001] = 0x47; // bit 0,a
  z.a = 0x01;
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_CB ||
      z.int_accept != Z80_ACCEPT_NONE || z.cyc != 1 || z.r != 0x11) {
    printf("*** ERROR %s after the prefix: pc %04X pending %02X accept %u"
        " cyc %lu r %02X (expected 0001 CB NONE 1 11)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the prefix", &z);
  z80_step(&z);
  if (z.pc != 2 || z.pending_prefix != Z80_PREFIX_NONE ||
      z.int_accept != Z80_ACCEPT_ALL || z.cyc != 2 || z.r != 0x12 ||
      z.zf != 0) {
    printf("*** ERROR %s after the opcode: pc %04X pending %02X accept %u"
        " cyc %lu r %02X zf %d (expected 0002 NONE ALL 2 12 0)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r, z.zf);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the opcode", &z);
  return r;
}

static int check_prefix_ldir(void) {
  const char* name = "ED LDIR";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xED;
  memory[0x0001] = 0xB0; // ldir
  memory[0x4000] = 0xAA;
  memory[0x4001] = 0xAA; // the second pass copies this one to 0x5001
  z.b = 0;
  z.c = 2;
  z.h = 0x40;
  z.l = 0x00;
  z.d = 0x50;
  z.e = 0x00;
  int r = 0;
  z80_step(&z); // ED prefix
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_ED ||
      z.int_accept != Z80_ACCEPT_NONE || z.cyc != 1 || z.r != 0x11) {
    printf("*** ERROR %s after the prefix: pc %04X pending %02X accept %u"
        " cyc %lu r %02X (expected 0001 ED NONE 1 11)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the prefix", &z);
  z80_step(&z); // first pass: BC 2 -> 1, PC back
  if (z.pc != 0 || z.pending_prefix != Z80_PREFIX_NONE ||
      z.int_accept != Z80_ACCEPT_ALL || z.cyc != 6 || z.r != 0x12 ||
      ((uint16_t)(z.b << 8) | z.c) != 1) {
    printf("*** ERROR %s first pass: pc %04X pending %02X accept %u cyc %lu"
        " r %02X bc %02X%02X (expected 0000 NONE ALL 6 12 0001)\n",
        name, z.pc, z.pending_prefix, z.int_accept, z.cyc, z.r, z.b, z.c);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the first pass", &z);
  z80_step(&z); // ED prefix again
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_ED || z.cyc != 7 ||
      z.r != 0x13) {
    printf("*** ERROR %s second prefix: pc %04X pending %02X cyc %lu r %02X"
        " (expected 0001 ED 7 13)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the second prefix", &z);
  z80_step(&z); // last pass: BC 1 -> 0, PC past
  if (z.pc != 2 || z.pending_prefix != Z80_PREFIX_NONE || z.cyc != 11 ||
      z.r != 0x14 || z.b != 0 || z.c != 0 || memory[0x5000] != 0xAA ||
      memory[0x5001] != 0xAA) {
    printf("*** ERROR %s last pass: pc %04X pending %02X cyc %lu r %02X"
        " bc %02X%02X (expected 0002 NONE 11 14 0000)\n",
        name, z.pc, z.pending_prefix, z.cyc, z.r, z.b, z.c);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the last pass", &z);
  return r;
}

// T4, DDCB: DD CB 05 46 costs 1, 1, 4 with R + 1, + 1, + 0. The third step
// reads d and op as operands and the (IX+d) byte as data, with no M1.
// The T counter pins the two internal T between the op and the data
// access: 4 + 4 + 12 = 20, so dropping them reads 18 with cyc unchanged.
static z80_read_kind_t ddcb_kinds[8];
static int ddcb_kind_count;

static uint8_t ddcb_kind_rb(void* userdata, uint16_t addr,
    z80_read_kind_t kind) {
  (void)userdata;
  if (ddcb_kind_count < 8) {
    ddcb_kinds[ddcb_kind_count++] = kind;
  }
  return memory[addr];
}

static int check_prefix_ddcb(void) {
  const char* name = "DDCB step";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0xCB;
  memory[0x0002] = 0x05; // d
  memory[0x0003] = 0x46; // bit 0,(ix+5)
  z.ix = 0x4000;
  memory[0x4005] = 0x81;
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_DD || z.cyc != 1 ||
      z.t_counter != 4 || z.r != 0x11) {
    printf("*** ERROR %s step 1: pc %04X pending %02X cyc %lu t %lu r %02X"
        " (expected 0001 DD 1 4 11)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.t_counter, z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 1", &z);
  z80_step(&z);
  if (z.pc != 2 || z.pending_prefix != Z80_PREFIX_DDCB || z.cyc != 2 ||
      z.t_counter != 8 || z.r != 0x12) {
    printf("*** ERROR %s step 2: pc %04X pending %02X cyc %lu t %lu r %02X"
        " (expected 0002 DDCB 2 8 12)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.t_counter, z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 2", &z);
  z.read_byte = ddcb_kind_rb;
  ddcb_kind_count = 0;
  z80_step(&z);
  if (z.pc != 4 || z.pending_prefix != Z80_PREFIX_NONE || z.cyc != 6 ||
      z.t_counter != 20 || z.r != 0x12 || z.zf != 0) {
    printf("*** ERROR %s step 3: pc %04X pending %02X cyc %lu t %lu r %02X"
        " zf %d (expected 0004 NONE 6 20 12 0)\n", name, z.pc,
        z.pending_prefix, z.cyc, z.t_counter, z.r, z.zf);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 3", &z);
  if (ddcb_kind_count != 3 || ddcb_kinds[0] != Z80_READ_OPERAND ||
      ddcb_kinds[1] != Z80_READ_OPERAND || ddcb_kinds[2] != Z80_READ_DATA) {
    printf("*** ERROR %s step 3: %d reads (expected OPERAND OPERAND DATA)\n",
        name, ddcb_kind_count);
    r = 1;
  }
  for (int i = 0; i < ddcb_kind_count; i++) {
    if (ddcb_kinds[i] == Z80_READ_M1) {
      printf("*** ERROR %s step 3: read %d is an M1 fetch\n", name, i);
      r = 1;
    }
  }
  return r;
}

// T4b, FDCB: FD CB 05 46 mirrors the DDCB step on IY -- 1, 1, 4 us with
// R + 1, + 1, + 0, and the same two internal T, so 4 + 4 + 12 = 20 T.
static int check_prefix_fdcb(void) {
  const char* name = "FDCB step";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xFD;
  memory[0x0001] = 0xCB;
  memory[0x0002] = 0x05; // d
  memory[0x0003] = 0x46; // bit 0,(iy+5)
  z.iy = 0x4000;
  memory[0x4005] = 0x81;
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_FD || z.cyc != 1 ||
      z.t_counter != 4 || z.r != 0x11) {
    printf("*** ERROR %s step 1: pc %04X pending %02X cyc %lu t %lu r %02X"
        " (expected 0001 FD 1 4 11)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.t_counter, z.r);
    r = 1;
  }
  z80_step(&z);
  if (z.pc != 2 || z.pending_prefix != Z80_PREFIX_FDCB || z.cyc != 2 ||
      z.t_counter != 8 || z.r != 0x12) {
    printf("*** ERROR %s step 2: pc %04X pending %02X cyc %lu t %lu r %02X"
        " (expected 0002 FDCB 2 8 12)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.t_counter, z.r);
    r = 1;
  }
  z80_step(&z);
  if (z.pc != 4 || z.pending_prefix != Z80_PREFIX_NONE || z.cyc != 6 ||
      z.t_counter != 20 || z.r != 0x12 || z.zf != 0) {
    printf("*** ERROR %s step 3: pc %04X pending %02X cyc %lu t %lu r %02X"
        " zf %d (expected 0004 NONE 6 20 12 0)\n", name, z.pc,
        z.pending_prefix, z.cyc, z.t_counter, z.r, z.zf);
    r = 1;
  }
  return r;
}

// T5, DD ED: DD ED 6A runs ADC HL,HL on HL, leaving IX alone.
static int check_prefix_dd_ed(void) {
  const char* name = "DD ED ADC";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0xED;
  memory[0x0002] = 0x6A; // adc hl,hl
  z.h = 0x10;
  z.l = 0x00;
  z.ix = 0x4000;
  z.cf = 0;
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_DD || z.cyc != 1) {
    printf("*** ERROR %s step 1: pc %04X pending %02X cyc %lu"
        " (expected 0001 DD 1)\n", name, z.pc, z.pending_prefix, z.cyc);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 1", &z);
  z80_step(&z);
  if (z.pc != 2 || z.pending_prefix != Z80_PREFIX_ED || z.cyc != 2 ||
      ((uint16_t)(z.h << 8) | z.l) != 0x1000 || z.ix != 0x4000) {
    printf("*** ERROR %s step 2: pc %04X pending %02X cyc %lu hl %02X%02X"
        " ix %04X (expected 0002 ED 2 1000 4000)\n", name, z.pc,
        z.pending_prefix, z.cyc, z.h, z.l, z.ix);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 2", &z);
  z80_step(&z);
  if (z.pc != 3 || z.pending_prefix != Z80_PREFIX_NONE || z.cyc != 5 ||
      ((uint16_t)(z.h << 8) | z.l) != 0x2000 || z.ix != 0x4000) {
    printf("*** ERROR %s step 3: pc %04X pending %02X cyc %lu hl %02X%02X"
        " ix %04X (expected 0003 NONE 5 2000 4000)\n", name, z.pc,
        z.pending_prefix, z.cyc, z.h, z.l, z.ix);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 3", &z);
  return r;
}

// T6, ED DD: two 1 us steps, PC + 2, R + 2. The second byte runs as an
// invalid ED op with no effect, so no prefix stays pending afterwards.
static int check_prefix_ed_dd(void) {
  const char* name = "ED DD";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xED;
  memory[0x0001] = 0xDD;
  z.a = 0x5A;
  z.mem_ptr = 0x1234;
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_ED || z.cyc != 1 ||
      z.r != 0x11) {
    printf("*** ERROR %s step 1: pc %04X pending %02X cyc %lu r %02X"
        " (expected 0001 ED 1 11)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 1", &z);
  z80_step(&z);
  if (z.pc != 2 || z.pending_prefix != Z80_PREFIX_NONE || z.cyc != 2 ||
      z.r != 0x12 || z.a != 0x5A || z.mem_ptr != 0x1234) {
    printf("*** ERROR %s step 2: pc %04X pending %02X cyc %lu r %02X"
        " (expected 0002 NONE 2 12)\n", name, z.pc, z.pending_prefix, z.cyc,
        z.r);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "step 2", &z);
  return r;
}

// T7, INT: with I set before DD 21 nn nn, nothing is taken after the
// prefix; it is taken after the opcode, stacking 0x0004.
static int check_prefix_int(void) {
  const char* name = "INT across a prefix";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0x21; // ld ix,**
  memory[0x0002] = 0x34;
  memory[0x0003] = 0x12;
  z.interrupt_mode = 1;
  z.iff1 = 1;
  z.iff2 = 1;
  z80_gen_int(&z, 0xFF);
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || !z.int_pending || z.sp != 0xF000) {
    printf("*** ERROR %s after the prefix: pc %04X int %d sp %04X"
        " (expected 0001 1 F000)\n", name, z.pc, z.int_pending, z.sp);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the prefix", &z);
  z80_step(&z);
  r += expect_taken(name, &z, 0x0004);
  r += check_accept_matches_prefix(name, "after the acknowledge", &z);
  return r;
}

// T8, NMI: the same across a prefix, stacking 0x0004 at 0x66.
static int check_prefix_nmi(void) {
  const char* name = "NMI across a prefix";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0x21; // ld ix,**
  memory[0x0002] = 0x34;
  memory[0x0003] = 0x12;
  z80_gen_nmi(&z);
  int r = 0;
  z80_step(&z);
  if (z.pc != 1 || !z.nmi_pending || z.sp != 0xF000) {
    printf("*** ERROR %s after the prefix: pc %04X nmi %d sp %04X"
        " (expected 0001 1 F000)\n", name, z.pc, z.nmi_pending, z.sp);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the prefix", &z);
  z80_step(&z);
  if (z.pc != 0x0066 || z.nmi_pending || pushed_return(&z) != 0x0004) {
    printf("*** ERROR %s after the opcode: pc %04X return %04X nmi %d"
        " (expected 0066 0004 0)\n", name, z.pc, pushed_return(&z),
        z.nmi_pending);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the acknowledge", &z);
  return r;
}

// T9, EI: INT waits for the instruction after EI, even across prefixes;
// NMI goes through right after EI.
static void init_prefix_ei_cpu(z80* const z) {
  init_prefix_cpu(z);
  z->interrupt_mode = 1;
  z->iff1 = 1;
  z->iff2 = 1;
}

static int check_prefix_ei_nop(void) {
  const char* name = "EI NOP with INT";
  z80 z;
  init_prefix_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0x00; // nop
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  int r = 0;
  if (z.pc != 1 || z.int_accept != Z80_ACCEPT_NMI_ONLY || !z.int_pending) {
    printf("*** ERROR %s after EI: pc %04X accept %u int %d"
        " (expected 0001 NMI_ONLY 1)\n", name, z.pc, z.int_accept,
        z.int_pending);
    r = 1;
  }
  z80_step(&z);
  r += expect_taken(name, &z, 0x0002);
  return r;
}

static int check_prefix_ei_ei_nop(void) {
  const char* name = "EI EI NOP with INT";
  z80 z;
  init_prefix_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0xFB; // ei
  memory[0x0002] = 0x00; // nop
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  z80_step(&z);
  int r = 0;
  if (z.pc != 2 || z.int_accept != Z80_ACCEPT_NMI_ONLY || !z.int_pending) {
    printf("*** ERROR %s after two EIs: pc %04X accept %u int %d"
        " (expected 0002 NMI_ONLY 1)\n", name, z.pc, z.int_accept,
        z.int_pending);
    r = 1;
  }
  z80_step(&z);
  r += expect_taken(name, &z, 0x0003);
  return r;
}

static int check_prefix_ei_dd_op(void) {
  const char* name = "EI DD LD IX with INT";
  z80 z;
  init_prefix_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0xDD;
  memory[0x0002] = 0x21; // ld ix,**
  memory[0x0003] = 0x34;
  memory[0x0004] = 0x12;
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  z80_step(&z);
  int r = 0;
  if (z.pc != 2 || !z.int_pending || z.ix != 0) {
    printf("*** ERROR %s after the prefix: pc %04X int %d ix %04X"
        " (expected 0002 1 0000)\n", name, z.pc, z.int_pending, z.ix);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the prefix", &z);
  z80_step(&z);
  if (z.ix != 0x1234) {
    printf("*** ERROR %s after the opcode: ix %04X (expected 1234)\n",
        name, z.ix);
    r = 1;
  }
  r += expect_taken(name, &z, 0x0005);
  return r;
}

static int check_prefix_ei_nmi(void) {
  const char* name = "EI NOP with NMI";
  z80 z;
  init_prefix_ei_cpu(&z);
  memory[0x0000] = 0xFB; // ei
  memory[0x0001] = 0x00; // nop
  z80_gen_nmi(&z);
  z80_step(&z);
  int r = 0;
  if (z.pc != 0x0066 || z.nmi_pending || pushed_return(&z) != 0x0001) {
    printf("*** ERROR %s: pc %04X return %04X nmi %d (expected 0066 0001 0)\n",
        name, z.pc, pushed_return(&z), z.nmi_pending);
    r = 1;
  }
  return r;
}

// T10, IM0 prefix: the bus gives DD, then 0x21 over INTA, then the
// operands at the frozen PC. The acknowledge step leaves DD with the IM0
// run still open; the next step loads IX and leaves PC frozen.
static int prefix_im0_intas;
static int prefix_im0_reads;

static uint8_t prefix_im0_bus(z80* const bus_z, uint16_t bus_pc,
    z80_im0_cycle_t cycle) {
  (void)bus_z;
  (void)bus_pc;
  if (cycle == Z80_IM0_INTA) {
    prefix_im0_intas++;
    return 0x21; // ld iz,** after the DD prefix
  }
  prefix_im0_reads++;
  return prefix_im0_reads == 1 ? 0x34 : 0x12;
}

static int check_prefix_im0_once(bool derived) {
  const char* name = derived ? "IM0 prefix derived" : "IM0 prefix";
  z80 z;
  init_prefix_cpu(&z);
  z.interrupt_mode = 0;
  z.iff1 = 1;
  z.iff2 = 1;
  z.im0_bus = prefix_im0_bus;
  prefix_im0_intas = 0;
  prefix_im0_reads = 0;
  if (derived) {
    z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
    z.periodic_wait_mask = 0xD;
  }
  z80_gen_int(&z, 0xDD); // the prefix byte on the bus
  z80_step(&z);
  int r = 0;
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_DD || !z.im0_active ||
      z.cyc != 3) {
    printf("*** ERROR %s after the acknowledge: pc %04X pending %02X"
        " active %d cyc %lu (expected 0001 DD 1 3)\n",
        name, z.pc, z.pending_prefix, z.im0_active, z.cyc);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the acknowledge", &z);
  z80_step(&z);
  if (z.pc != 1 || z.pending_prefix != Z80_PREFIX_NONE || z.im0_active ||
      z.cyc != 7 || z.ix != 0x1234 || prefix_im0_intas != 1 ||
      prefix_im0_reads != 2) {
    printf("*** ERROR %s after the opcode: pc %04X pending %02X active %d"
        " cyc %lu ix %04X intas %d reads %d"
        " (expected 0001 NONE 0 7 1234 1 2)\n",
        name, z.pc, z.pending_prefix, z.im0_active, z.cyc, z.ix,
        prefix_im0_intas, prefix_im0_reads);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the opcode", &z);
  return r;
}

static int check_prefix_im0(void) {
  int r = 0;
  r += check_prefix_im0_once(false);
  r += check_prefix_im0_once(true);
  return r;
}

// T11, invariant: accept is NONE exactly while a prefix is pending,
// across a DDCB run and an EI+NMI run.
static int check_prefix_invariant(void) {
  const char* name = "accept invariant";
  z80 z;
  init_prefix_cpu(&z);
  memory[0x0000] = 0xDD;
  memory[0x0001] = 0xCB;
  memory[0x0002] = 0x05;
  memory[0x0003] = 0x46; // bit 0,(ix+5)
  memory[0x0004] = 0xFB; // ei
  z.ix = 0x4000;
  memory[0x4005] = 0x81;
  int r = 0;
  for (int step = 0; step < 3; step++) {
    z80_step(&z);
    r += check_accept_matches_prefix(name, "on a DDCB step", &z);
  }
  if (z.pc != 4 || z.pending_prefix != Z80_PREFIX_NONE ||
      z.int_accept != Z80_ACCEPT_ALL) {
    printf("*** ERROR %s after DDCB: pc %04X pending %02X accept %u"
        " (expected 0004 NONE ALL)\n", name, z.pc, z.pending_prefix,
        z.int_accept);
    r = 1;
  }
  z80_gen_nmi(&z);
  z80_step(&z); // EI with NMI: taken at once
  if (z.pc != 0x0066 || z.int_accept != Z80_ACCEPT_ALL) {
    printf("*** ERROR %s after EI+NMI: pc %04X accept %u"
        " (expected 0066 ALL)\n", name, z.pc, z.int_accept);
    r = 1;
  }
  r += check_accept_matches_prefix(name, "after the acknowledge", &z);
  return r;
}

static int run_prefix_step_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  r += check_prefix_single();
  r += check_prefix_chain();
  r += check_prefix_cb();
  r += check_prefix_ldir();
  r += check_prefix_ddcb();
  r += check_prefix_fdcb();
  r += check_prefix_dd_ed();
  r += check_prefix_ed_dd();
  r += check_prefix_int();
  r += check_prefix_nmi();
  r += check_prefix_ei_nop();
  r += check_prefix_ei_ei_nop();
  r += check_prefix_ei_dd_op();
  r += check_prefix_ei_nmi();
  r += check_prefix_im0();
  r += check_prefix_invariant();
  printf("*** per-prefix step test: %s\n", r == 0 ? "OK" : "FAILED");
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
  z80_step(&z); // ED prefix
  z80_step(&z); // ld a,i
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
  z80_step(&z); // CB prefix
  z80_step(&z); // bit 3,b
  z80_step(&z); // scf
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
  z80_step(&z); // DD prefix step: clears Q
  z80_step(&z); // SCF itself
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

// MARK: stack write order
// A push puts the high byte on the bus first: the write to the higher
// address runs a full cycle ahead of the write to the lower one. CALL,
// RST and the interrupt acknowledge push the return address the same way,
// as do the two writes of ex (sp),hl/iz. Plain ld (**),rr stores keep
// their low-byte-first order and are not covered here.
static uint16_t write_log[8];
static int write_count = 0;

static void record_write(void* userdata, uint16_t addr, uint8_t val) {
  (void)userdata;
  memory[addr] = val;
  if (write_count < 8) {
    write_log[write_count++] = addr;
  }
}

static void init_push_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = record_write;
  z->port_in = in;
  z->port_out = out;
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0x1000;
  write_count = 0;
}

static int check_push_order(const char* name, uint16_t hi, uint16_t lo) {
  if (write_count != 2 || write_log[0] != hi || write_log[1] != lo) {
    printf("*** ERROR %s: writes", name);
    for (int i = 0; i < write_count; ++i) {
      printf(" %04X", write_log[i]);
    }
    printf(" (expected %04X %04X)\n", hi, lo);
    return 1;
  }
  return 0;
}

static int check_pushed(const char* name, const z80* const z,
    uint16_t expected) {
  if (pushed_return(z) != expected) {
    printf("*** ERROR %s: pushed %04X (expected %04X)\n", name,
        pushed_return(z), expected);
    return 1;
  }
  return 0;
}

static int run_push_order_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  z80 z;

  // PUSH BC stores B at SP - 1 ahead of C at SP - 2.
  init_push_cpu(&z);
  memory[0x0000] = 0xC5; // push bc
  z.b = 0x12;
  z.c = 0x34;
  z80_step(&z);
  r += check_push_order("push bc", 0x0FFF, 0x0FFE);
  r += check_pushed("push bc", &z, 0x1234);

  // CALL pushes the return address high byte first, then jumps.
  init_push_cpu(&z);
  memory[0x0000] = 0xCD; // call 1234
  memory[0x0001] = 0x34;
  memory[0x0002] = 0x12;
  z80_step(&z);
  r += check_push_order("call", 0x0FFF, 0x0FFE);
  r += check_pushed("call", &z, 0x0003);
  if (z.pc != 0x1234) {
    printf("*** ERROR call: pc %04X (expected 1234)\n", z.pc);
    r += 1;
  }

  // RST pushes the return address the same way.
  init_push_cpu(&z);
  memory[0x0000] = 0xFF; // rst 38h
  z80_step(&z);
  r += check_push_order("rst", 0x0FFF, 0x0FFE);
  r += check_pushed("rst", &z, 0x0001);
  if (z.pc != 0x0038) {
    printf("*** ERROR rst: pc %04X (expected 0038)\n", z.pc);
    r += 1;
  }

  // The IM 1 acknowledge pushes the interrupted pc high byte first.
  init_push_cpu(&z);
  z.interrupt_mode = 1;
  z.iff1 = 1;
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  r += check_push_order("im1 ack", 0x0FFF, 0x0FFE);
  r += check_pushed("im1 ack", &z, 0x0001);
  if (z.pc != 0x0038) {
    printf("*** ERROR im1 ack: pc %04X (expected 0038)\n", z.pc);
    r += 1;
  }

  // ex (sp),hl reads the stack pair, then writes the high byte to SP + 1
  // ahead of the low byte to SP.
  init_push_cpu(&z);
  memory[0x0000] = 0xE3; // ex (sp),hl
  memory[0x1000] = 0x78;
  memory[0x1001] = 0x56;
  z.h = 0x12;
  z.l = 0x34;
  z80_step(&z);
  r += check_push_order("ex (sp),hl", 0x1001, 0x1000);
  if (memory[0x1001] != 0x12 || memory[0x1000] != 0x34 || z.h != 0x56 || z.l != 0x78) {
    printf("*** ERROR ex (sp),hl: stack %02X%02X hl %02X%02X (expected 1234 5678)\n",
        memory[0x1001], memory[0x1000], z.h, z.l);
    r += 1;
  }

  printf("*** push order test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: IM2 acknowledge bus order
// The acknowledge pushes the return address before reading the vector, as
// the Zilog sequence does: INTA, one internal T, push PCH, push PCL, read
// the vector low byte, read the high byte, jump. The four cycles keep
// their 3 T shape either way; only their order moves, so the T span is
// unchanged and the jump target stays the same.
#define IM2ORDER_MAX_EVENTS 8
static struct {
  char kind; // 'M' M1 fetch, 'v' vector-table read, 'r' other read, 'w' write
  uint16_t addr;
  uint8_t val;
  unsigned long start; // bus_cycle_start: the cycle's first T
} im2order_events[IM2ORDER_MAX_EVENTS];
static int im2order_event_count;

static uint8_t im2order_rb(void* userdata, uint16_t addr, z80_read_kind_t kind) {
  z80* const z = (z80*)userdata;
  if (im2order_event_count < IM2ORDER_MAX_EVENTS) {
    im2order_events[im2order_event_count].kind =
        kind == Z80_READ_IM2_VECTOR ? 'v' : (kind == Z80_READ_M1 ? 'M' : 'r');
    im2order_events[im2order_event_count].addr = addr;
    im2order_events[im2order_event_count].val = memory[addr];
    im2order_events[im2order_event_count].start = z->bus_cycle_start;
    im2order_event_count++;
  }
  return memory[addr];
}

static void im2order_wb(void* userdata, uint16_t addr, uint8_t val) {
  z80* const z = (z80*)userdata;
  if (im2order_event_count < IM2ORDER_MAX_EVENTS) {
    im2order_events[im2order_event_count].kind = 'w';
    im2order_events[im2order_event_count].addr = addr;
    im2order_events[im2order_event_count].val = val;
    im2order_events[im2order_event_count].start = z->bus_cycle_start;
    im2order_event_count++;
  }
  memory[addr] = val;
}

// One IM2 acknowledge over a NOP with no WAIT stretching, so every T is
// pinned: the NOP fetch opens at T 0, the INTA at T 4, then one internal T
// puts the four acknowledge cycles at T 11/14/17/20. Push first means the
// two stack writes (PCH to SP - 1, PCL to SP - 2) come ahead of the two
// vector reads; the jump target and the pushed bytes pin the addresses and
// data, and MEMPTR must end on the jump target read from the table. The T
// counter span is the same in either order.
static int check_im2order(const char* name, uint8_t data, uint16_t vec_lo_addr,
    uint16_t vec_hi_addr, uint8_t vec_lo, uint8_t vec_hi, uint16_t target) {
  z80 z;
  z80_init(&z);
  z.read_byte = im2order_rb;
  z.write_byte = im2order_wb;
  z.port_in = in;
  z.port_out = out;
  z.userdata = &z;
  z.periodic_wait_mask = 0x0; // no stretching: absolute T below is exact
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z.pc = 0;
  z.sp = 0xF000;
  z.interrupt_mode = 2;
  z.iff1 = 1;
  z.iff2 = 1;
  z.i = 0x10;
  memory[vec_lo_addr] = vec_lo;
  memory[vec_hi_addr] = vec_hi;
  im2order_event_count = 0;
  z80_gen_int(&z, data);
  z80_step(&z);
  int failed = 0;
  // The NOP return address 0x0001 pushes PCH 0x00 ahead of PCL 0x01.
  static const struct {
    char kind;
    uint16_t addr;
    uint8_t val;
    unsigned long start;
  } expected[] = {
    {'M', 0x0000, 0x00, 0},
    {'w', 0xEFFF, 0x00, 11},
    {'w', 0xEFFE, 0x01, 14},
    {'v', 0, 0, 17}, // addresses and bytes filled in below
    {'v', 0, 0, 20},
  };
  if (im2order_event_count != 5) {
    printf("*** ERROR %s: %d bus events (expected 5)\n", name, im2order_event_count);
    return 1;
  }
  for (int i = 0; i < 5; ++i) {
    uint16_t addr = expected[i].addr;
    uint8_t val = expected[i].val;
    if (i == 3) {
      addr = vec_lo_addr;
      val = vec_lo;
    }
    if (i == 4) {
      addr = vec_hi_addr;
      val = vec_hi;
    }
    if (im2order_events[i].kind != expected[i].kind ||
        im2order_events[i].addr != addr || im2order_events[i].val != val ||
        im2order_events[i].start != expected[i].start) {
      printf("*** ERROR %s event %d: %c %04X %02X start %lu"
          " (expected %c %04X %02X start %lu)\n", name, i,
          im2order_events[i].kind, im2order_events[i].addr,
          im2order_events[i].val, im2order_events[i].start,
          expected[i].kind, addr, val, expected[i].start);
      failed = 1;
    }
  }
  if (z.pc != target || z.sp != 0xEFFE || z.t_counter != 23 || z.mem_ptr != target) {
    printf("*** ERROR %s: pc %04X sp %04X t %lu mem_ptr %04X"
        " (expected %04X EFFE 23 %04X)\n",
        name, z.pc, z.sp, z.t_counter, z.mem_ptr, target, target);
    failed = 1;
  }
  return failed;
}

static int run_im2order_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  // Plain vector byte: the table entry is I<<8|data and the next byte.
  r += check_im2order("im2 push first", 0x20, 0x1020, 0x1021, 0x34, 0x12, 0x1234);
  // Vector byte 0xFF: the high byte comes from the next page, a plain
  // 16-bit increment of the vector address, the way the other emulators
  // read it.
  r += check_im2order("im2 push first page carry", 0xFF, 0x10FF, 0x1100, 0x78, 0x56, 0x5678);
  printf("*** im2 order test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: derived cycle-count source
// With the source off (the default) the table charges are unchanged; with
// it on and a WAIT mask with exactly one free phase, one step charges the
// number of 4-T periods from its M1 sample slot to the next one; any other
// mask falls back to the table.
static int check_cyc_source(const char* name, uint8_t opcode, z80_cyc_source_t source,
    uint8_t mask, unsigned long expected_cyc) {
  z80 z;
  z80_init(&z);
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = in;
  z.port_out = out;
  memset(memory, 0, MEMORY_SIZE);
  memory[0x0000] = opcode;
  z.pc = 0;
  z80_set_cyc_source(&z, source);
  z.periodic_wait_mask = mask;
  z80_step(&z);
  if (z.cyc != expected_cyc) {
    printf("*** ERROR %s: cyc %lu (expected %lu)\n", name, z.cyc, expected_cyc);
    return 1;
  }
  return 0;
}

// A mask with every slot set would hold WAIT asserted forever: the stretch
// loop looks for the next clear slot and never finds one. No board produces
// such a mask, but a test or caller bug could, and hanging the process is
// the wrong answer. The core ignores a mask with no clear phase instead.
// The step runs in a forked child under alarm(): before the fix the child
// hangs and the alarm kills it, after it the child steps one unstretched
// NOP and exits clean.
static int check_wait_mask_no_free_phase_case(const char* name, int phase) {
  const pid_t pid = fork();
  if (pid < 0) {
    printf("*** ERROR %s: fork failed\n", name);
    return 1;
  }
  if (pid == 0) {
    alarm(5);
    z80 z;
    z80_init(&z);
    z.read_byte = rb;
    z.write_byte = wb;
    z.port_in = in;
    z.port_out = out;
    z.pc = 0;
    z.periodic_wait_mask = 0xF;
    if (phase == 2)
      z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
    z80_step(&z);
    if (phase == 0) {
      // One unstretched M1 fetch: 4 T, the table charge, pc advanced by one.
      _exit(z.t_counter == 4 && z.cyc == 4 && z.pc == 1 ? 0 : 2);
    }
    if (phase == 1) {
      // A device hold after the step adds its T and resumes sampling past
      // it instead of hanging: the recorded clear slot is not a real one
      // under this mask, but there is nothing to stretch on.
      z80_add_wait_states(&z, 2);
      _exit(z.t_counter == 6 && z.cyc == 4 && z.pc == 1 ? 0 : 2);
    }
    // With no free phase the derived source falls back to the table.
    _exit(z.t_counter == 4 && z.cyc == 4 && z.pc == 1 ? 0 : 2);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    if (WIFEXITED(status))
      printf("*** ERROR %s: child exited %d (expected an unstretched step)\n",
          name, WEXITSTATUS(status));
    else
      printf("*** ERROR %s: child did not finish (WAIT stretched forever)\n",
          name);
    return 1;
  }
  return 0;
}

static int check_wait_mask_no_free_phase(void) {
  memset(memory, 0, MEMORY_SIZE);
  memory[0x0000] = 0x00; // nop
  int r = 0;
  r += check_wait_mask_no_free_phase_case("mask 0xF", 0);
  r += check_wait_mask_no_free_phase_case("mask 0xF hold", 1);
  r += check_wait_mask_no_free_phase_case("mask 0xF derived", 2);
  return r;
}

static int run_cyc_source_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  // Source off: the table charge, whatever the mask.
  r += check_cyc_source("table nop", 0x00, Z80_CYC_FROM_TABLE, 0xD, 4);
  // Source on without a one-free-phase mask: still the table (mask 0
  // stretches nothing; mask 0x5 leaves two phases free).
  r += check_cyc_source("slots nop mask 0", 0x00, Z80_CYC_FROM_WAIT_SLOTS, 0x0, 4);
  r += check_cyc_source("slots nop mask 5", 0x00, Z80_CYC_FROM_WAIT_SLOTS, 0x5, 4);
  // Source on with one free phase: the M1-slot span (NOP one 4-T period,
  // INC BC two).
  r += check_cyc_source("slots nop", 0x00, Z80_CYC_FROM_WAIT_SLOTS, 0xD, 1);
  r += check_cyc_source("slots nop rotated", 0x00, Z80_CYC_FROM_WAIT_SLOTS, 0x7, 1);
  r += check_cyc_source("slots incbc", 0x03, Z80_CYC_FROM_WAIT_SLOTS, 0xD, 2);
  // Two chained steps share the running counter.
  {
    z80 z;
    z80_init(&z);
    z.read_byte = rb;
    z.write_byte = wb;
    z.port_in = in;
    z.port_out = out;
    memset(memory, 0, MEMORY_SIZE);
    memory[0x0000] = 0x00;
    memory[0x0001] = 0x00;
    z.pc = 0;
    z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
    z.periodic_wait_mask = 0xD;
    z80_step(&z);
    z80_step(&z);
    if (z.cyc != 2) {
      printf("*** ERROR slots nop chained: cyc %lu (expected 2)\n", z.cyc);
      r += 1;
    }
  }
  r += check_wait_mask_no_free_phase();
  printf("*** cyc source test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: host-driven interrupt check
// With int_check_by_host set, z80_step() stops after recording
// int_sample_point and leaves a pending interrupt alone; the host brings its
// inputs up to that T and runs the check itself with z80_check_interrupt().
// The acknowledge hook runs inside the INTA cycle, so it sees that cycle's
// start T and clear WAIT sample slot.
static unsigned long ack_bus_start;
static unsigned long ack_sample_t;
static unsigned long ack_t_counter;
static int ack_calls;

static void record_ack(z80* const z) {
  ack_calls += 1;
  ack_bus_start = z->bus_cycle_start;
  ack_sample_t = z->wait_sample_t;
  ack_t_counter = z->t_counter;
}

static void init_ack_cpu(z80* const z) {
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z->pc = 0;
  z->sp = 0xF000;
  z->interrupt_mode = 1;
  z->iff1 = 1;
  z->iff2 = 1;
  z->int_ack = record_ack;
  ack_calls = 0;
}

// Flag clear: the step accepts as before, and the hook runs inside the INTA
// cycle: bus_cycle_start is its first T, wait_sample_t its clear WAIT sample
// slot, and the T counter sits six T past the start. The mask-0xD row pins
// the clear slot past a stretch; NMI never calls the hook.
static int check_ack_sighting(const char* name, int mode, uint8_t data,
    uint8_t mask, unsigned long exp_bus, unsigned long exp_sample,
    unsigned long exp_t, uint16_t exp_pc) {
  z80 z;
  init_ack_cpu(&z);
  z.interrupt_mode = (uint8_t)mode;
  z.i = 0x10;
  memory[0x1020] = 0x34;
  memory[0x1021] = 0x12;
  z.periodic_wait_mask = mask;
  z80_gen_int(&z, data);
  z80_step(&z);
  if (z.pc != exp_pc || z.int_pending || ack_calls != 1) {
    printf("*** ERROR %s: pc %04X int_pending %d calls %d"
        " (expected %04X 0 1)\n", name, z.pc, z.int_pending, ack_calls,
        exp_pc);
    return 1;
  }
  if (ack_bus_start != exp_bus || ack_sample_t != exp_sample ||
      ack_t_counter != exp_t) {
    printf("*** ERROR %s: bus_start %lu sample %lu t %lu"
        " (expected %lu %lu %lu)\n", name, ack_bus_start, ack_sample_t,
        ack_t_counter, exp_bus, exp_sample, exp_t);
    return 1;
  }
  return 0;
}

static int check_hostcheck_clear(void) {
  int r = 0;
  // IM1 with no stretch: the INTA opens past the NOP fetch.
  r += check_ack_sighting("flag clear im1", 1, 0xFF, 0x0, 4, 7, 10, 0x0038);
  // IM0 runs a NOP off the frozen bus; the hook still sees the INTA.
  r += check_ack_sighting("flag clear im0", 0, 0x00, 0x0, 4, 7, 10, 0x0001);
  // IM2 reads its vector at 0x1020; the hook still sees the INTA first.
  r += check_ack_sighting("flag clear im2", 2, 0x20, 0x0, 4, 7, 10, 0x1234);
  // IM1 under mask 0xD: the INTA sample stretches to the clear slot.
  r += check_ack_sighting("flag clear mask d", 1, 0xFF, 0xD, 4, 9, 12,
      0x0038);
  // NMI takes 0x66 with no hook call: the hook is maskable-only.
  {
    z80 z;
    init_ack_cpu(&z);
    z80_gen_nmi(&z);
    z80_step(&z);
    if (z.pc != 0x0066 || z.nmi_pending || ack_calls != 0) {
      printf("*** ERROR nmi: pc %04X nmi_pending %d calls %d"
          " (expected 0066 0 0)\n", z.pc, z.nmi_pending, ack_calls);
      r += 1;
    }
  }
  return r;
}

// Flag set: the step leaves the pending interrupt and pc alone; the host's
// own check then reaches exactly the flag-clear end state.
static int check_hostcheck_set(void) {
  const char* name = "flag set";
  z80 clear;
  init_ack_cpu(&clear);
  z80_gen_int(&clear, 0xFF);
  z80_step(&clear);
  const unsigned long clear_bus = ack_bus_start;
  const unsigned long clear_sample = ack_sample_t;
  const unsigned long clear_t = ack_t_counter;
  z80 host;
  init_ack_cpu(&host);
  host.int_check_by_host = true;
  z80_gen_int(&host, 0xFF);
  z80_step(&host);
  if (host.pc != 0x0001 || !host.int_pending) {
    printf("*** ERROR %s: after the step pc %04X int_pending %d"
        " (expected 0001 1)\n", name, host.pc, host.int_pending);
    return 1;
  }
  z80_check_interrupt(&host);
  if (host.pc != clear.pc || host.sp != clear.sp ||
      host.t_counter != clear.t_counter || host.cyc != clear.cyc ||
      host.iff1 != clear.iff1 || host.iff2 != clear.iff2 ||
      host.r != clear.r || host.mem_ptr != clear.mem_ptr ||
      host.int_pending || ack_calls != 1 ||
      ack_bus_start != clear_bus || ack_sample_t != clear_sample ||
      ack_t_counter != clear_t) {
    printf("*** ERROR %s: host pc %04X sp %04X t %lu cyc %lu pending %d"
        " (clear pc %04X sp %04X t %lu cyc %lu)\n", name, host.pc, host.sp,
        host.t_counter, host.cyc, host.int_pending, clear.pc, clear.sp,
        clear.t_counter, clear.cyc);
    return 1;
  }
  return 0;
}

// Flag set with EI first: nothing is accepted at EI's boundary or by the
// host's check there; the next step takes it.
static int check_hostcheck_ei(void) {
  const char* name = "flag set after ei";
  z80 z;
  init_ack_cpu(&z);
  z.int_check_by_host = true;
  memory[0x0000] = 0xFB; // ei
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  if (z.pc != 0x0001 || !z.int_pending) {
    printf("*** ERROR %s: after EI pc %04X int_pending %d"
        " (expected 0001 1)\n", name, z.pc, z.int_pending);
    return 1;
  }
  z80_check_interrupt(&z);
  if (z.pc != 0x0001 || !z.int_pending) {
    printf("*** ERROR %s: host check accepted at EI's boundary (pc %04X)\n",
        name, z.pc);
    return 1;
  }
  z80_step(&z); // nop
  if (z.pc != 0x0002 || !z.int_pending) {
    printf("*** ERROR %s: after the next step pc %04X int_pending %d"
        " (expected 0002 1)\n", name, z.pc, z.int_pending);
    return 1;
  }
  z80_check_interrupt(&z);
  if (z.pc != 0x0038 || z.int_pending || ack_calls != 1) {
    printf("*** ERROR %s: pc %04X int_pending %d calls %d"
        " (expected 0038 0 1)\n", name, z.pc, z.int_pending, ack_calls);
    return 1;
  }
  return 0;
}

// z80_init() clears the flag: a garbage-filled struct still steps flag-clear.
// It also zeroes the M1 sample slot and the A-sourced port flag.
static int check_hostcheck_init(void) {
  const char* name = "init clears the flag";
  z80 z;
  memset(&z, 0xA5, sizeof z);
  z80_init(&z);
  if (z.step_m1_sample_tstate != 0 || z.port_high_byte_from_a) {
    printf("*** ERROR %s: sample slot %lu high-byte flag %d (want 0 0)\n",
        name, z.step_m1_sample_tstate, z.port_high_byte_from_a);
    return 1;
  }
  z.read_byte = rb;
  z.write_byte = wb;
  z.port_in = in;
  z.port_out = out;
  memset(memory, 0, MEMORY_SIZE); // NOPs everywhere
  z.pc = 0;
  z.sp = 0xF000;
  z.interrupt_mode = 1;
  z.iff1 = 1;
  z.iff2 = 1;
  z.int_ack = record_ack;
  ack_calls = 0;
  z80_gen_int(&z, 0xFF);
  z80_step(&z);
  if (z.int_check_by_host) {
    printf("*** ERROR %s: flag still set after z80_init()\n", name);
    return 1;
  }
  if (z.pc != 0x0038 || z.int_pending) {
    printf("*** ERROR %s: pc %04X int_pending %d (expected 0038 0)\n",
        name, z.pc, z.int_pending);
    return 1;
  }
  return 0;
}

// Flag clear vs flag set with an IM0 data byte that writes the flags: the
// acknowledge's Q latch must match. 0xAF is XOR A; with A seeded to 0xAF
// the result is zero, so Q comes out Z+P (0x44).
static int check_hostcheck_im0_q(void) {
  const char* name = "im0 q latch";
  z80 clear;
  init_ack_cpu(&clear);
  clear.interrupt_mode = 0;
  clear.a = 0xAF;
  z80_gen_int(&clear, 0xAF);
  z80_step(&clear);
  z80 host;
  init_ack_cpu(&host);
  host.interrupt_mode = 0;
  host.a = 0xAF;
  host.int_check_by_host = true;
  z80_gen_int(&host, 0xAF);
  z80_step(&host);
  z80_check_interrupt(&host);
  if (clear.q == 0) {
    printf("*** ERROR %s: flag-clear q is 0, the test proves nothing\n", name);
    return 1;
  }
  if (host.q != clear.q || host.pc != clear.pc) {
    printf("*** ERROR %s: host q %02X pc %04X (clear q %02X pc %04X)\n",
        name, host.q, host.pc, clear.q, clear.pc);
    return 1;
  }
  return 0;
}

static int run_hostcheck_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  r += check_hostcheck_clear();
  r += check_hostcheck_set();
  r += check_hostcheck_ei();
  r += check_hostcheck_init();
  r += check_hostcheck_im0_q();
  printf("*** host check test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: flag-clear vs flag-set equivalence sweep
// Every combination of NMI, interrupt mode, data byte, WAIT mask, cycle
// source and stepped instruction must end in the same whole-struct state
// whether the acknowledge runs inside z80_step() or in a following
// z80_check_interrupt() under int_check_by_host.
static unsigned long equiv_cyc, equiv_bus, equiv_ws, equiv_t;
static int equiv_calls;

static void record_equiv(z80* const z) {
  equiv_calls += 1;
  equiv_cyc = z->cyc;
  equiv_bus = z->bus_cycle_start;
  equiv_ws = z->wait_sample_t;
  equiv_t = z->t_counter;
}

static void setup_equiv(z80* const z, int mode, uint8_t data, uint8_t mask,
    int derived, uint8_t op, int nmi) {
  memset(z, 0, sizeof *z);
  z80_init(z);
  z->read_byte = rb;
  z->write_byte = wb;
  z->port_in = in;
  z->port_out = out;
  z->int_ack = record_equiv;
  memset(memory, 0, MEMORY_SIZE);
  memory[0x0000] = op;
  memory[0x0001] = 0x00;
  memory[0x3000] = 0x34;
  memory[0x3001] = 0x12;
  z->pc = 0;
  z->sp = 0xF000;
  z->i = 0x30;
  z->interrupt_mode = (uint8_t)mode;
  z->iff1 = 1;
  z->iff2 = 1;
  z->r = 0x7F;
  z->periodic_wait_mask = mask;
  z->t_counter = 2;
  if (derived)
    z80_set_cyc_source(z, Z80_CYC_FROM_WAIT_SLOTS);
  z80_set_timing(z, &z80_timing_cpc);
  if (nmi)
    z80_gen_nmi(z);
  else
    z80_gen_int(z, data);
  equiv_calls = 0;
}

static int run_hostequiv_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  static const uint8_t masks[] = {0x0, 0xD, 0x5, 0x7, 0xF};
  static const uint8_t datas[] = {0xFF, 0xAF, 0x3C, 0xCB, 0x00, 0x37};
  static const uint8_t ops[] = {0x00, 0x37, 0x3C, 0xFB, 0x76};
  int bad = 0, total = 0;
  for (int nmi = 0; nmi < 2; nmi++)
  for (int mode = 0; mode < 3; mode++)
  for (unsigned di = 0; di < sizeof datas; di++)
  for (unsigned mi = 0; mi < sizeof masks; mi++)
  for (int derived = 0; derived < 2; derived++)
  for (unsigned oi = 0; oi < sizeof ops; oi++) {
    if (nmi && (mode || di))
      continue;
    z80 clear_run, host_run;
    total++;
    setup_equiv(&clear_run, mode, datas[di], masks[mi], derived, ops[oi],
        nmi);
    z80_step(&clear_run);
    const int clear_calls = equiv_calls;
    const unsigned long clear_cyc = equiv_cyc, clear_bus = equiv_bus;
    const unsigned long clear_ws = equiv_ws, clear_t = equiv_t;
    setup_equiv(&host_run, mode, datas[di], masks[mi], derived, ops[oi],
        nmi);
    host_run.int_check_by_host = true;
    z80_step(&host_run);
    z80_check_interrupt(&host_run);
    host_run.int_check_by_host = false;
    if (memcmp(&clear_run, &host_run, sizeof clear_run) != 0 ||
        clear_calls != equiv_calls ||
        (clear_calls && (clear_cyc != equiv_cyc || clear_bus != equiv_bus ||
            clear_ws != equiv_ws || clear_t != equiv_t))) {
      bad++;
      printf("*** ERROR equiv nmi=%d im%d data=%02X mask=%X derived=%d"
          " op=%02X: q %02X vs %02X pc %04X/%04X t %lu/%lu cyc %lu/%lu\n",
          nmi, mode, datas[di], masks[mi], derived, ops[oi], clear_run.q,
          host_run.q, clear_run.pc, host_run.pc, clear_run.t_counter,
          host_run.t_counter, clear_run.cyc, host_run.cyc);
    }
  }
  printf("*** hostequiv: %d of %d cases differ\n", bad, total);
  free(memory);
  return bad != 0;
}

// MARK: M1 sample slot and strobe windows
// The step's first M1 WAIT-sample slot, the running cycle's /IORQ and /WR
// windows, and which port shapes take their address high byte from A.
// Every bus-cycle shape (M1 fetch, memory read, memory write, port read,
// port write) runs at four start phases under mask 0 (no stretching) and
// mask 0xD (one free phase), on both cycle-count sources. The slot must be
// the first free slot at or after entry T plus one; both windows must
// follow the running cycle's start and WAIT sample T; only IN A,(n) and
// OUT (n),A report the high byte from A. A trailing NMI acknowledge must
// leave the slot alone, and the halted path reports the slot its fetch
// would have used.
#define STROBE_MAX_EVENTS 16
static struct {
  char kind; // 'm' M1 fetch, 'r' memory/operand read, 'w' memory write,
             // 'i' port read, 'o' port write
  unsigned long start, sample;
  unsigned long iorq_begin, iorq_end, wr_begin, wr_end;
  bool high_byte_from_a;
} strobe_events[STROBE_MAX_EVENTS];
static int strobe_event_count;
static z80* strobe_cpu;

static void strobe_record(char kind, z80* const z) {
  if (strobe_event_count < STROBE_MAX_EVENTS) {
    strobe_events[strobe_event_count].kind = kind;
    strobe_events[strobe_event_count].start = z->bus_cycle_start;
    strobe_events[strobe_event_count].sample = z->wait_sample_t;
    const z80_strobe_window_t iorq = z80_iorq_window(z);
    const z80_strobe_window_t wr = z80_write_window(z);
    strobe_events[strobe_event_count].iorq_begin = iorq.begin_half_tstates;
    strobe_events[strobe_event_count].iorq_end = iorq.end_half_tstates;
    strobe_events[strobe_event_count].wr_begin = wr.begin_half_tstates;
    strobe_events[strobe_event_count].wr_end = wr.end_half_tstates;
    strobe_events[strobe_event_count].high_byte_from_a =
        z->port_high_byte_from_a;
    strobe_event_count++;
  }
}

static uint8_t strobe_rb(void* userdata, uint16_t addr, z80_read_kind_t kind) {
  (void)userdata;
  strobe_record(kind == Z80_READ_M1 ? 'm' : 'r', strobe_cpu);
  return memory[addr];
}

static void strobe_wb(void* userdata, uint16_t addr, uint8_t val) {
  (void)userdata;
  strobe_record('w', strobe_cpu);
  memory[addr] = val;
}

static uint8_t strobe_in(z80* const z, uint16_t port) {
  (void)port;
  strobe_record('i', z);
  return 0xFF;
}

static void strobe_out(z80* const z, uint16_t port, uint8_t val) {
  (void)port;
  (void)val;
  strobe_record('o', z);
}

// First free phase-1 slot at or after entry T plus one: mask 0xD leaves
// only phase 1 free.
static unsigned long strobe_slot_0xd(unsigned long entry) {
  const unsigned long t = entry + 1;
  return t + ((1u - t) & 3u);
}

static int check_strobe_shape(const char* name, const uint8_t* bytes,
    const char* want_kinds, int port_kind, int derived, int settles_first,
    long want_rel_begin, long want_rel_end, int split_prefix) {
  int failed = 0;
  static const uint8_t masks[] = {0x0, 0xD};
  for (unsigned mi = 0; mi < sizeof masks; mi++)
  for (unsigned long entry = 0; entry < 4; entry++) {
    z80 z;
    z80_init(&z);
    z.read_byte = strobe_rb;
    z.write_byte = strobe_wb;
    z.port_in = strobe_in;
    z.port_out = strobe_out;
    memset(memory, 0, MEMORY_SIZE);
    memcpy(memory + 0x1000, bytes, 8);
    z.pc = 0x1000;
    z.sp = 0xF000;
    z.a = 0x56;
    z.b = 0x12;
    z.c = 0x34;
    z.t_counter = entry;
    z.periodic_wait_mask = masks[mi];
    if (derived)
      z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
    strobe_cpu = &z;
    strobe_event_count = 0;
    // HALT only settles into the halted path on the step after the one
    // that executes it, so discard the executing step and check the next.
    if (settles_first)
      z80_step(&z);
    // A prefixed shape costs two steps: the prefix byte's own M1 first,
    // checked here, then the opcode step below with want_kinds holding the
    // remaining events.
    if (split_prefix) {
      const unsigned long pbase = strobe_cpu->t_counter;
      z80_step(&z);
      const unsigned long pwant =
          masks[mi] == 0 ? pbase + 1 : strobe_slot_0xd(pbase);
      char pwhere[96];
      snprintf(pwhere, sizeof pwhere, "%s prefix mask=%X entry=%lu%s", name,
          masks[mi], pbase, derived ? " derived" : "");
      if (z.step_m1_sample_tstate != pwant) {
        printf("*** ERROR %s: sample slot %lu (want %lu)\n", pwhere,
            z.step_m1_sample_tstate, pwant);
        failed = 1;
      }
      if (strobe_event_count != 1 || strobe_events[0].kind != 'm' ||
          strobe_events[0].start != pbase ||
          strobe_events[0].sample != pwant) {
        printf("*** ERROR %s: prefix step events %d (want 1 m)\n", pwhere,
            strobe_event_count);
        failed = 1;
        continue;
      }
      strobe_event_count = 0;
    }
    const unsigned long base = strobe_cpu->t_counter;
    strobe_event_count = 0;
    z80_step(&z);
    const unsigned long want_slot =
        masks[mi] == 0 ? base + 1 : strobe_slot_0xd(base);
    char where[96];
    snprintf(where, sizeof where, "%s mask=%X entry=%lu%s", name,
        masks[mi], base, derived ? " derived" : "");
    if (z.step_m1_sample_tstate != want_slot) {
      printf("*** ERROR %s: sample slot %lu (want %lu)\n", where,
          z.step_m1_sample_tstate, want_slot);
      failed = 1;
    }
    const size_t want_count = strlen(want_kinds);
    if ((size_t)strobe_event_count != want_count) {
      printf("*** ERROR %s: %d bus events (want %lu %s)\n", where,
          strobe_event_count, (unsigned long)want_count, want_kinds);
      failed = 1;
      continue;
    }
    for (size_t i = 0; i < want_count; i++) {
      if (strobe_events[i].kind != want_kinds[i]) {
        printf("*** ERROR %s: event %lu kind %c (want %c)\n", where,
            (unsigned long)i, strobe_events[i].kind, want_kinds[i]);
        failed = 1;
      }
      const unsigned long bcs = strobe_events[i].start;
      const unsigned long ws = strobe_events[i].sample;
      if (strobe_events[i].iorq_begin != 2 * bcs + 2 ||
          strobe_events[i].iorq_end != 2 * ws + 3 ||
          strobe_events[i].wr_begin != 2 * bcs + 3 ||
          strobe_events[i].wr_end != 2 * ws + 3) {
        printf("*** ERROR %s: event %lu windows [%lu,%lu]/[%lu,%lu]"
            " (want [%lu,%lu]/[%lu,%lu])\n", where, (unsigned long)i,
            strobe_events[i].iorq_begin, strobe_events[i].iorq_end,
            strobe_events[i].wr_begin, strobe_events[i].wr_end,
            2 * bcs + 2, 2 * ws + 3, 2 * bcs + 3, 2 * ws + 3);
        failed = 1;
      }
      const int want_a =
          port_kind != 0 && strobe_events[i].kind == port_kind;
      if (strobe_events[i].high_byte_from_a != want_a) {
        printf("*** ERROR %s: event %lu high byte from A %d (want %d)\n",
            where, (unsigned long)i,
            strobe_events[i].high_byte_from_a, want_a);
        failed = 1;
      }
    }
    // The first M1 fetch opens at the step's entry T and samples the slot.
    if (want_count > 0 && (strobe_events[0].start != base ||
        strobe_events[0].sample != want_slot)) {
      printf("*** ERROR %s: first fetch start/sample %lu/%lu"
          " (want %lu/%lu)\n", where, strobe_events[0].start,
          strobe_events[0].sample, base, want_slot);
      failed = 1;
    }
    // Literal half-T /IORQ row of the port access, relative to the grid
    // origin, under the one-free-phase mask. Mask 0 stretches nothing, so
    // its windows are narrower and not listed here.
    if (want_rel_begin >= 0 && masks[mi] == 0xD) {
      size_t pi = want_count;
      for (size_t i = 0; i < want_count; i++)
        if (strobe_events[i].kind == 'i' || strobe_events[i].kind == 'o') {
          pi = i;
          break;
        }
      if (pi >= want_count) {
        printf("*** ERROR %s: no port access for the literal row\n", where);
        failed = 1;
      } else {
        const unsigned long origin = 2 * (z.step_m1_sample_tstate - 1);
        const long got_begin = (long)(strobe_events[pi].iorq_begin - origin);
        const long got_end = (long)(strobe_events[pi].iorq_end - origin);
        if (got_begin != want_rel_begin || got_end != want_rel_end) {
          printf("*** ERROR %s: port /IORQ [%ld,%ld) (want [%ld,%ld))\n",
              where, got_begin, got_end, want_rel_begin, want_rel_end);
          failed = 1;
        }
      }
    }
  }
  return failed;
}

// A trailing NMI acknowledge runs more bus cycles after the instruction;
// the step's sample slot must still be the instruction's own.
static int check_strobe_ack_slot(void) {
  int failed = 0;
  static const uint8_t masks[] = {0x0, 0xD};
  for (unsigned mi = 0; mi < sizeof masks; mi++)
  for (unsigned long entry = 0; entry < 4; entry++) {
    z80 z;
    z80_init(&z);
    z.read_byte = strobe_rb;
    z.write_byte = strobe_wb;
    z.port_in = strobe_in;
    z.port_out = strobe_out;
    memset(memory, 0, MEMORY_SIZE);
    memory[0x1000] = 0x00; // NOP, then the NMI acknowledge
    z.pc = 0x1000;
    z.sp = 0xF000;
    z.t_counter = entry;
    z.periodic_wait_mask = masks[mi];
    z80_gen_nmi(&z);
    strobe_cpu = &z;
    strobe_event_count = 0;
    z80_step(&z);
    const unsigned long want_slot =
        masks[mi] == 0 ? entry + 1 : strobe_slot_0xd(entry);
    if (z.step_m1_sample_tstate != want_slot) {
      printf("*** ERROR nmi mask=%X entry=%lu: sample slot %lu (want %lu)\n",
          masks[mi], entry, z.step_m1_sample_tstate, want_slot);
      failed = 1;
    }
  }
  return failed;
}

// Masks with several free phases: the slot is the first free slot at or
// after entry T plus one, which is also where the first M1 fetch samples.
static int check_sample_slot_rows(void) {
  int failed = 0;
  static const struct { unsigned mask; unsigned long entry, want; } rows[] = {
    {0x1, 3, 5}, {0x3, 0, 2}, {0x3, 3, 6}, {0x5, 1, 3},
    {0x5, 3, 5}, {0x9, 2, 5}, {0x9, 3, 5}, {0xD, 2, 5},
  };
  for (size_t ri = 0; ri < sizeof rows / sizeof rows[0]; ri++) {
    z80 z;
    z80_init(&z);
    z.read_byte = strobe_rb;
    z.write_byte = strobe_wb;
    z.port_in = strobe_in;
    z.port_out = strobe_out;
    memset(memory, 0, MEMORY_SIZE);
    memory[0x1000] = 0x00; // NOP
    z.pc = 0x1000;
    z.sp = 0xF000;
    z.t_counter = rows[ri].entry;
    z.periodic_wait_mask = (uint8_t)rows[ri].mask;
    strobe_cpu = &z;
    strobe_event_count = 0;
    z80_step(&z);
    if (z.step_m1_sample_tstate != rows[ri].want) {
      printf("*** ERROR slot mask=%X entry=%lu: slot %lu (want %lu)\n",
          rows[ri].mask, rows[ri].entry,
          z.step_m1_sample_tstate, rows[ri].want);
      failed = 1;
    }
    if (strobe_event_count < 1 || strobe_events[0].kind != 'm' ||
        strobe_events[0].sample != rows[ri].want) {
      printf("*** ERROR slot mask=%X entry=%lu: first fetch samples %lu (want %lu)\n",
          rows[ri].mask, rows[ri].entry,
          strobe_event_count < 1 ? 9999 : strobe_events[0].sample, rows[ri].want);
      failed = 1;
    }
  }
  return failed;
}

static int im0_observed_high;
static uint8_t im0_observe_bus(z80* const z, uint16_t pc, z80_im0_cycle_t cycle) {
  (void)pc;
  (void)cycle;
  if (z->port_high_byte_from_a)
    im0_observed_high = 1;
  return 0x00;
}

// The A-sourced port flag is clear for every non-port bus cycle: an IM0
// CALL acknowledged right after an OUT (n),A reads its bytes through
// im0_bus with the flag clear.
static int check_im0_clears_high_byte(void) {
  const char* name = "im0 clears the high byte flag";
  z80 z;
  z80_init(&z);
  z.read_byte = strobe_rb;
  z.write_byte = strobe_wb;
  z.port_in = strobe_in;
  z.port_out = strobe_out;
  memset(memory, 0, MEMORY_SIZE);
  memory[0x1000] = 0xD3; // OUT (n),A sets the flag on its port cycle
  memory[0x1001] = 0x34;
  z.pc = 0x1000;
  z.sp = 0xF000;
  z.a = 0x56;
  z.iff1 = 1; // the trailing check only acknowledges with iff1 set
  strobe_cpu = &z;
  strobe_event_count = 0;
  z80_step(&z);
  if (!z.port_high_byte_from_a) {
    printf("*** ERROR %s: OUT (n),A left the flag clear\n", name);
    return 1;
  }
  z.interrupt_mode = 0;
  z80_gen_int(&z, 0xCD); // CALL: its bytes arrive through im0_bus
  z.im0_bus = im0_observe_bus;
  im0_observed_high = 0;
  strobe_event_count = 0;
  z80_step(&z);
  if (z.pc != 0x0000 || im0_observed_high) {
    printf("*** ERROR %s: pc %04X flag seen high %d (want 0000 0)\n",
        name, z.pc, im0_observed_high);
    return 1;
  }
  return 0;
}

static int run_strobe_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int r = 0;
  static const uint8_t nop[] = {0x00, 0, 0, 0, 0, 0, 0, 0};
  static const uint8_t ld_a_nn[] = {0x3A, 0x00, 0x20, 0, 0, 0, 0, 0};
  static const uint8_t ld_nn_a[] = {0x32, 0x00, 0x20, 0, 0, 0, 0, 0};
  static const uint8_t in_a_n[] = {0xDB, 0x34, 0, 0, 0, 0, 0, 0};
  static const uint8_t out_n_a[] = {0xD3, 0x34, 0, 0, 0, 0, 0, 0};
  static const uint8_t in_b_c[] = {0xED, 0x40, 0, 0, 0, 0, 0, 0};
  static const uint8_t out_c_b[] = {0xED, 0x41, 0, 0, 0, 0, 0, 0};
  static const uint8_t outi[] = {0xED, 0xA3, 0, 0, 0, 0, 0, 0};
  static const uint8_t ini[] = {0xED, 0xA2, 0, 0, 0, 0, 0, 0};
  static const uint8_t halt[] = {0x76, 0, 0, 0, 0, 0, 0, 0};
  for (int derived = 0; derived < 2; derived++) {
    r += check_strobe_shape("nop", nop, "m", 0, derived, 0, -1, -1, 0);
    r += check_strobe_shape("ld a,(nn)", ld_a_nn, "mrrr", 0, derived, 0, -1, -1, 0);
    r += check_strobe_shape("ld (nn),a", ld_nn_a, "mrrw", 0, derived, 0, -1, -1, 0);
    r += check_strobe_shape("in a,(n)", in_a_n, "mri", 'i', derived, 0, 16, 21, 0);
    r += check_strobe_shape("out (n),a", out_n_a, "mro", 'o', derived, 0, 16, 21, 0);
    r += check_strobe_shape("in b,(c)", in_b_c, "mi", 0, derived, 0, 10, 21, 1);
    r += check_strobe_shape("out (c),b", out_c_b, "mo", 0, derived, 0, 10, 21, 1);
    r += check_strobe_shape("outi", outi, "mro", 0, derived, 0, 24, 29, 1);
    r += check_strobe_shape("ini", ini, "miw", 0, derived, 0, 12, 21, 1);
    r += check_strobe_shape("halt", halt, "", 0, derived, 1, -1, -1, 0);
  }
  r += check_sample_slot_rows();
  r += check_im0_clears_high_byte();
  r += check_strobe_ack_slot();
  printf("*** strobe windows test: %s\n", r == 0 ? "OK" : "FAILED");
  free(memory);
  return r != 0;
}

// MARK: derived acknowledge charge
// Under Z80_CYC_FROM_WAIT_SLOTS every acknowledge charges its own span
// the way a step does, so the step total is the instruction span plus the
// acknowledge span (NOP 5/7/4, INC BC 4/6/4 for IM1/IM2/NMI; IM 0 RST
// 5/4, an injected CALL nn 6/5, a CB-prefixed byte 4/3).
static int check_ack_span(const char* name, int mode, int nmi, uint8_t op,
    uint8_t data, unsigned long exp_cyc, uint16_t exp_pc, int start_halted,
    uint8_t wait_mask) {
  z80 z;
  init_ack_cpu(&z);
  z80_set_timing(&z, &z80_timing_cpc);
  z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
  z.periodic_wait_mask = wait_mask;
  z.halted = (uint8_t)start_halted;
  z.interrupt_mode = (uint8_t)mode;
  z.i = 0x10;
  memory[0x0000] = op;
  memory[0x1020] = 0x34;
  memory[0x1021] = 0x12;
  if (nmi)
    z80_gen_nmi(&z);
  else
    z80_gen_int(&z, data);
  // An injected prefix byte is its own step, so a prefixed injected
  // instruction needs follow-on steps: run to the end of the instruction,
  // as the chainim0 test does. Anything else finishes in one step.
  int steps = 0;
  do {
    z80_step(&z);
    steps += 1;
  } while (z.pending_prefix != Z80_PREFIX_NONE && steps < 4);
  if (z.cyc != exp_cyc || z.pc != exp_pc) {
    printf("*** ERROR %s: cyc %lu pc %04X (expected %lu %04X)\n",
        name, z.cyc, z.pc, exp_cyc, exp_pc);
    return 1;
  }
  return 0;
}

// A read callback that moves the WAIT mask to one with two free phases
// the first time it is called, i.e. during the step's opcode fetch.
static uint8_t rb_mask_change(void* userdata, uint16_t addr,
    z80_read_kind_t kind) {
  (void)kind;
  ((z80*)userdata)->periodic_wait_mask = 0x5;
  return memory[addr];
}

// A mask changed mid-step to one with no single free phase: the
// acknowledge keeps its table charge instead of aligning to a phase.
static int check_ack_mask_change_mode(int mode, uint8_t data,
    unsigned long expected, uint16_t exp_pc) {
  z80 z;
  init_ack_cpu(&z);
  z.read_byte = rb_mask_change;
  z.userdata = &z;
  z80_set_timing(&z, &z80_timing_cpc);
  z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
  z.periodic_wait_mask = 0xD;
  z.interrupt_mode = (uint8_t)mode;
  z.i = 0x10;
  z80_gen_int(&z, data);
  z80_step(&z);
  int r = 0;
  // The NOP's derived period (1) plus the table charge.
  if (z.cyc != expected || z.pc != exp_pc) {
    printf("*** ERROR mask change mode %d: cyc %lu pc %04X (expected %lu %04X)\n",
        mode, z.cyc, z.pc, expected, exp_pc);
    r = 1;
  }
  // The accepted acknowledge relatches the step latch to its own
  // decision, so the latch reads table after both steps.
  if (z.cyc_derived_step != 0) {
    printf("*** ERROR mask change mode %d: cyc_derived_step %u (want 0)\n",
        mode, z.cyc_derived_step);
    r = 1;
  }
  return r;
}

static int check_ack_mask_change(void) {
  int r = 0;
  r += check_ack_mask_change_mode(1, 0xFF, 1 + z80_timing_cpc.int_ack_im1, 0x0038);
  // IM 0 on the same changed mask: the NOP's period, the acknowledge
  // overhead and the injected RST's own table charge.
  r += check_ack_mask_change_mode(0, 0xFF,
      1 + z80_timing_cpc.int_ack_im0 + z80_timing_cpc.cyc_00[0xFF], 0x0038);
  return r;
}

static int hold_added;
static void wb_hold_once(void* userdata, uint16_t addr, uint8_t val) {
  z80* const z = (z80*)userdata;
  if (!hold_added) {
    hold_added = 1;
    z80_add_wait_states(z, 4);
  }
  memory[addr] = val;
}

// A host hold during an acknowledge's first push lands in the step's cycle
// count on the sampled-slots source, for IM 0 as for IM 1.
static int check_ack_hold(int mode, uint8_t data, unsigned long expected) {
  z80 z;
  init_ack_cpu(&z);
  z.write_byte = wb_hold_once;
  z.userdata = &z;
  z80_set_timing(&z, &z80_timing_cpc);
  z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
  z.periodic_wait_mask = 0xD;
  z.interrupt_mode = (uint8_t)mode;
  hold_added = 0;
  z80_gen_int(&z, data);
  z80_step(&z);
  if (z.cyc != expected || z.pc != 0x0038 || !hold_added) {
    printf("*** ERROR ack hold mode %d: cyc %lu pc %04X held %d (expected %lu 0038 1)\n",
        mode, z.cyc, z.pc, hold_added, expected);
    return 1;
  }
  return 0;
}

// IM 0 with a prefixed injection: the CB byte is the first INTA, the
// follow-on byte (SET 0,(HL), 0xC6) a further one from the host bus. A
// host hold during the follow-on write must land in the step total, and
// the follow-on step latches the derived-source decision like any other.
static uint8_t im0_followon_byte(z80* const z, uint16_t pc, z80_im0_cycle_t kind) {
  (void)z;
  (void)pc;
  return kind == Z80_IM0_INTA ? 0xC6 : 0;
}

static int check_im0_followon(const char* name, int hold, unsigned long exp_cyc,
    unsigned long exp_t) {
  z80 z;
  init_ack_cpu(&z);
  z.write_byte = wb_hold_once;
  z.im0_bus = im0_followon_byte;
  z.userdata = &z;
  z80_set_timing(&z, &z80_timing_cpc);
  z80_set_cyc_source(&z, Z80_CYC_FROM_WAIT_SLOTS);
  z.periodic_wait_mask = 0xD;
  z.interrupt_mode = 0;
  z.h = 0x40;
  z.l = 0;
  hold_added = hold ? 0 : 1; // 1: the callback adds no hold
  z80_gen_int(&z, 0xCB);
  int steps = 0;
  do {
    z80_step(&z);
    steps += 1;
  } while ((z.im0_active || z.pending_prefix != Z80_PREFIX_NONE) && steps < 5);
  int r = 0;
  if (exp_cyc != 0 && (z.cyc != exp_cyc || z.t_counter != exp_t)) {
    printf("*** ERROR %s: cyc %lu t %lu (expected %lu %lu)\n",
        name, z.cyc, z.t_counter, exp_cyc, exp_t);
    r = 1;
  }
  if (z.cyc_derived_step != 1) {
    printf("*** ERROR %s: cyc_derived_step %u after the follow-on step (want 1)\n",
        name, z.cyc_derived_step);
    r = 1;
  }
  return r;
}

static int run_ackcyc_test(void) {
  memory = malloc(MEMORY_SIZE);
  if (memory == NULL) {
    return 1;
  }
  int mismatches = 0;
  // The instruction span (NOP one period, INC BC two, the halted filler
  // one) plus the acknowledge span.
  mismatches += check_ack_span("nop im1", 1, 0, 0x00, 0xFF, 6, 0x0038, 0, 0xD);
  mismatches += check_ack_span("nop im2", 2, 0, 0x00, 0x20, 8, 0x1234, 0, 0xD);
  mismatches += check_ack_span("nop nmi", 0, 1, 0x00, 0xFF, 5, 0x0066, 0, 0xD);
  mismatches += check_ack_span("incbc im1", 1, 0, 0x03, 0xFF, 6, 0x0038, 0, 0xD);
  mismatches += check_ack_span("incbc im2", 2, 0, 0x03, 0x20, 8, 0x1234, 0, 0xD);
  mismatches += check_ack_span("incbc nmi", 0, 1, 0x03, 0xFF, 6, 0x0066, 0, 0xD);
  // A halted step runs 4 internal T before the trailing acknowledge, so
  // each total is one 4-T period above the acknowledge span: 6/8/5.
  mismatches += check_ack_span("halt im1", 1, 0, 0x76, 0xFF, 6, 0x0038, 1, 0xD);
  mismatches += check_ack_span("halt im2", 2, 0, 0x76, 0x20, 8, 0x1234, 1, 0xD);
  mismatches += check_ack_span("halt nmi", 0, 1, 0x76, 0xFF, 5, 0x0066, 1, 0xD);
  // IM 0: the derived NOP span and the off-bus NOP's own table charge,
  // i.e. NOP span 1 + acknowledge span 2.
  mismatches += check_ack_span("nop im0", 0, 0, 0x00, 0x00, 3, 0x0001, 0, 0xD);
  // IM 0 from its sampled span: the instruction span plus the
  // acknowledge span (RST 5/4, CALL nn 6/5, CB-prefixed byte 4/3).
  mismatches += check_ack_span("nop im0 rst", 0, 0, 0x00, 0xFF, 6, 0x0038, 0, 0xD);
  mismatches += check_ack_span("incbc im0", 0, 0, 0x03, 0xFF, 6, 0x0038, 0, 0xD);
  mismatches += check_ack_span("exsphl im0", 0, 0, 0xE3, 0xFF, 10, 0x0038, 0, 0xD);
  mismatches += check_ack_span("nop im0 call", 0, 0, 0x00, 0xCD, 7, 0x0000, 0, 0xD);
  mismatches += check_ack_span("incbc im0 call", 0, 0, 0x03, 0xCD, 7, 0x0000, 0, 0xD);
  mismatches += check_ack_span("nop im0 cb", 0, 0, 0x00, 0xCB, 5, 0x0001, 0, 0xD);
  mismatches += check_ack_span("incbc im0 cb", 0, 0, 0x03, 0xCB, 5, 0x0001, 0, 0xD);
  mismatches += check_im0_followon("im0 cb c6 hold", 1, 8, 31);
  mismatches += check_im0_followon("im0 cb c6", 0, 0, 0);
  mismatches += check_ack_hold(1, 0xFF, 7);
  mismatches += check_ack_hold(0, 0xFF, 7);
  mismatches += check_ack_mask_change();
  printf("*** acknowledge charge test: %s\n", mismatches == 0 ? "OK" : "FAILED");
  free(memory);
  return mismatches != 0;
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
  if (argc > 1 && strcmp(argv[1], "memptr") == 0) {
    return run_memptr_test();
  }
  if (argc > 1 && strcmp(argv[1], "blockxy") == 0) {
    return run_blockxy_test();
  }
  if (argc > 1 && strcmp(argv[1], "ioflags") == 0) {
    return run_ioflags_test();
  }
  if (argc > 1 && strcmp(argv[1], "eidelay") == 0) {
    return run_ei_delay_test();
  }
  if (argc > 1 && strcmp(argv[1], "retiiff") == 0) {
    return run_reti_iff_test();
  }
  if (argc > 1 && strcmp(argv[1], "strayr") == 0) {
    return run_stray_r_test();
  }
  if (argc > 1 && strcmp(argv[1], "chainlong") == 0) {
    return run_chain_long_test();
  }
  if (argc > 1 && strcmp(argv[1], "chainim0") == 0) {
    return run_chain_im0_test();
  }
  if (argc > 1 && strcmp(argv[1], "prefixstep") == 0) {
    return run_prefix_step_test();
  }
  if (argc > 1 && strcmp(argv[1], "qlatch") == 0) {
    return run_qlatch_test();
  }
  if (argc > 1 && strcmp(argv[1], "pushorder") == 0) {
    return run_push_order_test();
  }
  if (argc > 1 && strcmp(argv[1], "im2order") == 0) {
    return run_im2order_test();
  }
  if (argc > 1 && strcmp(argv[1], "hostcheck") == 0) {
    return run_hostcheck_test();
  }
  if (argc > 1 && strcmp(argv[1], "hostequiv") == 0) {
    return run_hostequiv_test();
  }
  if (argc > 1 && strcmp(argv[1], "cycsource") == 0) {
    return run_cyc_source_test();
  }
  if (argc > 1 && strcmp(argv[1], "strobe") == 0) {
    return run_strobe_test();
  }
  if (argc > 1 && strcmp(argv[1], "ackcyc") == 0) {
    return run_ackcyc_test();
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
  if (which && strcmp(which, "prelim") != 0 && strcmp(which, "zexdoc") != 0 &&
      strcmp(which, "zexall") != 0) {
    // Every named suite returns above; down here only the three rom
    // exercisers are valid, so an unknown name must fail loudly instead
    // of running nothing and exiting 0.
    fprintf(stderr, "unknown suite: %s (expected prelim, zexdoc or zexall)\n", which);
    free(memory);
    return 1;
  }
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
    r += run_memptr_test();
    r += run_blockxy_test();
    r += run_ioflags_test();
    r += run_ei_delay_test();
    r += run_reti_iff_test();
    r += run_stray_r_test();
    r += run_chain_long_test();
    r += run_chain_im0_test();
    r += run_prefix_step_test();
    r += run_qlatch_test();
    r += run_cyc_source_test();
  }
  return r != 0;
}
