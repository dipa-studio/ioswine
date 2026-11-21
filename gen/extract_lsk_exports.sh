#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DSC="${1:-${IOSWINE_DSC:-vendor-local/23G90__iPhone17,3/dyld_shared_cache_arm64e}}"

if [ ! -f "$DSC" ]; then
    echo "Error: Dyld shared cache not found at $DSC" >&2
    exit 1
fi

TMPDIR="${TMPDIR:-build/tmp}"
mkdir -p "$TMPDIR" gen/data

LOADS_TXT="$TMPDIR/lsk_loads.txt"
SYMS_TXT="$TMPDIR/lsk_syms.txt"

# Dump load commands (to find exact section boundaries)
ipsw dsc macho "$DSC" libsystem_kernel.dylib -l > "$LOADS_TXT"

# Dump all symbols (public and private, to compute gaps)
ipsw dsc macho "$DSC" libsystem_kernel.dylib -n > "$SYMS_TXT"

python3 - "$LOADS_TXT" "$SYMS_TXT" gen/data/libsystem_kernel.exports.tsv gen/data/libsystem_kernel.exports.summary << 'EOF'
import sys, re
from collections import defaultdict

loads_path = sys.argv[1]
syms_path = sys.argv[2]
tsv_path = sys.argv[3]
summary_path = sys.argv[4]

# Parse section end addresses from load commands
# Format: sz=0x... off=... addr=0x<start>-0x<end>   <seg>.<sec>
section_ends = {}
with open(loads_path, "r", encoding="utf-8") as f:
    for line in f:
        m = re.search(r"addr=0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+)\s+(\S+)\.(\S+)", line)
        if m:
            end_addr = int(m.group(2), 16)
            seg = m.group(3)
            sec = m.group(4)
            section_ends[(seg, sec)] = end_addr

# Parse all symbols from macho -n output
# Format: 0x<addr>:  (<seg>,<sec>) <visibility> <name>
all_symbols = [] # (addr, seg, sec, is_ext, name)
with open(syms_path, "r", encoding="utf-8") as f:
    for line in f:
        m = re.match(r"^(0x[0-9a-fA-F]+):\s+\(([^,]+),([^)]+)\)\s+([^\t]+)\t([^\t\n\r]+)", line)
        if m:
            addr = int(m.group(1), 16)
            seg = m.group(2)
            sec = m.group(3)
            sym_type = m.group(4)
            name = m.group(5).strip()
            is_ext = ("external" in sym_type) and ("non-external" not in sym_type)
            all_symbols.append((addr, seg, sec, is_ext, name))

# Group symbols by section to compute gaps
sec_symbols = defaultdict(list)
for s in all_symbols:
    sec_symbols[(s[1], s[2])].append(s)

exports = [] # (name, kind, addr, gap)
code_gaps = []
all_gaps = []

for sec, slist in sec_symbols.items():
    sec_end = section_ends.get(sec)
    all_addrs = sorted(list(set(s[0] for s in slist)))
    addr_to_next = {}
    for i, a in enumerate(all_addrs):
        if i + 1 < len(all_addrs):
            addr_to_next[a] = all_addrs[i+1] - a
        else:
            if sec_end is not None:
                addr_to_next[a] = sec_end - a
            else:
                addr_to_next[a] = 0

    for s in slist:
        addr, seg, sec_name, is_ext, name = s
        if is_ext:
            kind = "code" if (seg == "__TEXT" and sec_name == "__text") else "data"
            gap = addr_to_next[addr]
            exports.append((name, kind, addr, gap))
            all_gaps.append(gap)
            if kind == "code":
                code_gaps.append(gap)

# Sort exports by address ascending, then name ascending
exports.sort(key=lambda x: (x[2], x[0]))

# Write TSV
with open(tsv_path, "w", encoding="utf-8") as f:
    f.write("name\tkind\taddr\tgap\n")
    for name, kind, addr, gap in exports:
        f.write(f"{name}\t{kind}\t0x{addr:x}\t{gap}\n")

# Write Summary
total_exports = len(exports)
code_exports = sum(1 for e in exports if e[1] == "code")
data_exports = sum(1 for e in exports if e[1] == "data")
code_lt_16 = sum(1 for g in code_gaps if g < 16)
min_code_gap = min(code_gaps) if code_gaps else 0
min_gap = min(all_gaps) if all_gaps else 0

summary_text = f"""Total exports: {total_exports}
Code exports: {code_exports}
Data exports: {data_exports}
Code exports with gap < 16: {code_lt_16}
Minimum code gap: {min_code_gap}
Minimum gap: {min_gap}
"""

with open(summary_path, "w", encoding="utf-8") as f:
    f.write(summary_text)

print(summary_text)
EOF
