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

#include "test/cpp/microbenchmarks/fullstack_unary_sync_ping_pong.h"
#include "test/cpp/util/test_config.h"

namespace grpc {
namespace testing {

// force library initialization
auto& force_library_initialization = Library::get();

/*******************************************************************************
 * CONFIGURATIONS
 */

static void SweepSizesArgs(benchmark::internal::Benchmark* b) {
  b->Args({0, 0});
  for (int i = 1; i <= 128 * 1024 * 1024; i *= 8) {
    b->Args({i, i});
  }
}

BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, TCP, NoOpMutator)
    ->Apply(SweepSizesArgs);
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, MinTCP, NoOpMutator)
    ->Apply(SweepSizesArgs);
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, UDS, NoOpMutator)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, MinUDS, NoOpMutator)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess, NoOpMutator)
    ->Apply(SweepSizesArgs);
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, MinInProcess, NoOpMutator)
    ->Apply(SweepSizesArgs);
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, SockPair, NoOpMutator)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, MinSockPair, NoOpMutator)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2, NoOpMutator)
    ->Apply(SweepSizesArgs);
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, MinInProcessCHTTP2, NoOpMutator
                   )
    ->Apply(SweepSizesArgs);
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomBinaryMetadata<10>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomBinaryMetadata<31>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomBinaryMetadata<100>, 1>
                   )
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomBinaryMetadata<10>, 2>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomBinaryMetadata<31>, 2>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomBinaryMetadata<100>, 2>
                   )
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomAsciiMetadata<10>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomAsciiMetadata<31>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcessCHTTP2,
                   Client_AddMetadata<RandomAsciiMetadata<100>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomBinaryMetadata<10>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomBinaryMetadata<31>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomBinaryMetadata<100>, 1>
                   )
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomBinaryMetadata<10>, 2>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomBinaryMetadata<31>, 2>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomBinaryMetadata<100>, 2>
                   )
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomAsciiMetadata<10>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomAsciiMetadata<31>, 1>)
    ->Args({0, 0});
BENCHMARK_TEMPLATE(BM_UnarySyncPingPong, InProcess,
                   Client_AddMetadata<RandomAsciiMetadata<100>, 1>)
    ->Args({0, 0});

}  // namespace testing
}  // namespace grpc

// Some distros have RunSpecifiedBenchmarks under the benchmark namespace,
// and others do not. This allows us to support both modes.
namespace benchmark {
void RunTheBenchmarksNamespaced() { RunSpecifiedBenchmarks(); }
}  // namespace benchmark

int main(int argc, char** argv) {
  ::benchmark::Initialize(&argc, argv);
  ::grpc::testing::InitTest(&argc, &argv, false);
  benchmark::RunTheBenchmarksNamespaced();
  return 0;
}
