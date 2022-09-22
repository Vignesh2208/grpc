// Copyright 2022 gRPC Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/core/lib/event_engine/posix_engine/posix_endpoint.h"

#include <fcntl.h>
#include <poll.h>

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "absl/strings/str_cat.h"

#include <grpc/event_engine/event_engine.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/event_engine/channel_args_endpoint_config.h"
#include "src/core/lib/event_engine/posix_engine/event_poller.h"
#include "src/core/lib/event_engine/posix_engine/event_poller_posix_default.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine_closure.h"
#include "src/core/lib/event_engine/posix_engine/tcp_socket_utils.h"
#include "src/core/lib/gprpp/dual_ref_counted.h"
#include "src/core/lib/gprpp/global_config.h"
#include "src/core/lib/gprpp/notification.h"
#include "src/core/lib/iomgr/port.h"
#include "test/core/event_engine/posix/posix_engine_test_utils.h"
#include "test/core/event_engine/test_suite/event_engine_test_utils.h"
#include "test/core/event_engine/test_suite/oracle_event_engine_posix.h"
#include "test/core/util/port.h"

GPR_GLOBAL_CONFIG_DECLARE_STRING(grpc_poll_strategy);

namespace grpc_event_engine {
namespace posix_engine {

namespace {

using ::grpc_event_engine::experimental::ChannelArgsEndpointConfig;
using ::grpc_event_engine::experimental::EventEngine;
using ::grpc_event_engine::experimental::GetNextSendMessage;
using ::grpc_event_engine::experimental::Poller;
using ::grpc_event_engine::experimental::PosixEventEngine;
using ::grpc_event_engine::experimental::PosixOracleEventEngine;
using ::grpc_event_engine::experimental::URIToResolvedAddress;
using ::grpc_event_engine::experimental::WaitForPendingTasks;
using Endpoint = ::grpc_event_engine::experimental::EventEngine::Endpoint;
using Listener = ::grpc_event_engine::experimental::EventEngine::Listener;
using namespace std::chrono_literals;

constexpr int kMinMessageSize = 1024;
constexpr int kNumConnections = 10;
constexpr int kNumExchangedMessages = 100;
std::atomic<int> g_num_active_connections{0};

EventEngine* GetOracleEE() {
  static EventEngine* oracle_ee = new PosixOracleEventEngine();
  return oracle_ee;
}

std::list<std::tuple<std::unique_ptr<EventEngine::Endpoint>,
                     std::unique_ptr<EventEngine::Endpoint>>>
CreateConnectedEndpoints(PosixEventPoller* poller, bool is_zero_copy_enabled,
                         int num_connections,
                         std::shared_ptr<EventEngine> posix_ee) {
  EXPECT_NE(GetOracleEE(), nullptr);
  EXPECT_NE(poller, nullptr);
  std::list<std::tuple<std::unique_ptr<EventEngine::Endpoint>,
                       std::unique_ptr<EventEngine::Endpoint>>>
      connections;
  auto memory_quota = absl::make_unique<grpc_core::MemoryQuota>("bar");
  std::string target_addr = absl::StrCat(
      "ipv6:[::1]:", std::to_string(grpc_pick_unused_port_or_die()));
  EventEngine::ResolvedAddress resolved_addr =
      URIToResolvedAddress(target_addr);
  std::unique_ptr<EventEngine::Endpoint> server_endpoint;
  grpc_core::Notification* server_signal = new grpc_core::Notification();

  Listener::AcceptCallback accept_cb =
      [&server_endpoint, &server_signal](
          std::unique_ptr<Endpoint> ep,
          grpc_core::MemoryAllocator /*memory_allocator*/) {
        server_endpoint = std::move(ep);
        server_signal->Notify();
      };
  grpc_core::ChannelArgs args;
  auto quota = grpc_core::ResourceQuota::Default();
  args = args.Set(GRPC_ARG_RESOURCE_QUOTA, quota);
  if (is_zero_copy_enabled) {
    args = args.Set(GRPC_ARG_TCP_TX_ZEROCOPY_ENABLED, 1);
    args = args.Set(GRPC_ARG_TCP_TX_ZEROCOPY_SEND_BYTES_THRESHOLD,
                    kMinMessageSize);
  }
  ChannelArgsEndpointConfig config(args);
  auto listener = GetOracleEE()->CreateListener(
      std::move(accept_cb),
      [](absl::Status status) { ASSERT_TRUE(status.ok()); }, config,
      absl::make_unique<grpc_core::MemoryQuota>("foo"));
  GPR_ASSERT(listener.ok());

  EXPECT_TRUE((*listener)->Bind(resolved_addr).ok());
  EXPECT_TRUE((*listener)->Start().ok());

  // Create client socket and connect to the target address.
  for (int i = 0; i < num_connections; ++i) {
    int client_fd = ConnectToServerOrDie(resolved_addr);
    EventHandle* handle =
        poller->CreateHandle(client_fd, "test", poller->CanTrackErrors());
    EXPECT_NE(handle, nullptr);
    server_signal->WaitForNotification();
    EXPECT_NE(server_endpoint, nullptr);
    ++g_num_active_connections;
    connections.push_back(
        std::make_tuple<std::unique_ptr<Endpoint>, std::unique_ptr<Endpoint>>(
            CreatePosixEndpoint(handle,
                                PosixEngineClosure::TestOnlyToClosure(
                                    [poller](absl::Status /*status*/) {
                                      if (--g_num_active_connections == 0) {
                                        poller->Kick();
                                      }
                                    }),
                                posix_ee, config),
            std::move(server_endpoint)));
    delete server_signal;
    server_signal = new grpc_core::Notification();
  }
  delete server_signal;
  return connections;
}

}  // namespace

class TestParam {
 public:
  TestParam(std::string poller, bool is_zero_copy_enabled)
      : poller_(std::move(poller)),
        is_zero_copy_enabled_(is_zero_copy_enabled) {}

