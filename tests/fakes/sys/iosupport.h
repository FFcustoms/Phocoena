// Only the newlib fields used by Console.cpp; host-test include path only.
#pragma once
#include <cstddef>
#include <sys/types.h>
struct _reent {};
struct devoptab_t
{
  const char* name = nullptr;
  ssize_t (*write_r)(_reent*, void*, const char*, size_t) = nullptr;
};
inline constexpr int STD_OUT = 1, STD_ERR = 2;
extern const devoptab_t* devoptab_list[3];
