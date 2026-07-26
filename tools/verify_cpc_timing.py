#!/usr/bin/env python3
"""Cross-check tools/gen_timing_cpc.py's CPC_US dict against
docs/z80-cpc-timing.md's own markdown table, independently of the
generator's opcode-decode logic. Fails loudly on any mismatch.

Checks each of the doc's 87 table rows against the specific CPC_US
key(s) that row's mnemonic(s) correspond to (DOC_ROW_TO_KEYS below),
not just "does this number appear somewhere in CPC_US" -- a per-row
mapping is the only way this catches a single mnemonic's value being
transcribed wrong when the wrong value happens to still be some other
entry's correct value (e.g. a 2 miskeyed as a 3 -- both are common
CPC timings, so a set-membership check can't tell them apart).

Run from the repo root: python3 tools/verify_cpc_timing.py
"""
import re
import sys

sys.path.insert(0, 'tools')
from gen_timing_cpc import CPC_US, resolve  # reuse the dict, not the decode

def parse_doc_table(path):
    """Returns a list of (raw_mnemonic_cell, raw_value_cell) from every
    '| ... | ... |' row in the markdown table (skips the header/divider
    rows and the 'Other timings' bullet list, which isn't tabular)."""
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith('|') or line.startswith('|---'):
                continue
            cells = [c.strip() for c in line.strip('|').split('|')]
            if len(cells) != 2 or cells[0] == 'Mnemonic(s)':
                continue
            rows.append((cells[0], cells[1]))
    return rows

def parse_value_cell(cell):
    """Returns (plain_value, None) for a single number (annotations like
    ' (see note)', ', variable', ' (per iteration)' are ignored), or
    (not_taken_value, taken_value) for the two known two-value patterns:
    'nc: X, c: Y' and '<cond>: X, <cond>: Y (per iteration)'."""
    pairs = re.findall(r':\s*(\d+)', cell)
    if pairs:
        if len(pairs) != 2:
            raise AssertionError(f'expected 2 labelled values, got {len(pairs)}: {cell!r}')
        return int(pairs[0]), int(pairs[1])
    m = re.match(r'^\s*(\d+)', cell)
    if not m:
        raise AssertionError(f'no leading integer in value cell: {cell!r}')
    return int(m.group(1)), None

