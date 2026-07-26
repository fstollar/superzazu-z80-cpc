<!-- Mirrored from the CPC Plus raycaster project's
docs/z80/reference/z80-cpc-timing.md as of 2026-07-26 (updated: all 12
original cpctech/grimware conflicts now resolved via RASM as a third
source; interrupt-acknowledge IM1/IM2/NMI figures corrected via CPCEC
cross-check, cpctech's own published numbers for those were wrong). That
repo is the canonical source if this copy ever needs updating. -->

# Z80 Instruction Timing on Amstrad CPC (NOPs = µs)

Merged/canonical timing table for this project's cycle budgeting. Two
independent published sources (cpctech.org.uk and grimware.org) were
cross-checked; 12 rows disagreed. 10 of those 12 were resolved by working
out the exact per-M-cycle bus-stretching model from real Z80 M-cycle
T-state structure; the remaining 2 stayed genuinely ambiguous from that
model alone. A third independent CPC-specific source — the RASM
assembler's own author-maintained timing annex — was cross-checked
2026-07-26 and broke both remaining ties (**all 12 main-table rows
resolved**), plus re-confirmed all 10 previously-resolved rows with no
new disagreements. See `docs/z80/reference/z80-timing/z80-mcycle-model.md`
for the full derivation and the RASM tie-break reasoning.

Separately, the "Other timings" interrupt-acknowledge figures (below)
came from cpctech alone and turned out to be wrong for IM 1/IM 2 — see
that section for the CPCEC-sourced correction, cross-checked 2026-07-26.

Raw sources, saved in full: `docs/z80/reference/z80-timing/cpctech-instrtim.md`
(basis for this table's rows), `docs/z80/reference/z80-timing/grimware-z80-instruction-set.md`
(cross-check source — more detailed per-instruction, e.g. opcode encoding,
but its own author flags it "unfinished" and its Bit-test/Rotate-Shift/
Input-Output tables are empty), `docs/z80/reference/z80-timing/rasm-nops.md`
(third cross-check source, tie-breaker for the last 2 main-table rows),
`docs/z80/reference/z80-timing/z80-mcycle-model.md` (the resolution
methodology, citing exact M-cycle T-state tables from a generic Z80
reference), and `docs/z80/reference/z80-timing/cpcec-interrupt-timing.md`
(CPCEC emulator source, used for the interrupt-acknowledge correction).

## Why these numbers differ from a generic Z80 reference

The CPC's Gate Array generates a 4 MHz clock for the CPU but also arbitrates
RAM access with the video hardware (CRTC fetches 2 bytes every µs and is
given priority). It does this by driving the Z80's `/WAIT` pin. The
precise mechanism (worked out in `z80-mcycle-model.md`): **every Z80
M-cycle that touches the bus** (opcode fetch, memory read/write, I/O) is
individually stretched to the next whole multiple of 4 T-states (1 µs) —
M-cycles that are pure internal computation (no bus request) mostly aren't.
This is *not* the same as taking an instruction's total T-states and
dividing by 4 — that naive approach fails on instructions like `LDI` (16
T-states on a generic Z80 → naively 4 µs, but the real/measured value,
agreed by both sources, is 5 µs: its 4 M-cycles are `4,4,3,5`, and each one
individually rounds up to a 4T boundary, giving `1+1+1+2` = 5 µs).

`rp` = 16-bit reg (HL, DE, BC); `r` = 8-bit reg; `cc` = condition; `n`/`nn`
= 8/16-bit immediate; `dd` = 8-bit displacement; `nc`/`c` = condition not
satisfied/satisfied.

| Mnemonic(s) | µs |
|---|---|
| NOP | 1 |
| LD rp,nnnn | 3 |
| INC rp / DEC rp | 2 |
| INC r / DEC r | 1 |
| LD r,n | 2 |
| RLCA / RRCA / RLA / RRA | 1 |
| EX AF,AF' | 1 |
| ADD HL,rp | 3 |
| LD A,(BC) / LD (BC),A / LD (DE),A / LD A,(DE) | 2 |
| DJNZ dd | b-1=0: 3, b-1≠0: 4 |
| JR dd | 3 |
| JR cc,dd | nc: 2, c: 3 |
| LD (nnnn),HL / LD HL,(nnnn) | 5 |
| DAA | 1 |
| LD A,(nnnn) / LD (nnnn),A | 4 |
| INC (HL) / DEC (HL) | 3 |
| LD (HL),nn | 3 |
| SCF / CCF / CPL | 1 |
| LD r,r | 1 |
| LD r,(HL) / LD (HL),r | 2 |
| HALT | 1, variable |
| ADD A,r / ADC A,r / SUB r / SBC A,r | 1 |
| ADD A,(HL) / ADC A,(HL) / SUB A,(HL) / SBC A,(HL) | 2 |
| AND r / XOR r / OR r / CP r | 1 |
| AND (HL) / XOR (HL) / OR (HL) / CP (HL) | 2 |
| RET | 3 |
| RET cc | nc: 2, c: 4 |
| POP rp | 3 |
| PUSH rp | 4 |
| JP cc,nnnn | 3 |
| JP nnnn | 3 |
| CALL nnnn | 5 |
| CALL cc,nnnn | nc: 3, c: 5 |
| ADD A,n / ADC A,n / SUB n / SBC A,n | 2 |
| AND n / XOR n / OR n / CP n | 2 |
| RST n | 4 |
| RLC r / RRC r / RR r / RL r / SLA r / SLL r / SRL r | 2 |
| RLC (HL) / RRC (HL) / RR (HL) / RL (HL) / SLA (HL) / SLL (HL) / SRL (HL) | 4 |
| BIT b,r / RES b,r / SET b,r | 2 |
| BIT b,(HL) | 3 |
| RES b,(HL) / SET b,(HL) | 4 |
| IN A,(nn) / OUT (nn),A | 3 |
| EXX | 1 |
| ADD IX,rp | 4 |
| LD IX,nnnn | 4 |
| LD (nnnn),IX / LD IX,(nnnn) | 6 |
| INC IX / DEC IX | 3 |
| INC HIX / DEC HIX / INC LIX / DEC LIX | 2 |
| LD HIX,n / LD LIX,n | 3 |
| INC (IX+dd) / DEC (IX+dd) | 6 |
| LD (IX+dd),nn | 6 |
| LD r,HIX / LD r,LIX / LD HIX,r / LD LIX,r | 2 |
| LD r,(IX+dd) / LD (IX+dd),r | 5 |
| ADD A,HIX / ADC A,HIX / SUB HIX / SBC A,HIX | 2 |
| ADD A,(IX+dd) / ADC A,(IX+dd) / SUB (IX+dd) / SBC A,(IX+dd) | 5 |
| AND (IX+dd) / XOR (IX+dd) / OR (IX+dd) / CP (IX+dd) | 5 |
| AND HIX / XOR HIX / OR HIX / CP HIX | 2 |
| RLC (IX+dd) / RRC (IX+dd) / RL (IX+dd) / RR (IX+dd) / SLA (IX+dd) / SRA (IX+dd) / SLL (IX+dd) / SRL (IX+dd) | 7 |
| BIT b,(IX+dd) | 6 |
| RES b,(IX+dd) / SET b,(IX+dd) | 7 |
| PUSH IX / PUSH IY | 5 |
| POP IX / POP IY | 4 |
| EX (SP),IX / EX (SP),IY | 7 |
| JP (IX) | 2 |
| EX (SP),HL | 6 |
| LD SP,IX | 3 |
| JP (HL) | 1 |
| EX DE,HL | 1 |
| IN r,(C) / OUT (C),r / IN F,(C) / OUT (C),0 | 4 |
| SBC HL,rp / ADC HL,rp | 4 |
| LD (nnnn),rp / LD rp,(nnnn) (incl. ED-prefixed HL forms) | 6 |
| NEG | 2 |
| IM 0 / IM 1 / IM 2 | 2 |
| LD I,A / LD A,I / LD R,A / LD A,R | 3 |
| DI / EI | 1 |
| LD SP,HL | 2 |
| RLD / RRD | 5 |
| LDI / LDD | 5 |
| OUTI / OUTD | 5 |
| LDIR / LDDR | BC-1=0: 5, BC-1≠0: 6 (per iteration) |
| RETN / RETI | 4 |
| DD / FD prefix | 1 (see note) |
| ED "nop" (ED 00–ED 3F) | 2 |
| CPI / CPD | 4 |
| INI / IND | 5 |
| CPIR / CPDR | BC-1=0: 4, BC-1≠0: 6 (per iteration) |
| INIR / INDR / OTIR / OTDR | BC-1=0: 5, BC-1≠0: 6 (per iteration) |

Notes:
- The `DD`/`FD prefix` row (1 µs) applies when multiple `DD`/`FD` prefixes
  stack together.
- `IY` timings are identical to the `IX` timings shown above throughout
  this table (confirmed via M-cycle math for `EX (SP),IY` where grimware's
  raw table disagreed with itself on this point — see
  `z80-mcycle-model.md`).
- **`POP IX`/`POP IY` = 4 µs**, not 5 — this is the one row where the
  M-cycle model sided with grimware over cpctech (cpctech's original 5 µs
  was wrong). See `z80-mcycle-model.md` for why `PUSH IX` (5 µs) and
  `POP IX` (4 µs) aren't symmetric: `PUSH`'s M-cycles are `4,5,3,3` (an
  extra internal `SP←SP-2` folded into the second cycle before the first
  write), `POP`'s are `4,4,3,3` (no such extra cycle needed before the
  first read).

## Resolved by third-source tie-break (RASM annex)

Two rows had a genuine ambiguity in the M-cycle model itself (an
internal-only cycle whose bus/non-bus status wasn't pinned down from that
model alone). `docs/reference/z80-timing/rasm-nops.md` — a third
independent CPC-specific source — broke both ties; see
`docs/reference/z80-timing/z80-mcycle-model.md` for the full reasoning.
All 12 originally-conflicting rows are now resolved.

1. **`LD (IX+dd),nn`** (i.e. `LD (IX+d),n`): cpctech said **6 µs**;
   grimware said **5 µs**. **RASM confirms 6 µs** (cpctech's side) —
   already reflected in the table above.
2. **`CPI`/`CPD`** (and by extension `CPIR`/`CPDR`): cpctech grouped these
   with `INI`/`IND` at **5 µs**; grimware gave **4 µs**. **RASM confirms
   4 µs** (grimware's side) for `CPI`/`CPD` specifically — `INI`/`IND`
   stay at the undisputed 5 µs. This also meant the table's old combined
   `CPIR/INIR/OTIR/CPDR/INDR/OTDR` row was wrong for two of the six
   mnemonics — now split into separate `CPIR`/`CPDR` (4 µs terminal) and
   `INIR`/`INDR`/`OTIR`/`OTDR` (5 µs terminal) rows above.

Neither value is yet oracle/hardware-measured — RASM is a strong
independent cross-check (its author, roudoudou, writes CPC-hardware code
professionally), not a substitute for empirical verification if either
instruction ends up on a genuinely hot path.

## Other timings

- Interrupt-acknowledge → first instruction of the ISR: **IM 1: 4 µs;
  IM 2: 6 µs; NMI: 4 µs** (corrected 2026-07-26 — see below). IM 0 still
  depends on the instruction fetched; no fixed number.
- 1 monitor scanline: 64 µs.
- 1 PAL (50 Hz) monitor frame: 19968 µs (≈ 312 scanlines × 64 µs).
