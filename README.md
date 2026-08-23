# ARC

[![CI](https://github.com/twofaced141/arc/actions/workflows/ci.yml/badge.svg)](https://github.com/twofaced141/arc/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C-blue.svg)](https://www.tiobe.com/tiobe-index/c/)
[![Architectures](https://img.shields.io/badge/arch-amd64%20%7C%20i386%20%7C%20arm64-informational.svg)](#building)

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
