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

#include <fcntl.h>
#include <poll.h>

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include <grpc/event_engine/event_engine.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/event_engine/channel_args_endpoint_config.h"
#include "src/core/lib/event_engine/posix_engine/posix_endpoint.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine.h"
#include "src/core/lib/event_engine/promise.h"
#include "src/core/lib/iomgr/port.h"
#include "test/core/event_engine/test_suite/event_engine_test_utils.h"
#include "test/core/event_engine/test_suite/oracle_event_engine_posix.h"
#include "test/core/util/port.h"

namespace grpc_event_engine {
namespace posix_engine {

using ::grpc_event_engine::experimental::ChannelArgsEndpointConfig;
using ::grpc_event_engine::experimental::EventEngine;
using ::grpc_event_engine::experimental::PosixEventEngine;
using ::grpc_event_engine::experimental::PosixOracleEventEngine;
using ::grpc_event_engine::experimental::Promise;
using ::grpc_event_engine::experimental::URIToResolvedAddress;
using namespace std::chrono_literals;

namespace {

// A global event engine pointer is used to allow for thread leaks at the end of
// a test. If a shared_ptr is directly used instead of a pointer to a
// shared_ptr, the destructor may end up running after all the tests finish
// result in tsan thread leak warnings.
std::shared_ptr<PosixEventEngine>* g_engine = []() {
  std::shared_ptr<PosixEventEngine>* engine =
      new std::shared_ptr<PosixEventEngine>;
  *engine = std::make_shared<PosixEventEngine>();
  return engine;
}();

// Creates a server socket listening for one connection on a specific port. It
// then creates another client socket connected to the server socket. This fills
// up the kernel listen queue on the server socket. Any subsequent attempts to
// connect to the server socket will hang indefinitely. This can be used to
// test Connection timeouts and cancellation attempts.
absl::StatusOr<std::vector<int>> CreateConnectedSockets(
    EventEngine::ResolvedAddress resolved_addr) {
  int server_socket;
  int opt = -1;
  int client_socket;
  int one = 1;
  int flags;
  std::vector<int> ret_sockets;
  // Creating a new socket file descriptor.
  if ((server_socket = socket(AF_INET6, SOCK_STREAM, 0)) <= 0) {
    return absl::UnknownError(
        absl::StrCat("Error creating socket: ", std::strerror(errno)));
  }
  // MacOS biulds fail if SO_REUSEADDR and SO_REUSEPORT are set in the same
  // setsockopt syscall. So they are set separately one after the other.
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    return absl::UnknownError(
        absl::StrCat("Error setsockopt(SO_REUSEADDR): ", std::strerror(errno)));
  }
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
    return absl::UnknownError(
        absl::StrCat("Error setsockopt(SO_REUSEPORT): ", std::strerror(errno)));
  }

  // Forcefully bind the new socket.
  if (bind(server_socket, resolved_addr.address(), resolved_addr.size()) < 0) {
    return absl::UnknownError(
        absl::StrCat("Error bind: ", std::strerror(errno)));
  }
  // Set the new socket to listen for one active connection at a time.
  // accept() is intentionally not called on the socket. This allows the
  // connection queue to build up.
  if (listen(server_socket, 1) < 0) {
    return absl::UnknownError(
        absl::StrCat("Error listen: ", std::strerror(errno)));
  }
  ret_sockets.push_back(server_socket);
  // Create and connect client sockets until the connection attempt times out.
  // Even if the backlog specified to listen is 1, the kernel continues to
  // accept a certain number of SYN packets before dropping them. This loop
  // attempts to identify the number of new connection attempts that will
  // be allowed by the kernel before any subsequent connection attempts
  // hang indefinitely.
  while (1) {
    client_socket = socket(AF_INET6, SOCK_STREAM, 0);
    setsockopt(client_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Make fd non-blocking.
    flags = fcntl(client_socket, F_GETFL, 0);
    EXPECT_EQ(fcntl(client_socket, F_SETFL, flags | O_NONBLOCK), 0);

    if (connect(client_socket,
                const_cast<struct sockaddr*>(resolved_addr.address()),
                resolved_addr.size()) == -1) {
      if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = client_socket;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, 1000);
        if (ret == -1) {
          gpr_log(GPR_ERROR, "poll() failed during connect; errno=%d", errno);
          abort();
        } else if (ret == 0) {
          // current connection attempt timed out. It indicates that the
          // kernel will cause any subsequent connection attempts to hang
          // indefintely.
          ret_sockets.push_back(client_socket);
          return ret_sockets;
        }
      } else {
        gpr_log(GPR_ERROR, "Failed to connect to the server (errno=%d)", errno);
        abort();
      }
    }
    ret_sockets.push_back(client_socket);
  }
  return ret_sockets;
}

}  // namespace

