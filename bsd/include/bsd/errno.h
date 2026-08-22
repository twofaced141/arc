/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#ifndef BSD_ERRNO_H
#define BSD_ERRNO_H

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ETXTBSY 26
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32
#define EDOM    33
#define ERANGE  34

#define ENOSYS  38
#define ELOOP   40
#define EBUSY   16
#define ENAMETOOLONG 63
#define ENOTEMPTY 66

#define EWOULDBLOCK EAGAIN
#define ETIMEDOUT  110
#define EDEADLK    35
#define ENOTSOCK   88

/* Network / socket errors (Linux values for compatibility) */
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define EADDRNOTAVAIL 99
#define ENETDOWN     100
#define ENETUNREACH  101
#define ENETRESET    102
#define ECONNABORTED 103
#define ECONNRESET   104
#define ENOBUFS      105
#define EISCONN      106
#define ENOTCONN     107
#define ESHUTDOWN    108
#define ETOOMANYREFS 109
#define ETIMEDOUT_NET 110
#define ECONNREFUSED 111
#define EHOSTDOWN    112
#define EHOSTUNREACH 113
#define EALREADY     114
#define EINPROGRESS  115
#define ESTALE       116
#define EPROTONOSUPPORT 93
#define EPROTOTYPE   91
#define ENOPROTOOPT  92
#define EOPNOTSUPP   95
#define EPFNOSUPPORT 96
#define EDESTADDRREQ 89
#define EMSGSIZE     90

/* Internal pseudo-error: an interruptible syscall was blocked when a
 * signal arrived.  The dispatcher restarts the syscall if the delivered
 * signal has SA_RESTART, otherwise converts it to EINTR. */
#define ERESTARTSYS 512

extern int bsd_errno;

#endif
