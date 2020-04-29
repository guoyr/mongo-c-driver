/*
 * Copyright 2013-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common-prelude.h"

#ifndef COMMON_THREAD_PRIVATE_H
#define COMMON_THREAD_PRIVATE_H

#define BSON_INSIDE
#include "bson/bson-compat.h"
#include "bson/bson-config.h"
#include "bson/bson-macros.h"
#undef BSON_INSIDE

BSON_BEGIN_DECLS

#if defined(BSON_OS_UNIX)
#include <pthread.h>
#define BSON_ONCE_FUN(n) void n (void)
#define BSON_ONCE_RETURN return
#define BSON_ONCE_INIT PTHREAD_ONCE_INIT
#define bson_mutex_destroy pthread_mutex_destroy
#define bson_mutex_init(_n) pthread_mutex_init ((_n), NULL)
#define bson_mutex_lock pthread_mutex_lock
#define bson_mutex_t pthread_mutex_t
#define bson_mutex_unlock pthread_mutex_unlock
#define bson_once pthread_once
#define bson_once_t pthread_once_t
#define bson_thread_create(_t, _f, _d) pthread_create ((_t), NULL, (_f), (_d))
#define bson_thread_join(_n) pthread_join ((_n), NULL)
#define bson_thread_t pthread_t
#define BSON_THREAD_FUN(_function_name, _arg_name) void * _function_name (void * _arg_name)
#else
#define BSON_ONCE_FUN(n) \
   BOOL CALLBACK n (PINIT_ONCE _ignored_a, PVOID _ignored_b, PVOID *_ignored_c)
#define BSON_ONCE_INIT INIT_ONCE_STATIC_INIT
#define BSON_ONCE_RETURN return true
#define bson_mutex_destroy DeleteCriticalSection
#define bson_mutex_init InitializeCriticalSection
#define bson_mutex_lock EnterCriticalSection
#define bson_mutex_t CRITICAL_SECTION
#define bson_mutex_unlock LeaveCriticalSection
#define bson_once(o, c) InitOnceExecuteOnce (o, c, NULL, NULL)
#define bson_once_t INIT_ONCE
#define bson_thread_create(_thread_handle, _function, _arg) (!(*(_thread_handle) = (HANDLE)_beginthreadex (NULL, 0, (_function), (_arg), 0, NULL)))
#define bson_thread_join(_n) do { WaitForSingleObject ((_n), INFINITE); CloseHandle ((_n)); } while (0)
#define bson_thread_t HANDLE
#define BSON_THREAD_FUN(_function_name, _arg_name) unsigned __stdcall _function_name(void * _arg_name)
#endif

BSON_END_DECLS

#endif /* COMMON_THREAD_PRIVATE_H */
