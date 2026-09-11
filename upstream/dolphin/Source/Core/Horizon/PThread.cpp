// SPDX-License-Identifier: GPL-2.0-or-later
#include <pthread.h>
#include "Horizon/Log.h"

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                     void* (*entry)(void*), void* argument)
{
  // libnx defaults to 128 KiB. Dolphin and shader compilers were written for
  // desktop stacks. Preserve explicitly supplied attributes (e.g. SDL's), but
  // give std::thread's default workers 2 MiB instead of changing core threading.
  if (attributes) return __real_pthread_create(thread, attributes, entry, argument);
  pthread_attr_t local;
  int rc = pthread_attr_init(&local);
  if (rc) return rc;
  rc = pthread_attr_setstacksize(&local, 2 * 1024 * 1024);
  if (!rc) rc = __real_pthread_create(thread, &local, entry, argument);
  pthread_attr_destroy(&local);
  if (rc) Horizon::Error("Thread", "pthread_create/attributes failed errno=%d", rc);
  return rc;
}
