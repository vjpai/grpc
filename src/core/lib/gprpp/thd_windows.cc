/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* Windows implementation for gpr threads. */

#include <grpc/support/port_platform.h>

#ifdef GPR_WINDOWS

#include "src/core/lib/gprpp/thd.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/thd_id.h>
#include <string.h>

#if defined(_MSC_VER)
#define thread_local __declspec(thread)
#define WIN_LAMBDA
#elif defined(__GNUC__)
#define thread_local __thread
#define WIN_LAMBDA WINAPI
#else
#error "Unknown compiler - please file a bug report"
#endif

namespace {
struct thd_info {
  grpc_core::Thread* thread;
  void (*body)(void* arg); /* body of a thread */
  void* arg;               /* argument to a thread */
  HANDLE join_event;       /* the join event */
};

thread_local struct thd_info* g_thd_info;

/* Destroys a thread info */
void destroy_thread(struct thd_info* t) {
  CloseHandle(t->join_event);
  gpr_free(t);
}
}  // namespace

namespace grpc_core {

void Thread::Init() {}

bool Thread::AwaitAll(gpr_timespec deadline) {
  // TODO: Consider adding this if needed
  return false;
}

Thread::Thread(const char* thd_name, void (*thd_body)(void* arg), void* arg,
               bool* success)
    : real_(true), alive_(false), started_(false), joined_(false) {
  gpr_mu_init(&mu_);
  gpr_cv_init(&ready_);

  HANDLE handle;
  struct thd_info* info = (struct thd_info*)gpr_malloc(sizeof(*info));
  info->thread = this;
  info->body = thd_body;
  info->arg = arg;

  info->join_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (info->join_event == nullptr) {
    gpr_free(info);
    alive_ = false;
  } else {
    handle = CreateThread(nullptr, 64 * 1024,
                          [](void* v) WIN_LAMBDA -> DWORD {
                            g_thd_info = static_cast<thd_info*>(v);
                            gpr_mu_lock(&g_thd_info->thread->mu_);
                            while (!g_thd_info->thread->started_) {
                              gpr_cv_wait(&g_thd_info->thread->ready_,
                                          &g_thd_info->thread->mu_,
                                          gpr_inf_future(GPR_CLOCK_MONOTONIC));
                            }
                            gpr_mu_unlock(&g_thd_info->thread->mu_);
                            g_thd_info->body(g_thd_info->arg);
                            BOOL ret = SetEvent(g_thd_info->join_event);
                            GPR_ASSERT(ret);
                            return 0;
                          },
                          info, 0, nullptr);
    if (handle == nullptr) {
      destroy_thread(info);
      alive_ = false;
    } else {
      id_ = (gpr_thd_id)info;
      CloseHandle(handle);
      alive_ = true;
    }
  }
  if (success != nullptr) {
    *success = alive_;
  }
}

Thread::~Thread() {
  if (!alive_) {
    // This thread never existed, so nothing to do
  } else {
    GPR_ASSERT(joined_);
  }
  if (real_) {
    gpr_mu_destroy(&mu_);
    gpr_cv_destroy(&ready_);
  }
}

void Thread::Start() {
  GPR_ASSERT(real_);
  gpr_mu_lock(&mu_);
  if (alive_) {
    started_ = true;
    gpr_cv_signal(&ready_);
  }
  gpr_mu_unlock(&mu_);
}

void Thread::Join() {
  if (alive_) {
    thd_info* info = (thd_info*)id_;
    DWORD ret = WaitForSingleObject(info->join_event, INFINITE);
    GPR_ASSERT(ret == WAIT_OBJECT_0);
    destroy_thread(info);
  }
  joined_ = true;
}
}  // namespace grpc_core

gpr_thd_id gpr_thd_currentid(void) { return (gpr_thd_id)g_thd_info; }

#endif /* GPR_WINDOWS */