  std::string Poller() const { return poller_; }
  bool IsZeroCopyEnabled() const { return is_zero_copy_enabled_; }

 private:
  std::string poller_;
  bool is_zero_copy_enabled_;
};

std::string TestScenarioName(const ::testing::TestParamInfo<TestParam>& info) {
  return absl::StrCat("poller_type_", info.param.Poller(),
                      "_is_zero_copy_enabled_", info.param.IsZeroCopyEnabled());
}

// A helper class to drive the polling of Fds. It repeatedly calls the Work(..)
// method on the poller to get pet pending events, then schedules another
// parallel Work(..) instantiation and processes these pending events. This
// continues until all Fds have orphaned themselves.
class Worker : public grpc_core::DualRefCounted<Worker> {
 public:
  Worker(std::shared_ptr<EventEngine> engine, PosixEventPoller* poller)
      : engine_(std::move(engine)), poller_(poller) {
    WeakRef().release();
  }
  void Orphan() override { signal.Notify(); }
  void Start() {
    // Start executing Work(..).
    engine_->Run([this]() { Work(); });
  }

  void Wait() {
    signal.WaitForNotification();
    WeakUnref();
  }

 private:
  void Work() {
    auto result = poller_->Work(24h, [this]() {
      // Schedule next work instantiation immediately and take a Ref for
      // the next instantiation.
      Ref().release();
      engine_->Run([this]() { Work(); });
    });
    ASSERT_TRUE(result == Poller::WorkResult::kOk ||
                result == Poller::WorkResult::kKicked);
    // Corresponds to the Ref taken for the current instantiation. If the
    // result was Poller::WorkResult::kKicked, then the next work instantiation
    // would not have been scheduled and the poll_again callback would have
    // been deleted.
    Unref();
  }
  std::shared_ptr<EventEngine> engine_;
  PosixEventPoller* poller_;
  grpc_core::Notification signal;
};

class PosixEndpointTest : public ::testing::TestWithParam<TestParam> {
  void SetUp() override {
    posix_ee_ = std::make_shared<PosixEventEngine>();
    scheduler_ =
        absl::make_unique<grpc_event_engine::posix_engine::TestScheduler>(
            posix_ee_.get());
    EXPECT_NE(scheduler_, nullptr);
    GPR_GLOBAL_CONFIG_SET(grpc_poll_strategy, GetParam().Poller().c_str());
    poller_ = GetDefaultPoller(scheduler_.get());
    if (poller_ != nullptr) {
      EXPECT_EQ(poller_->Name(), GetParam().Poller());
    }
  }

  void TearDown() override {
    if (poller_ != nullptr) {
      poller_->Shutdown();
    }
    WaitForPendingTasks(std::move(posix_ee_));
  }

 public:
  TestScheduler* Scheduler() { return scheduler_.get(); }

  std::shared_ptr<EventEngine> GetPosixEE() { return posix_ee_; }

