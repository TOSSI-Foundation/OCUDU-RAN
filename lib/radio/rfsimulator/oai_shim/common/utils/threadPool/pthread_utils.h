#pragma once
#include <pthread.h>
#include "common/utils/assertions.h"
#define mutexinit(m)   AssertFatal(pthread_mutex_init(&(m), NULL) == 0, "mutex init\n")
#define mutexlock(m)   AssertFatal(pthread_mutex_lock(&(m)) == 0, "mutex lock\n")
#define mutexunlock(m) AssertFatal(pthread_mutex_unlock(&(m)) == 0, "mutex unlock\n")
#define condinit(c)    AssertFatal(pthread_cond_init(&(c), NULL) == 0, "cond init\n")
#define condsignal(c)  AssertFatal(pthread_cond_signal(&(c)) == 0, "cond signal\n")
#define condbroadcast(c) AssertFatal(pthread_cond_broadcast(&(c)) == 0, "cond bcast\n")
#define condwait(c, m) AssertFatal(pthread_cond_wait(&(c), &(m)) == 0, "cond wait\n")
