// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
#include <mbedtls/entropy_poll.h>

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
  (void)data;
  randomGet(output, len);
  *olen = len;
  return 0;
}