TEST(PosixEventEngineTest, IndefiniteConnectTimeoutOrRstTest) {
  std::string target_addr = absl::StrCat(
      "ipv6:[::1]:", std::to_string(grpc_pick_unused_port_or_die()));
  auto resolved_addr = URIToResolvedAddress(target_addr);
  std::string resolved_addr_str =
      SockaddrToString(&resolved_addr, true).value();
  auto sockets = CreateConnectedSockets(resolved_addr);
  Promise<bool> connect_promise;
  ASSERT_TRUE(sockets.ok());
  grpc_core::ChannelArgs args;
  auto quota = grpc_core::ResourceQuota::Default();
  args = args.Set(GRPC_ARG_RESOURCE_QUOTA, quota);
  ChannelArgsEndpointConfig config(args);
  auto memory_quota = absl::make_unique<grpc_core::MemoryQuota>("bar");
  (*g_engine)->Connect(
      [&connect_promise, &resolved_addr_str](
          absl::StatusOr<std::unique_ptr<EventEngine::Endpoint>> status) {
        ASSERT_FALSE(status.ok());
        absl::Status deadline_exceeded_expected_status =
            absl::CancelledError(absl::StrCat(
                "Failed to connect to remote host: ", resolved_addr_str,
                " with error: ",
                absl::DeadlineExceededError("connect() timed out").ToString()));
        absl::Status conn_reset_expected_status =
            absl::CancelledError(absl::StrCat(
                "Failed to connect to remote host: ", resolved_addr_str,
                " with error: ",
                absl::InternalError(absl::StrCat("getsockopt(SO_ERROR): ",
                                                 std::strerror(ECONNRESET)))
                    .ToString()));
        // Most of the time, the attempt will fail with deadline exceeded error.
        // Occasionally the kernel may send a RST to the connection attempt
        // instead of simply dropping the SYN packet. The RST will cause
        // the connection to fail before the timeout expires. The frequency
        // threshold for the RST depends on the kernel version. We dont
        // want to fail the test in both cases.
        EXPECT_TRUE(status.status() == deadline_exceeded_expected_status ||
                    status.status() == conn_reset_expected_status);
        connect_promise.Set(true);
      },
      URIToResolvedAddress(target_addr), config,
      memory_quota->CreateMemoryAllocator("conn-1"), 3s);
  ASSERT_TRUE(connect_promise.Get());
  for (auto sock : *sockets) {
    close(sock);
  }
}

TEST(PosixEventEngineTest, IndefiniteConnectCancellationTest) {
  std::string target_addr = absl::StrCat(
      "ipv6:[::1]:", std::to_string(grpc_pick_unused_port_or_die()));
  auto resolved_addr = URIToResolvedAddress(target_addr);
  std::string resolved_addr_str =
      SockaddrToString(&resolved_addr, true).value();
  auto sockets = CreateConnectedSockets(resolved_addr);
  ASSERT_TRUE(sockets.ok());
  grpc_core::ChannelArgs args;
  auto quota = grpc_core::ResourceQuota::Default();
  args = args.Set(GRPC_ARG_RESOURCE_QUOTA, quota);
  ChannelArgsEndpointConfig config(args);
  auto memory_quota = absl::make_unique<grpc_core::MemoryQuota>("bar");
  auto connection_handle = (*g_engine)->Connect(
      [](absl::StatusOr<std::unique_ptr<EventEngine::Endpoint>> /*status*/) {
        ASSERT_FALSE(
            "The on_connect callback should not have run since the connection "
            "attempt was cancelled.");
      },
      URIToResolvedAddress(target_addr), config,
      memory_quota->CreateMemoryAllocator("conn-2"), 3s);
  ASSERT_TRUE((*g_engine)->CancelConnect(connection_handle));
  for (auto sock : *sockets) {
    close(sock);
  }
}

}  // namespace posix_engine
}  // namespace grpc_event_engine

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
