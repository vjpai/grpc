/*
 *
 * Copyright 2016 gRPC authors.
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

/* Benchmark gRPC end2end in various configurations */

#ifndef TEST_CPP_MICROBENCHMARKS_FULLSTACK_UNARY_SYNC_PING_PONG_H
#define TEST_CPP_MICROBENCHMARKS_FULLSTACK_UNARY_SYNC_PING_PONG_H

#include <benchmark/benchmark.h>
#include <sstream>
#include "src/core/lib/profiling/timers.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/cpp/microbenchmarks/fullstack_context_mutators.h"
#include "test/cpp/microbenchmarks/fullstack_fixtures.h"

namespace grpc {
namespace testing {

/*******************************************************************************
 * BENCHMARKING KERNELS
 */

template <class Fixture, class ClientContextMutator>
static void BM_UnarySyncPingPong(benchmark::State& state) {
  EchoTestService::Service service;
  std::unique_ptr<Fixture> fixture(new Fixture(&service));
  EchoRequest send_request;
  EchoResponse recv_response;
  if (state.range(0) > 0) {
    send_request.set_message(std::string(state.range(0), 'a'));
  }
  std::unique_ptr<EchoTestService::Stub> stub(
      EchoTestService::NewStub(fixture->channel()));
  while (state.KeepRunning()) {
    GPR_TIMER_SCOPE("BenchmarkCycle", 0);
    recv_response.Clear();
    ClientContext cli_ctx;
    ClientContextMutator cli_ctx_mut(&cli_ctx);
    GPR_ASSERT(stub->Echo(&cli_ctx, send_request, &recv_response).ok());
  }
  fixture->Finish(state);
  fixture.reset();
  state.SetBytesProcessed(state.range(0) * state.iterations() +
                          state.range(1) * state.iterations());
}
}  // namespace testing
}  // namespace grpc

#endif  // TEST_CPP_MICROBENCHMARKS_FULLSTACK_UNARY_PING_PONG_H
