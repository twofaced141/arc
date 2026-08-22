# bsd/net — networking (lwIP)

Networking lives in
`bsd/net/`, but the stack itself is vendored `lwIP`.

## Why lwIP is noisy and how we clean it

Upstream `lwip-tcpip/lwip` (see `tools/vendor_lwip.sh:fetch`) bundles a lot
that the kernel does not need in a single repository:

* `doc/doxygen/output/*.html` — generated doxygen
* `contrib/ports/win32/msvc/*.sln`, `*.vcxproj` — Visual Studio projects
* `contrib/examples/httpd/examples_fs/*.html`, `src/apps/http/fs/*.html` — examples
* `test/`, `contrib/Coverity/`, `CMake` artifacts

Adding the whole repo via `BSD_DIRS += bsd/net/lwip` + `wildcard *.c` would
pull clutter into the build and `Makefile:96` would compile `makefsdata.c` as
part of the kernel.

Solution: **whitelist, not wildcard**.

* `BSD_DIRS` contains only `bsd/net` (our wrapper: `socket.c`, `if.c`, `arch/*`)
* `lwIP` lives in `bsd/net/lwip/` as a vendor, but only files from
  `bsd/net/lwip/src/Filelists.mk` (`COREFILES` + `CORE4FILES` + `APIFILES` +
  `NETIFFILES`) are built via explicit `LWIP_SRCS` in the top-level `Makefile`.
  See `Makefile:LWIP_SRCS`.
* Clutter never enters the repo: `tools/vendor_lwip.sh` copies only
  `src/{core,api,include,netif} + COPYING + src/Filelists.mk` and drops
  `doc/`, `contrib/`, `test/`, `*.html`, `*.sln`.

```
bsd/net/                <- BSD personality (sockets/if)
  socket.c              <- sockfs vnode_ops VSOCK
  if.c                  <- ifnet list
  arch/cc.h             <- lwIP port: types/byteorder
  arch/sys_arch.c       <- NO_SYS=1 stubs
  lwip/                 <- vendored lwIP (src/ only)
    src/core/*.c
    src/api/*.c
    src/include/lwip/*.h
    src/netif/ethernet.c
    COPYING
  lwipopts.h            <- lwIP config for ARC
```

## Vendoring

Do not commit the full upstream. To update:

```sh
tools/vendor_lwip.sh --rev master   # download tarball and clean
# or
tools/vendor_lwip.sh --rev STABLE-2_2_0 --pin
git add bsd/net/lwip bsd/net/lwipopts.h
```

The script is idempotent and removes `*.html`, `*.sln`, `*.vcxproj`,
`doxygen/`, `contrib/`.

## Build

`Makefile` builds `LWIP` separately with:

* `-I bsd/net -I bsd/net/lwip/src/include -I bsd/net/lwip/src/include/lwip`
* `-I bsd/net/arch` for `cc.h`
* `-Wno-unused-parameter` — lwIP triggers benign warnings

Check: `make ARCH=amd64` should show `lwip: X files` and not pull `doc/`.
