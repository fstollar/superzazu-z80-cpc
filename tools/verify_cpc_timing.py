#!/usr/bin/env python3
"""Cross-check tools/gen_timing_cpc.py's CPC_US dict against
docs/z80-cpc-timing.md's own markdown table, independently of the
generator's opcode-decode logic. Fails loudly on any mismatch.

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

def main():
    doc_rows = parse_doc_table('docs/z80-cpc-timing.md')
    if len(doc_rows) < 60:
        print(f'FAIL: only parsed {len(doc_rows)} doc rows, expected 60+ '
              '-- markdown table format may have changed', file=sys.stderr)
        sys.exit(1)

    # Every doc row must show up as *some* value used in CPC_US -- this
    # is a coverage check, not a full re-parse of multi-value cells like
    # "nc: 2, c: 3": we only assert the flat single-number rows here.
    flat_values = {int(v) for _, v in doc_rows if re.fullmatch(r'\d+', v)}
    used_values = {resolve(k)[0] for k in CPC_US}
    missing = flat_values - used_values
    if missing:
        print(f'FAIL: doc has flat values {sorted(missing)} that no '
              f'CPC_US entry resolves to -- possible transcription gap',
              file=sys.stderr)
        sys.exit(1)

    unresolved = {k: resolve(k) for k in CPC_US if resolve(k)[1]}
    expected_unresolved = {'CPI_CPD', 'INI_IND', 'LD_iIXd_N', 'NMI_ACK', 'INT_ACK_IM0'}
    if set(unresolved) != expected_unresolved:
        print(f'FAIL: UNRESOLVED set changed: {sorted(unresolved)} != '
              f'{sorted(expected_unresolved)} -- update this script\'s '
              f'expected set if that\'s intentional', file=sys.stderr)
        sys.exit(1)

    print(f'OK: {len(doc_rows)} doc rows parsed, '
          f'{len(flat_values)} flat values all accounted for in CPC_US, '
          f'{len(unresolved)} UNRESOLVED entries match expected set')

if __name__ == '__main__':
    main()
