# ARC
ARC is a hybrid kernel with a microkernel and BSD personality.

_Warning: i386 and arm64 support are experimental now. arm64 only works in QEMU virt._


_SMP support is incomplete on all architectures._

## Building

### Prerequisites
- A GCC/Clang cross-compiler for the target architecture
- GNU Make

### Build
```sh
make ARCH=amd64   # for amd64
make ARCH=i386    # for i386
make ARCH=arm64   # for arm64
```
