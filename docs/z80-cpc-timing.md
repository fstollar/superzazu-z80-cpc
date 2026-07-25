<!-- Mirrored from the CPC Plus raycaster project's
docs/z80/reference/z80-cpc-timing.md as of 2026-07-25. That repo is the
canonical source if this copy ever needs updating. -->

# Z80 Instruction Timing on Amstrad CPC (NOPs = µs)

Merged/canonical timing table for this project's cycle budgeting. Two
independent published sources (cpctech.org.uk and grimware.org) were
cross-checked; 12 rows disagreed. 10 of those 12 have since been resolved
by working out the exact per-M-cycle bus-stretching model from real Z80
M-cycle T-state structure — see
`docs/reference/z80-timing/z80-mcycle-model.md` for the full derivation.
The remaining 2 are marked **⚠[n]** with both values given in "Still
unresolved" below — **verify empirically before relying on either value**
for anything performance-critical on those two.

Raw sources, saved in full: `docs/reference/z80-timing/cpctech-instrtim.md`
(basis for this table's rows), `docs/reference/z80-timing/grimware-z80-instruction-set.md`
(cross-check source — more detailed per-instruction, e.g. opcode encoding,
but its own author flags it "unfinished" and its Bit-test/Rotate-Shift/
Input-Output tables are empty), and `docs/reference/z80-timing/z80-mcycle-model.md`
(the resolution methodology, citing exact M-cycle T-state tables from a
generic Z80 reference).

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
| LD (IX+dd),nn ⚠[1] | 6 |
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
| CPI / INI / CPD / IND ⚠[2] | 5 |
| CPIR / INIR / OTIR / CPDR / INDR / OTDR ⚠[2] | BC-1=0: 5, BC-1≠0: 6 (per iteration) |

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

## Still unresolved

Two rows have a genuine ambiguity in the M-cycle model itself (an
internal-only cycle whose bus/non-bus status wasn't pinned down) — see
`docs/reference/z80-timing/z80-mcycle-model.md` for the full reasoning.

1. **`LD (IX+dd),nn`** (i.e. `LD (IX+d),n`): cpctech says **6 µs**;
   grimware says **5 µs**. Model gives 6 µs *if* the disputed M-cycle is
   bus activity (fetching immediate `n`), but the *agreed* (both sources,
   5 µs) `LD r,(IX+dd)`/`LD (IX+dd),r` has the identical M-cycle shape with
   a cycle that's plausibly pure-internal (index-address calculation, no
   byte to fetch there) instead. Which reading applies to the `,n` variant
   isn't settled here.
2. **`CPI`/`CPD`** (and by extension `CPIR`/`CPDR`): cpctech groups these
   with `INI`/`IND` at **5 µs**; grimware gives **4 µs**. Structurally
   identical M-cycles to `LDI` (`4,4,3,5`), whose last cycle is a genuine
   bus write and rounds to 5 µs total (matching both sources) — but `CPI`'s
   last cycle is BC-decrement-and-compare with no write and no further
   read, so it may be pure-internal and not get the same stretch. Leaning
   cpctech (5 µs) on the `LDI` analogy, but not certain.

## Other timings

- Interrupt-acknowledge → first instruction of the ISR: IM 0 depends on
  the instruction fetched; IM 1: 5 µs; IM 2: 19 µs.
- 1 monitor scanline: 64 µs.
- 1 PAL (50 Hz) monitor frame: 19968 µs (≈ 312 scanlines × 64 µs).