  PosixEventPoller* PosixPoller() { return poller_; }

 private:
  PosixEventPoller* poller_;
  std::unique_ptr<TestScheduler> scheduler_;
  std::shared_ptr<EventEngine> posix_ee_;
};

TEST_P(PosixEndpointTest, ConnectExchangeBidiDataTransferTest) {
  if (PosixPoller() == nullptr) {
    return;
  }
  Worker* worker = new Worker(GetPosixEE(), PosixPoller());
  worker->Start();
  {
    auto connections = CreateConnectedEndpoints(
        PosixPoller(), GetParam().IsZeroCopyEnabled(), 1, GetPosixEE());
    auto it = connections.begin();
    auto client_endpoint = std::move(std::get<0>(*it));
    auto server_endpoint = std::move(std::get<1>(*it));
    EXPECT_TRUE(client_endpoint != nullptr);
    EXPECT_TRUE(server_endpoint != nullptr);
    connections.erase(it);

    // Alternate message exchanges between client -- server and server --
    // client.
    for (int i = 0; i < kNumExchangedMessages; i++) {
      // Send from client to server and verify data read at the server.
      ASSERT_TRUE(SendValidatePayload(GetNextSendMessage(),
                                      client_endpoint.get(),
                                      server_endpoint.get())
                      .ok());
      // Send from server to client and verify data read at the client.
      ASSERT_TRUE(SendValidatePayload(GetNextSendMessage(),
                                      server_endpoint.get(),
                                      client_endpoint.get())
                      .ok());
    }
  }
  worker->Wait();
}

// Create  N connections and exchange and verify random number of messages over
// each connection in parallel.
TEST_P(PosixEndpointTest, MultipleIPv6ConnectionsToOneOracleListenerTest) {
  if (PosixPoller() == nullptr) {
    return;
  }
  Worker* worker = new Worker(GetPosixEE(), PosixPoller());
  worker->Start();
  auto connections =
      CreateConnectedEndpoints(PosixPoller(), GetParam().IsZeroCopyEnabled(),
                               kNumConnections, GetPosixEE());
  std::vector<std::thread> threads;
  // Create one thread for each connection. For each connection, create
  // 2 more worker threads: to exchange and verify bi-directional data transfer.
  threads.reserve(kNumConnections);
  for (int i = 0; i < kNumConnections; i++) {
    // For each connection, simulate a parallel bi-directional data transfer.
    // All bi-directional transfers are run in parallel across all connections.
    auto it = connections.begin();
    auto client_endpoint = std::move(std::get<0>(*it));
    auto server_endpoint = std::move(std::get<1>(*it));
    EXPECT_TRUE(client_endpoint != nullptr);
    EXPECT_TRUE(server_endpoint != nullptr);
    connections.erase(it);
    threads.emplace_back([client_endpoint = std::move(client_endpoint),
                          server_endpoint = std::move(server_endpoint)]() {
      std::vector<std::thread> workers;
      workers.reserve(2);
      auto worker = [client_endpoint = client_endpoint.get(),
                     server_endpoint =
                         server_endpoint.get()](bool client_to_server) {
        for (int i = 0; i < kNumExchangedMessages; i++) {
          // If client_to_server is true, send from client to server and
          // verify data read at the server. Otherwise send data from server
          // to client and verify data read at client.
          if (client_to_server) {
            EXPECT_TRUE(SendValidatePayload(GetNextSendMessage(),
                                            client_endpoint, server_endpoint)
                            .ok());
          } else {
            EXPECT_TRUE(SendValidatePayload(GetNextSendMessage(),
                                            server_endpoint, client_endpoint)
                            .ok());
          }
        }
      };
      // worker[0] simulates a flow from client to server endpoint
      workers.emplace_back([&worker]() { worker(true); });
      // worker[1] simulates a flow from server to client endpoint
      workers.emplace_back([&worker]() { worker(false); });
      workers[0].join();
      workers[1].join();
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  worker->Wait();
}

INSTANTIATE_TEST_SUITE_P(
    PosixEndpoint, PosixEndpointTest,
    ::testing::ValuesIn({TestParam(std::string("epoll1"), false),
                         TestParam(std::string("epoll1"), true),
                         TestParam(std::string("poll"), false),
                         TestParam(std::string("poll"), true)}),
    &TestScenarioName);

}  // namespace posix_engine
}  // namespace grpc_event_engine

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
