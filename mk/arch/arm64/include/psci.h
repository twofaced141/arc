/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 */

#ifndef PSCI_H
#define PSCI_H

#include <stdint.h>

#define PSCI_CPU_ON_64  0xC4000003ULL
#define PSCI_VERSION    0x84000000ULL
#define PSCI_SUCCESS    0
#define PSCI_NOT_SUPPORTED  -1
#define PSCI_INVALID_PARAMS -2
#define PSCI_DENIED          -3
#define PSCI_ALREADY_ON      -4
#define PSCI_ON_PENDING      -5
#define PSCI_INTERNAL_FAILURE -6
#define PSCI_NOT_PRESENT     -7
#define PSCI_DISABLED        -8
#define PSCI_INVALID_ADDRESS -9

int psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context_id);
uint32_t psci_version(void);
const char *psci_strerror(int err);

#endif
