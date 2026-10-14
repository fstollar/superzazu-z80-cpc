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
  }
  return r != 0;
}
