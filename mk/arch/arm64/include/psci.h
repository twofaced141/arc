/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 */

#ifndef PSCI_H
#define PSCI_H

#include <stdint.h>

#define PSCI_CPU_ON_64  0xC4000003ULL
#define PSCI_SUCCESS    0

int psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context_id);

#endif
