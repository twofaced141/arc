#!/usr/bin/env python3
import sys

lines = sys.stdin.read().strip().splitlines()
print('#include "kallsyms.h"')
print()
print("const ksym_t kallsyms_table[] = {")
for line in lines:
    parts = line.split()
    if len(parts) >= 3:
        addr, typ, name = parts[0], parts[1], parts[2]
        if typ.isupper() and not typ == 'U':
            print(f'    {{0x{addr}, "{name}"}},')
print("};")
print()
print("const int kallsyms_count = sizeof(kallsyms_table) / sizeof(kallsyms_table[0]);")