# Maps each doc row's exact mnemonic-cell text to the CPC_US key(s) that
# row's value must match. Two-value rows (base/not-taken, taken/repeat)
# list keys as (base_key, bonus_key) tuples; the checked totals are
# resolve(base_key) == not_taken_value and
# resolve(base_key) + resolve(bonus_key) == taken_value.
DOC_ROW_TO_KEYS = {
    'NOP': ['NOP'],
    'LD rp,nnnn': ['LD_RP_NN'],
    'INC rp / DEC rp': ['INC_RP', 'DEC_RP'],
    'INC r / DEC r': ['INC_R', 'DEC_R'],
    'LD r,n': ['LD_R_N'],
    'RLCA / RRCA / RLA / RRA': ['RLCA', 'RRCA', 'RLA', 'RRA'],
    "EX AF,AF'": ['EX_AF_AF'],
    'ADD HL,rp': ['ADD_HL_RP'],
    'LD A,(BC) / LD (BC),A / LD (DE),A / LD A,(DE)':
        ['LD_A_iBC', 'LD_iBC_A', 'LD_iDE_A', 'LD_A_iDE'],
    'DJNZ dd': [('DJNZ_NT', 'DJNZ_T_BONUS')],
    'JR dd': ['JR'],
    'JR cc,dd': [('JR_CC_NT', 'JR_CC_T_BONUS')],
    'LD (nnnn),HL / LD HL,(nnnn)': ['LD_inn_HL', 'LD_HL_inn'],
    'DAA': ['DAA'],
    'LD A,(nnnn) / LD (nnnn),A': ['LD_A_inn', 'LD_inn_A'],
    'INC (HL) / DEC (HL)': ['INC_iHL', 'DEC_iHL'],
    'LD (HL),nn': ['LD_iHL_N'],
    'SCF / CCF / CPL': ['SCF', 'CCF', 'CPL'],
    'LD r,r': ['LD_R_R'],
    'LD r,(HL) / LD (HL),r': ['LD_R_iHL', 'LD_iHL_R'],
    'HALT': ['HALT'],
    'ADD A,r / ADC A,r / SUB r / SBC A,r': ['ALU_R'],
    'ADD A,(HL) / ADC A,(HL) / SUB A,(HL) / SBC A,(HL)': ['ALU_iHL'],
    'AND r / XOR r / OR r / CP r': ['ALU_R'],
    'AND (HL) / XOR (HL) / OR (HL) / CP (HL)': ['ALU_iHL'],
    'RET': ['RET'],
    'RET cc': [('RET_CC_NT', 'RET_CC_T_BONUS')],
    'POP rp': ['POP_RP'],
    'PUSH rp': ['PUSH_RP'],
    'JP cc,nnnn': ['JP_CC'],
    'JP nnnn': ['JP_NN'],
    'CALL nnnn': ['CALL_NN'],
    'CALL cc,nnnn': [('CALL_CC_NT', 'CALL_CC_T_BONUS')],
    'ADD A,n / ADC A,n / SUB n / SBC A,n': ['ALU_N'],
    'AND n / XOR n / OR n / CP n': ['ALU_N'],
    'RST n': ['RST'],
    'RLC r / RRC r / RR r / RL r / SLA r / SLL r / SRL r': ['ROT_R'],
    'RLC (HL) / RRC (HL) / RR (HL) / RL (HL) / SLA (HL) / SLL (HL) / SRL (HL)': ['ROT_iHL'],
    'BIT b,r / RES b,r / SET b,r': ['BIT_R'],
    'BIT b,(HL)': ['BIT_iHL'],
    'RES b,(HL) / SET b,(HL)': ['RES_SET_iHL'],
    'IN A,(nn) / OUT (nn),A': ['IN_A_iN', 'OUT_iN_A'],
    'EXX': ['EXX'],
    'ADD IX,rp': ['ADD_IX_RP'],
    'LD IX,nnnn': ['LD_IX_NN'],
    'LD (nnnn),IX / LD IX,(nnnn)': ['LD_inn_IX', 'LD_IX_inn'],
    'INC IX / DEC IX': ['INC_IX', 'DEC_IX'],
    'INC HIX / DEC HIX / INC LIX / DEC LIX': ['INC_HIX', 'DEC_HIX', 'INC_LIX', 'DEC_LIX'],
    'LD HIX,n / LD LIX,n': ['LD_HIX_N', 'LD_LIX_N'],
    'INC (IX+dd) / DEC (IX+dd)': ['INC_iIXd', 'DEC_iIXd'],
    'LD (IX+dd),nn': ['LD_iIXd_N'],
    'LD r,HIX / LD r,LIX / LD HIX,r / LD LIX,r': ['LD_R_HLIX'],
    'LD r,(IX+dd) / LD (IX+dd),r': ['LD_R_iIXd', 'LD_iIXd_R'],
    'ADD A,HIX / ADC A,HIX / SUB HIX / SBC A,HIX': ['ALU_A_HIX'],
    'ADD A,(IX+dd) / ADC A,(IX+dd) / SUB (IX+dd) / SBC A,(IX+dd)': ['ALU_A_iIXd'],
    'AND (IX+dd) / XOR (IX+dd) / OR (IX+dd) / CP (IX+dd)': ['ALU_A_iIXd'],
    'AND HIX / XOR HIX / OR HIX / CP HIX': ['ALU_A_HIX'],
    'RLC (IX+dd) / RRC (IX+dd) / RL (IX+dd) / RR (IX+dd) / SLA (IX+dd) / SRA (IX+dd) / SLL (IX+dd) / SRL (IX+dd)':
        ['DDFDCB_OTHER'],
    'BIT b,(IX+dd)': ['DDFDCB_BIT'],
    'RES b,(IX+dd) / SET b,(IX+dd)': ['DDFDCB_OTHER'],
    'PUSH IX / PUSH IY': ['PUSH_IX'],
    'POP IX / POP IY': ['POP_IX'],
    'EX (SP),IX / EX (SP),IY': ['EX_iSP_IX'],
    'JP (IX)': ['JP_iIX'],
    'EX (SP),HL': ['EX_iSP_HL'],
    'LD SP,IX': ['LD_SP_IX'],
    'JP (HL)': ['JP_iHL'],
    'EX DE,HL': ['EX_DE_HL'],
    'IN r,(C) / OUT (C),r / IN F,(C) / OUT (C),0': ['IN_R_iC', 'OUT_iC_R'],
    'SBC HL,rp / ADC HL,rp': ['ADC_SBC_HL_RP'],
    'LD (nnnn),rp / LD rp,(nnnn) (incl. ED-prefixed HL forms)': ['LD_inn_RP'],
    'NEG': ['NEG'],
    'IM 0 / IM 1 / IM 2': ['IM'],
    'LD I,A / LD A,I / LD R,A / LD A,R': ['LD_I_A', 'LD_A_I', 'LD_R_A', 'LD_A_R'],
    'DI / EI': ['DI', 'EI'],
    'LD SP,HL': ['LD_SP_HL'],
    'RLD / RRD': ['RLD', 'RRD'],
    'LDI / LDD': ['LDI', 'LDD'],
    'OUTI / OUTD': ['OUTI_OUTD'],
    'LDIR / LDDR': [('LDIR_LDDR', 'LDIR_LDDR_REPEAT_BONUS')],
    'RETN / RETI': ['RETN_RETI'],
    'DD / FD prefix': ['DD_PREFIX', 'FD_PREFIX', 'DDFD_PASSTHROUGH'],
    'ED "nop" (ED 00–ED 3F)': ['ED_NOP'],
    'CPI / CPD': ['CPI_CPD'],
    'INI / IND': ['INI_IND'],
    'CPIR / CPDR': [('CPIR_CPDR', 'CPIR_CPDR_REPEAT_BONUS')],
    'INIR / INDR / OTIR / OTDR': [
        ('INIR_INDR', 'INIR_INDR_REPEAT_BONUS'),
        ('OTIR_OTDR', 'OTIR_OTDR_REPEAT_BONUS'),
    ],
}

