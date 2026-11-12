#!/usr/bin/env python3
"""CTRL status frame vs. the main chip's frame check, from the real sources.

Takes the $D sprintf format and the XOR-appending code from LPC845/main.c, and frame_checksum_ok()
from LPC1768/intermcu_protocol.c (between CHECKSUM_BEGIN and CHECKSUM_END), builds a frame on the
Mac with typical and worst-case values, and checks the LPC1768 accepts it and that it fits both
176-byte buffers. CTRL v1.11 first added a 28th field, and every LPC1768 firmware dropped every
frame (bench 2026-11-12): this is the test that would have caught it.

  python3 bench/frame_compat_test.py [repo root]
"""
import os, re, subprocess, sys, tempfile

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ctrl = open(os.path.join(root, 'LPC845', 'main.c')).read()
main = open(os.path.join(root, 'LPC1768', 'intermcu_protocol.c')).read()

# The $D sprintf: format string and its argument list up to the closing ");"
m = re.search(r'^[ \t]*sprintf\(gc_xmit_buf, ("\$D[^"]*")\s*,(.*?)\);', ctrl, re.S | re.M)   # not a //-commented one
fmt, args = m.group(1), m.group(2)
nargs = len([a for a in re.sub(r'//[^\n]*', '', args).split(',') if a.strip()])
nconv = len(re.findall(r'%(?:0?\d+)?[dc]', fmt))
assert nargs == nconv, f'$D format has {nconv} conversions but {nargs} arguments'

# The XOR block right after it
xor = re.search(r'(\{\s*int frame_len = \(int\)strlen\(gc_xmit_buf\);.*?\n\t\t\t\})', ctrl, re.S).group(1)

check = main[main.index('/* CHECKSUM_BEGIN */'):main.index('/* CHECKSUM_END */')]
hexv = re.search(r'(static int hex_value\(char c\)\s*\{.*?\n\})', main, re.S).group(1)

def frame_c(values):
    return f'''
    {{
        char gc_xmit_buf[176 + 64];   /* Room to measure an overflow instead of crashing */
        memset(gc_xmit_buf, 0, sizeof(gc_xmit_buf));
        sprintf(gc_xmit_buf, {fmt}, {values});
        {xor}
        size_t n = strlen(gc_xmit_buf);
        int ok = frame_checksum_ok(gc_xmit_buf);
        printf("%s frame: %zu chars, LPC1768 %s\\n", name, n, ok ? "accepts it" : "DROPS IT");
        if (!ok || n >= 176) fails++;
    }}'''

# Arguments: one per conversion. Worst case = the widest each field can really get: ADC 4095, HDC
# raw 65535, a 15-minute wind pulse total of 90000 (100 Hz), 900 s, rain-day tips 99999, the reset
# field with the clock state in bits 8-9 (0x21F = 543), date-times 2 digits each.
convs = re.findall(r'%(?:0?\d+)?([dc])', fmt)
WORST = [20, 26, 11, 12, 23, 59, 59, 20, 26, 11, 12, 23, 45, 0,
         9999, 4095, 65535, 65535, 90000, 4095, 100, 900, 65535, 65535, 90000, 4095, 900,
         'G', 'U', 'b', 9999, 9999, 'C', 99999, 2, 'K', 543, 999, 1]
def vals(kind):
    assert len(WORST) == len(convs), f'{len(convs)} conversions in $D: update WORST in this test'
    out = []
    for c, w in zip(convs, WORST):
        if c == 'c':
            out.append(f"'{w}'")
        else:
            out.append(str(w) if kind == 'worst' else '7')
    return ', '.join(out)

src = '#include <stdio.h>\n#include <string.h>\n#include <stdint.h>\n#include <stdbool.h>\n' + hexv + '\n' + check + f'''
int main(void)
{{
    int fails = 0;
    const char *name;
    name = "typical";    {frame_c(vals("typical"))}
    name = "worst-case"; {frame_c(vals("worst"))}
    printf(fails ? "FAILED\\n" : "ALL PASS\\n");
    return fails != 0;
}}
'''
with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, 'frame_test.c')
    exe = os.path.join(d, 'frame_test')
    open(c, 'w').write(src)
    subprocess.run(['cc', '-std=gnu11', '-w', '-o', exe, c], check=True)
    sys.exit(subprocess.run([exe]).returncode)
