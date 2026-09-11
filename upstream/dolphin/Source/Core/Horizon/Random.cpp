// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/Random.h"
#include <switch.h>
namespace Common::Random
{
void Generate(void* buffer, std::size_t size) { randomGet(buffer, size); }
}
