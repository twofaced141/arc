# bsd/net — сеть (lwIP)

`ARC` гибрид: `mk/` микроядро + `bsd/` personality. Сеть живет в `bsd/net/`,
но сам стек - вендорный `lwIP` (BSD license, совместим с `LICENSE` ARC `BSD-3-Clause`).

## Почему lwIP грязный и как мы чистим

Upstream `lwip-tcpip/lwip` (см. `tools/vendor_lwip.sh:fetch`) содержит в одном
репозитории то что не нужно ядру:

* `doc/doxygen/output/*.html` - сгенеренный doxygen
* `contrib/ports/win32/msvc/*.sln`, `*.vcxproj` - Visual Studio проекты
* `contrib/examples/httpd/examples_fs/*.html`, `src/apps/http/fs/*.html` - примеры
* `test/`, `contrib/Coverity/`, `CMake` артефакты

Если добавить весь репо через `BSD_DIRS += bsd/net/lwip` + `wildcard *.c` - в сборку
попадет мусор и `Makefile:96` соберет `makefsdata.c` как часть ядра.

Решение: **whitelist, а не wildcard**.

* `BSD_DIRS` содержит только `bsd/net` (наш код-обвязка: `socket.c`, `if.c`, `arch/*`)
* `lwIP` лежит в `bsd/net/lwip/` как вендор, но компилируются только файлы из
  `bsd/net/lwip/src/Filelists.mk` (`COREFILES` + `CORE4FILES` + `APIFILES` + `NETIFFILES`)
  через явный `LWIP_SRCS` в корневом `Makefile`. См. `Makefile:LWIP_SRCS`.
* Мусор физически не попадает в репо: `tools/vendor_lwip.sh` копирует только
  `src/{core,api,include,netif} + COPYING + src/Filelists.mk` и отбрасывает
  `doc/`, `contrib/`, `test/`, `*.html`, `*.sln`.

```
bsd/net/                <- наш BSD personality (сокеты/if)
  socket.c              <- sockfs vnode_ops VSOCK
  if.c                  <- ifnet список
  arch/cc.h             <- lwIP port: типы/byteorder
  arch/sys_arch.c       <- NO_SYS=1 заглушки
  lwip/                 <- вендор lwIP (только src/)
    src/core/*.c
    src/api/*.c
    src/include/lwip/*.h
    src/netif/ethernet.c
    COPYING
  lwipopts.h            <- конфигурация lwIP для ARC
```

## Вендоринг

Не коммитим весь upstream. Для обновления:

```sh
tools/vendor_lwip.sh --rev master   # качает tarball и чистит
# или
tools/vendor_lwip.sh --rev STABLE-2_2_0 --pin
git add bsd/net/lwip bsd/net/lwipopts.h
```

Скрипт идемпотентен, удаляет `*.html`, `*.sln`, `*.vcxproj`, `doxygen/`, `contrib/`.

## Сборка

`Makefile` отдельно собирает `LWIP` с флагами:
* `-I bsd/net -I bsd/net/lwip/src/include -I bsd/net/lwip/src/include/lwip`
* `-I bsd/net/arch` для `cc.h`
* `-Wno-unused-parameter` - lwIP триггерит штатные варнинги

Проверка: `make ARCH=amd64` должен показать `lwip: X files` и не тянуть `doc/`.
