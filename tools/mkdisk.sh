#!/bin/sh
# Build disk.img with ext2 filesystem for opencodeOS
# Run from project root: ./tools/mkdisk.sh

set -e

OUT="${1:-disk.img}"

dd if=/dev/zero of="$OUT" bs=1M count=16 2>/dev/null
mkfs.ext2 -b 1024 -F "$OUT" 2>/dev/null

printf "HELLO EXT2 WORLD." > /tmp/kernel_hello.txt
printf "NESTED FILE" > /tmp/kernel_nested.txt

printf "write /tmp/kernel_hello.txt hello.txt\nmkdir /subdir\nwrite /tmp/kernel_nested.txt /subdir/nested.txt\n" \
  | debugfs -w "$OUT" 2>/dev/null

printf "mkdir /bin\nwrite init.elf /bin/init\nwrite shell.elf /bin/shell\nwrite cmd_pwd.elf /bin/pwd\nwrite cmd_getpid.elf /bin/getpid\nwrite cmd_clear.elf /bin/clear\nwrite cmd_ls.elf /bin/ls\nwrite cmd_kill.elf /bin/kill\nwrite cmd_cat.elf /bin/cat\nwrite cmd_date.elf /bin/date\nwrite cmd_kmalloc_test.elf /bin/kmalloc_test\n" \
  | debugfs -w "$OUT" 2>/dev/null

rm -f /tmp/kernel_hello.txt /tmp/kernel_nested.txt

echo "disk.img created: $OUT"
