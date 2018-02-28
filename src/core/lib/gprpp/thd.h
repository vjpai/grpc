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

#ifndef GRPC_CORE_LIB_GPRPP_THD_H
#define GRPC_CORE_LIB_GPRPP_THD_H

/** Internal thread interface. */

#include <grpc/support/port_platform.h>

#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/thd_id.h>
#include <grpc/support/time.h>

namespace grpc_core {

class Thread {
 public:
  /// Default constructor only to allow use in structs that lack constructors
  /// Does not produce a validly-constructed thread; must later
  /// use placement new to construct a real thread. Does not init mu_ and cv_
  Thread() : real_(false), alive_(false), started_(false), joined_(false) {}

  /// Normal constructor to create a thread with name \a thd_name,
  /// which will execute a thread based on function \a thd_body
  /// with argument \a arg once it is started.
  /// The optional \a success argument indicates whether the thread
  /// is successfully created.
  Thread(const char* thd_name, void (*thd_body)(void* arg), void* arg,
         bool* success = nullptr);

  /// Move constructor for thread
  Thread(Thread&&);

  /// Move assignment operator for thread
  Thread& operator=(Thread&&);

  ~Thread();

  void Start();
  void Join();

  static void Init();
  static bool AwaitAll(gpr_timespec deadline);

 private:
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  gpr_mu mu_;
  gpr_cv ready_;

  gpr_thd_id id_;

  // The following fields indicate state about the thread
  // Is it real (as opposed to being a dummy placeholder Thread?)
  bool real_;

  // Is it alive (is there a successful thread of control for this Thread)
  bool alive_;

  // Has the thread been started?
  bool started_;

  // Has the thread been joined?
  bool joined_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_LIB_GPRPP_THD_H */