# Interrupt-ack values live in the doc's "Other timings" prose bullets,
# not the table, so they're outside DOC_ROW_TO_KEYS -- checked directly.
# IM 0 and NMI ack have no fixed doc value (IM 0 is explicitly
# instruction-dependent; NMI ack isn't covered at all) and stay
# UNRESOLVED, so they're intentionally not asserted here.
PROSE_TIMINGS = {
    'INT_ACK_IM1': 5,  # "IM 1: 5 us"
    'INT_ACK_IM2': 19,  # "IM 2: 19 us"
}

def check_row(mnemonic, value_cell, errors):
    entries = DOC_ROW_TO_KEYS.get(mnemonic)
    if entries is None:
        errors.append(f'no DOC_ROW_TO_KEYS mapping for doc row {mnemonic!r} '
                       '-- add one or this row is unverified')
        return
    not_taken, taken = parse_value_cell(value_cell)
    for entry in entries:
        if isinstance(entry, tuple):
            base_key, bonus_key = entry
            base_val = resolve(base_key)[0]
            if base_val != not_taken:
                errors.append(f'{mnemonic!r}: doc base value {not_taken} but '
                               f'CPC_US[{base_key!r}] resolves to {base_val}')
            if taken is not None:
                total = base_val + resolve(bonus_key)[0]
                if total != taken:
                    errors.append(f'{mnemonic!r}: doc taken/repeat value {taken} '
                                   f'but {base_key!r}+{bonus_key!r} resolves to {total}')
        else:
            val = resolve(entry)[0]
            if val != not_taken:
                errors.append(f'{mnemonic!r}: doc value {not_taken} but '
                               f'CPC_US[{entry!r}] resolves to {val}')

def main():
    doc_rows = parse_doc_table('docs/z80-cpc-timing.md')
    if len(doc_rows) != 87:
        print(f'FAIL: parsed {len(doc_rows)} doc rows, expected exactly 87 '
              '-- markdown table format may have changed, update DOC_ROW_TO_KEYS',
              file=sys.stderr)
        sys.exit(1)

    errors = []
    for mnemonic, value_cell in doc_rows:
        check_row(mnemonic, value_cell, errors)

    unmapped_keys = set(DOC_ROW_TO_KEYS) - {m for m, _ in doc_rows}
    if unmapped_keys:
        errors.append(f'DOC_ROW_TO_KEYS has entries for rows no longer in the doc: '
                       f'{sorted(unmapped_keys)}')

    for field, expected in PROSE_TIMINGS.items():
        val = resolve(field)[0]
        if val != expected:
            errors.append(f'{field}: doc prose says {expected} but CPC_US resolves to {val}')

    unresolved = {k: resolve(k) for k in CPC_US if resolve(k)[1]}
    expected_unresolved = {'NMI_ACK', 'INT_ACK_IM0'}
    if set(unresolved) != expected_unresolved:
        errors.append(f'UNRESOLVED set changed: {sorted(unresolved)} != '
                       f'{sorted(expected_unresolved)} -- update this script\'s '
                       f'expected set if that\'s intentional')

    if errors:
        print(f'FAIL: {len(errors)} mismatch(es):', file=sys.stderr)
        for e in errors:
            print(f'  - {e}', file=sys.stderr)
        sys.exit(1)

    print(f'OK: {len(doc_rows)} doc rows independently cross-checked against '
          f'CPC_US per-mnemonic (not just value-set membership), '
          f'{len(PROSE_TIMINGS)} prose-timing values checked, '
          f'{len(unresolved)} UNRESOLVED entries match expected set')

if __name__ == '__main__':
    main()
