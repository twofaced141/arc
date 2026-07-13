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

printf "mkdir /bin\nwrite init.elf /bin/init\nwrite shell.elf /bin/shell\n
\nwrite cmd_pwd.elf /bin/pwd\nwrite cmd_getpid.elf /bin/getpid\nwrite cmd_clear.elf /bin/clear\nwrite cmd_ls.elf /bin/ls\nwrite cmd_kill.elf /bin/kill\nwrite cmd_cat.elf /bin/cat\nwrite cmd_date.elf /bin/date\nwrite cmd_sleep.elf /bin/sleep\nwrite cmd_uname.elf /bin/uname\nwrite cmd_stat.elf /bin/stat\nwrite cmd_mkdir.elf /bin/mkdir\nwrite cmd_rm.elf /bin/rm\nwrite cmd_echo.elf /bin/echo\nwrite cmd_touch.elf /bin/touch\nwrite cmd_cp.elf /bin/cp\nwrite cmd_mv.elf /bin/mv\nwrite cmd_ps.elf /bin/ps\nwrite cmd_whoami.elf /bin/whoami\nwrite cmd_id.elf /bin/id\nwrite cmd_true.elf /bin/true\nwrite cmd_false.elf /bin/false\nwrite cmd_yes.elf /bin/yes\nwrite cmd_basename.elf /bin/basename\nwrite cmd_dirname.elf /bin/dirname\nwrite cmd_wc.elf /bin/wc\nwrite cmd_seq.elf /bin/seq\nwrite cmd_head.elf /bin/head\nwrite cmd_which.elf /bin/which\nwrite cmd_test.elf /bin/test\nwrite cmd_cmp.elf /bin/cmp\nwrite cmd_hexdump.elf /bin/hexdump\nwrite cmd_hostname.elf /bin/hostname\nwrite cmd_ln.elf /bin/ln\nwrite cmd_chmod.elf /bin/chmod\nwrite cmd_free.elf /bin/free\nwrite cmd_poweroff.elf /bin/poweroff\nwrite cmd_reboot.elf /bin/reboot\nwrite cmd_curl.elf /bin/curl\nwrite cmd_chown.elf /bin/chown\nwrite cmd_socket_test.elf /bin/socket_test\nwrite cmd_chgrp.elf /bin/chgrp\nwrite cmd_bench.elf /bin/bench\nwrite wm.elf /bin/wm\n" \
  | debugfs -w "$OUT" 2>/dev/null

rm -f /tmp/kernel_hello.txt /tmp/kernel_nested.txt

echo "disk.img created: $OUT"
