# План: довести BSD-прослойку до POSIX 7/10

Сводка оценки (4/10): широкое покрытие API, но семантика неконформна —
заглушки и системные нарушения.

## Критичный блок (сначала)

- [x] **1. brk/sbrk** — `bsd/sys/sys_process.c:113-136`. Расширять `proc_t.heap_end`
      от `USER_HEAP_START` до `USER_MMAP_START` (маппинг страниц VMM_USER|VMM_WRITABLE,
      zero-fill новых страниц, unmap+free при сжатии, откат при ENOMEM).
      Семантика: `brk(0)` = запрос; при ошибке break не меняется; `sbrk` возвращает
      старый break, при ошибке -1. Основа malloc().
      СДЕЛАНО 2026-08-05: реализовано, собирается на amd64/i386/arm64.
      СДЕЛАНО 2026-08-05: runtime smoke-тест в user/init/init.c — 11/11 OK в QEMU
      (grow/shrink, write/readback, лимит USER_MMAP_START, rollback при ENOMEM).
- [x] **2. Сброс сигнального состояния в exec** — `bsd/proc/exec.c`. При успешном
      exec: handler'ы → SIG_DFL (кроме SIG_IGN, которые остаются), sa_mask/sa_flags → 0,
      pending → 0, exit_sig → 0, снос sigaltstack. POSIX 2.9.1.
      ДОП: сбросить heap_end → USER_HEAP_START и mmap_next (новая программа без кучи).
      СДЕЛАНО 2026-08-05: реализовано, собирается на amd64/i386/arm64.
      СДЕЛАНО 2026-08-05: runtime-тест (user/tests/sigexec + test_exec_signals в init.c):
      A — handler сброшен в SIG_DFL (ребёнок умер по SIGUSR1), B — SIG_IGN пережил exec
      (exit 0). PASSED в QEMU.
      ПОПУТНО исправлено (bsd/signal/signal.c, bsd/sys/sys_signal.c):
      после proc_exit для TERM/CORE-сигналов и SIGKILL теперь thread_exit — мёртвый
      процесс больше не iret-ится обратно в user (был page fault + panic).
      ПОПУТНО исправлен баг COW-fork (mk/arch/amd64+vmm.c, i386): страницы, помеченные
      COW ранним fork, пропускались при копировании в нового ребёнка — ребёнок получал
      supervisor-identity huge page вместо своего текста и падал с #PF (err=0x5) на
      первом же шаге. Теперь COW-страницы разделяются (стандартная семантика COW);
      добавлен invlpg при понижении прав родителя; user-фаут теперь делает
      proc_exit+thread_exit вместо повторного входа в user (refault → panic).
- [x] **3. sigreturn: восстановить saved_mask** — `bsd/sys/sys_signal.c`
      (sigframe_t.saved_mask уже есть, не применяется). Дописать arm64-ветку
      (frame-укладка в signal.c отсутствует).
      СДЕЛАНО 2026-08-05: sigframe_t трёхархный (+saved_mask, arm64: saved_x0/lr/spsr/
      elr/sp); sigstate_t + sa_restorer[NSIG] и sigframe_addr; доставка в signal.c для
      всех трёх архов (frame + блокировка sig|sa_mask с учётом SA_NODEFER, layout:
      restorer ровно на entry-rsp хендлера, frame ниже; arm64: x0=sig, lr=restorer);
      sys_sigaction хранит/возвращает sa_restorer; sys_sigreturn снимает frame по
      sigframe_addr и восстанавливает контекст и маску. Собирается на всех архах.
      Runtime-тест (test_sigreturn в init.c): PASSED в QEMU amd64.
      ПОПУТНО исправлено: amd64-доставка не передавала номер сигнала хендлеру
      (rdi не ставился — хендлер видел pid=1 из syscall и молча возвращался);
      i386-доставка не клала аргумент на стек (cdecl); sys_sigprocmask отдавал
      blocked[0] вместо упакованной маски (тесты маски всегда проходили ложно).
