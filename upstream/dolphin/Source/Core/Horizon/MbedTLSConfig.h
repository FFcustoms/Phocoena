// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Dolphin does not use mbedTLS's Unix signal/alarm based timer module.
#undef MBEDTLS_TIMING_C
// Supply real entropy from libnx instead of assuming /dev/urandom exists.
#define MBEDTLS_ENTROPY_HARDWARE_ALT
