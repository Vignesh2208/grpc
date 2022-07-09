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

#include <memory>

#include "absl/status/statusor.h"

#include "src/core/lib/event_engine/iomgr_engine/wakeup_fd_eventfd.h"
#include "src/core/lib/event_engine/iomgr_engine/wakeup_fd_pipe.h"
#include "src/core/lib/event_engine/iomgr_engine/wakeup_fd_posix.h"
#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_POSIX_WAKEUP_FD

namespace grpc_event_engine {
namespace iomgr_engine {

void ConfigureDefaultWakeupFdFactories() {
#ifndef GRPC_POSIX_NO_SPECIAL_WAKEUP_FD
  if (EventFdWakeupFd::IsSupported()) {
    SetDefaultWakeupFdFactoryIfUnset(&EventFdWakeupFd::CreateEventFdWakeupFd);
    return;
  }
#endif  // GRPC_POSIX_NO_SPECIAL_WAKEUP_FD
  if (PipeWakeupFd::IsSupported()) {
    SetDefaultWakeupFdFactoryIfUnset(&PipeWakeupFd::CreatePipeWakeupFd);
  }
}

}  // namespace iomgr_engine
}  // namespace grpc_event_engine

#endif