- [x] **4. Права в kill** — `bsd/sys/sys_signal.c`. EPERM если
      target->uid/euid != p->euid и p->euid != 0. Разделить pid==0 (группа) и
      pid==-1 (все).
      СДЕЛАНО 2026-08-05: kill_permitted (self всегда; euid==0; euid == target
      uid/euid); pid>0 — одиночная цель, pid==0 — своя группа (p->pgrp), pid==-1 —
      все процессы, pid<-1 — группа -pid (proc_collect_kill_targets в proc.c,
      сбор pid под proc_lock, доставка без блокировок); sig==0 — только проверка
      существования/прав; -ESRCH если целей нет, -EPERM если ни одна не разрешена;
      починен SIGKILL-на-чужой процесс: раньше proc_exit(9) убивал отправителя
      (теперь отметка + доставка на следующем входе в ядро).
      Runtime-тест (test_kill_perms в init.c): PASSED в QEMU amd64 (root→self 0;
      nonroot→init EPERM; kill(0,0)/kill(-1,0)→0; bogus и reaped pid→ESRCH).
      ПОПУТНО исправлен латентный double-free (mk/arch/amd64/vm/vmm.c, i386):
      vmm_free_directory освобождал COW-страницы, разделяемые с родителем —
      при выходе ребёнка физическая страница текста родителя уходила в пул и
      перевыделялась под новые данные → исполнение мусора в тексте родителя
      (#PF по 0x0/0x2, один и тот же rip у обоих процессов). Теперь COW-страницы
      при teardown не освобождаются (arm64 не затронут — там общие таблицы).

## Средний блок

- [x] **5. Подключить tty ioctl** — `bsd/sys/sys_file.c`: sys_ioctl → vfs_ioctl
      → vnode ops (tty уже имеет TIOCGETA/SETA/GWINSZ/GPGRP).
      СДЕЛАНО 2026-08-05: sys_ioctl → vfs_ioctl (fd→vnode→ops->ioctl, -EBADF/-ENOTTY);
      dev_console_ops.ioctl → tty_ioctl; termios_t (ISIG/ICANON/ECHO, NCCS 20) +
      struct winsize добавлены в tty.h; tty хранит term и winsize (25x80 по умолчанию),
      TIOCGETA/SETA реально копируют (SETA синхронизирует echo/icanon/isig),
      TIOCGWINSZ отдаёт размеры.
      Runtime-тест (test_tty_ioctl в init.c): PASSED в QEMU amd64 (GPGRP/SPGRP
      round-trip, GWINSZ, GETA/SETA, ENOTTY на неизвестный cmd, EBADF на bad fd).
      ПОПУТНО исправлен баг vfs_copy_path: проверка «путь слишком длинный»
      читала kpath[254] — мусор стека для коротких путей → ЛЮБОЙ user open()
      возвращал -ENAMETOOLONG (раньше не проявлялся: fds 0-2 привязаны
      напрямую, execve использует свой путь). Теперь ищется NUL в окне 255 байт,
      при отсутствии — проверка байта 255.
- [x] **6. chdir/getcwd** — `bsd/sys/sys_process.c`: sys_chdir (vfs_copy_path →
      vfs_lookup → VDIR + X_OK проверки → p->cwd), sys_getcwd (копия, -ERANGE).
      Пути копируются как absolute-нормализованные (vfs_build_abs_path: cwd-префикс,
      «/»-схлопывание, «.», «..») — затронуты все vfs_copy_path-вызовы (open/mkdir/
      readlink/rename/unlink/...) и execve (относительные пути). cwd копируется
      в fork. Тест: 8/8 PASSED.
 - [x] **7. nanosleep** — сон через waitq + таймаут clockevent'а, заполнять rem при
       -EINTR.
       СДЕЛАНО 2026-08-07: scheduler_sleep_ticks (amd64) возвращает 1 (заблокирован)
       / 0 (дедлайн прошёл) / -1 (ошибка); waitq_sleep_timeout (bsd/proc/proc.c)
       вызывает thread_yield() только при 1, при -1 — poll-цикл по дедлайну;
       sys_nanosleep заполняет rem при -EINTR.
       Runtime-тест (test_nanosleep в init.c): PASSED в QEMU amd64 (EINVAL-проверки,
       zero sleep, 100ms, 2s elapsed ~2s, сигнал прерывает сон с -EINTR + rem).
       ПОПУТНО исправлен латентный баг thread_yield (mk/arch/amd64/intr/interrupts.s
       .yield_resume): IRETQ в 64-битном режиме проматывает RSP/SS-слоты и при
       same-ring (40 байт), а код добавлял ещё 16 → `ret` уходил в мусор и поток
       зависал после первого реального сна (путь resume'а yield никогда не
       исполнялся — в старом коде thread_yield был закомментирован). Теперь
       `.yield_resume` — просто `ret`.
       ПОПУТНО исправлена гонка в tick-пути scheduler_switch (int 32): kernel_rsp
       не сохранялся, если тик попадал между scheduler_sleep_ticks (THREAD_BLOCKED)
       и thread_yield → пробуждение шло на устаревший syscall-кадр и syscall не
       завершался (флаки «FAIL 100ms sleep»). Теперь кадр сохраняется для любого
       потока, преемпт — только для RUNNING.
       ПОПУТНО исправлен сигнальный EINTR (bsd/signal/signal.c, bsd/sys/sys_signal.c,
       bsd/sys/syscall.c, bsd/include/bsd/signal.h): результат syscall'а публиковался
       в rax уже после signal_check_pending, sigframe не сохранял rax, а
       mk_syscall_handler затирал восстановленный sigreturn'ом rax нулём —
       прерванный сигналом nanosleep возвращал в user 0 вместо -EINTR. Теперь
       результат публикуется в кадр до проверки сигналов, sigframe_t хранит
       saved_rax (x86_64) / saved_x0 (arm64), sigreturn возвращает его в кадр.
- [x] **8. select: timeout без busy-poll** — 5-й аргумент (syscall6-обёртка или
       упакованная структура), сон на waitq с таймаутом вместо thread_yield.
       СДЕЛАНО 2026-08-07:
       - sys_select (bsd/sys/sys_file.c): nfds/EINVAL-валидация, copy_from_user/
         copy_to_user fd_set; ARG5 = timeout (struct timeval, NULL = блокировать
         бесконечно, {0,0} = опрос без блокировки); deadline = now + sec*100 +
         usec/10000 (100 Гц).
       - NULL-таймаут: deadline now + 0x7FFFFFFF — НЕ now+0xFFFFFF00: scheduler
         хранит sleep_until как uint32 и будит при (int32)(now-dl)>=0, wrap
         давал немедленное пробуждение (~10 мс).
       - Сон на waitq дескриптора (vnode_ops.poll_waitq → pipe rq/wq, выбирается
         первым watch-fd через vfs_fd_poll_waitq, fallback p->waitq) + scheduler
         sleep_queue (sleep_wake_tick), ранний wake от pipe_write; сигнал будит
         через proc_wakeup → waitq_sleep_timeout возвращает -EINTR;
         select_update_timeout пишет остаток в user timeval (POSIX).
       - Рабочие копии fd_set пересобираются каждую итерацию цикла (иначе после
         sleep+re-poll watch-fd терялся из-за FD_CLR → blocking select вис).
       - ТЕСТ user/init/init.c test_select: 6/6 OK (EINVAL, ready, 300ms timeout,
         {0,0}, blocking wake, EINTR+rem), select test: PASSED в QEMU.

## Добить до 7/10

- [ ] **9. O_EXCL, O_NONBLOCK** в vfs_open
- [ ] **10. mmap/munmap/mprotect** (vnode_ops.mmap уже задекларирован)
- [ ] **11. fcntl** (F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL)
- [ ] **12. waitpid WCONTINUED + killpg**

## Статус

Сделано: задачи 1-8 (brk/sbrk, exec-сигналы, sigreturn, права kill, tty ioctl, chdir/getcwd, nanosleep, select) + runtime-тесты в QEMU.
Следующая: задача 9 (O_EXCL, O_NONBLOCK в vfs_open).
