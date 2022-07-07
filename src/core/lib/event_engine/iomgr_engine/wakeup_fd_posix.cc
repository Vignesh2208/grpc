// Copyright 2022 The gRPC Authors
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

#include <grpc/support/port_platform.h>

#include "src/core/lib/event_engine/iomgr_engine/wakeup_fd_posix.h"

#include <functional>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "src/core/lib/iomgr/port.h"

namespace grpc_event_engine {
namespace iomgr_engine {

#ifdef GRPC_POSIX_WAKEUP_FD

namespace {

absl::Mutex g_mu;
std::function<absl::StatusOr<std::unique_ptr<WakeupFd>>()> g_wakeup_fd_fn =
    nullptr;
}  // namespace

void SetDefaultWakeupFdFactoryIfUnset(
    std::function<absl::StatusOr<std::unique_ptr<WakeupFd>>()> factory) {
  absl::MutexLock lock(&g_mu);
  if (g_wakeup_fd_fn == nullptr) {
    g_wakeup_fd_fn = factory;
  }
}

bool SupportsWakeupFd() {
  absl::MutexLock lock(&g_mu);
  return g_wakeup_fd_fn != nullptr;
}

absl::StatusOr<std::unique_ptr<WakeupFd>> CreateWakeupFd() {
  if (SupportsWakeupFd()) {
    return g_wakeup_fd_fn();
  }
  return absl::NotFoundError("Wakeup-fd is not supported on this system");
}

#else

bool SupportsWakeupFd() { return false; }

absl::StatusOr<std::unique_ptr<WakeupFd>> CreateWakeupFd() {
  return absl::NotFoundError("Wakeup-fd is not supported on this system");
}

#endif

}  // namespace iomgr_engine
}  // namespace grpc_event_engine
