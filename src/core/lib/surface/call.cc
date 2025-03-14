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

#include <grpc/support/port_platform.h>

#include "src/core/lib/surface/call.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

#include <grpc/compression.h>
#include <grpc/grpc.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/compression/algorithm_metadata.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/gpr/alloc.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/time_precise.h"
#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/gprpp/arena.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_split.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/slice/slice_utils.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/call_test_only.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/completion_queue.h"
#include "src/core/lib/surface/server.h"
#include "src/core/lib/surface/validate_metadata.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/metadata.h"
#include "src/core/lib/transport/static_metadata.h"
#include "src/core/lib/transport/status_metadata.h"
#include "src/core/lib/transport/transport.h"

/** The maximum number of concurrent batches possible.
    Based upon the maximum number of individually queueable ops in the batch
    api:
      - initial metadata send
      - message send
      - status/close send (depending on client/server)
      - initial metadata recv
      - message recv
      - status/close recv (depending on client/server) */
#define MAX_CONCURRENT_BATCHES 6

#define MAX_SEND_EXTRA_METADATA_COUNT 3

// Used to create arena for the first call.
#define ESTIMATED_MDELEM_COUNT 16

struct batch_control {
  batch_control() = default;

  grpc_call* call = nullptr;
  grpc_transport_stream_op_batch op;
  /* Share memory for cq_completion and notify_tag as they are never needed
     simultaneously. Each byte used in this data structure count as six bytes
     per call, so any savings we can make are worthwhile,

     We use notify_tag to determine whether or not to send notification to the
     completion queue. Once we've made that determination, we can reuse the
     memory for cq_completion. */
  union {
    grpc_cq_completion cq_completion;
    struct {
      /* Any given op indicates completion by either (a) calling a closure or
         (b) sending a notification on the call's completion queue.  If
         \a is_closure is true, \a tag indicates a closure to be invoked;
         otherwise, \a tag indicates the tag to be used in the notification to
         be sent to the completion queue. */
      void* tag;
      bool is_closure;
    } notify_tag;
  } completion_data;
  grpc_closure start_batch;
  grpc_closure finish_batch;
  std::atomic<intptr_t> steps_to_complete{0};
  AtomicError batch_error;
  void set_num_steps_to_complete(uintptr_t steps) {
    steps_to_complete.store(steps, std::memory_order_release);
  }
  bool completed_batch_step() {
    return steps_to_complete.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }
};

struct parent_call {
  parent_call() { gpr_mu_init(&child_list_mu); }
  ~parent_call() { gpr_mu_destroy(&child_list_mu); }

  gpr_mu child_list_mu;
  grpc_call* first_child = nullptr;
};

struct child_call {
  explicit child_call(grpc_call* parent) : parent(parent) {}
  grpc_call* parent;
  /** siblings: children of the same parent form a list, and this list is
     protected under
      parent->mu */
  grpc_call* sibling_next = nullptr;
  grpc_call* sibling_prev = nullptr;
};

#define RECV_NONE ((gpr_atm)0)
#define RECV_INITIAL_METADATA_FIRST ((gpr_atm)1)

struct grpc_call {
  grpc_call(grpc_core::Arena* arena, const grpc_call_create_args& args)
      : arena(arena),
        cq(args.cq),
        channel(args.channel),
        is_client(args.server_transport_data == nullptr),
        stream_op_payload(context) {}

  ~grpc_call() {
    for (int i = 0; i < GRPC_CONTEXT_COUNT; ++i) {
      if (context[i].destroy) {
        context[i].destroy(context[i].value);
      }
    }
    gpr_free(static_cast<void*>(const_cast<char*>(final_info.error_string)));
  }

  grpc_core::RefCount ext_ref;
  grpc_core::Arena* arena;
  grpc_core::CallCombiner call_combiner;
  grpc_completion_queue* cq;
  grpc_polling_entity pollent;
  grpc_channel* channel;
  gpr_cycle_counter start_time = gpr_get_cycle_counter();
  /* parent_call* */ gpr_atm parent_call_atm = 0;
  child_call* child = nullptr;

  /* client or server call */
  bool is_client;
  /** has grpc_call_unref been called */
  bool destroy_called = false;
  /** flag indicating that cancellation is inherited */
  bool cancellation_is_inherited = false;
  // Trailers-only response status
  bool is_trailers_only = false;
  /** which ops are in-flight */
  bool sent_initial_metadata = false;
  bool sending_message = false;
  bool sent_final_op = false;
  bool received_initial_metadata = false;
  bool receiving_message = false;
  bool requested_final_op = false;
  gpr_atm any_ops_sent_atm = 0;
  gpr_atm received_final_op_atm = 0;

  batch_control* active_batches[MAX_CONCURRENT_BATCHES] = {};
  grpc_transport_stream_op_batch_payload stream_op_payload;

  /* first idx: is_receiving, second idx: is_trailing */
  grpc_metadata_batch metadata_batch[2][2] = {};

  /* Buffered read metadata waiting to be returned to the application.
     Element 0 is initial metadata, element 1 is trailing metadata. */
  grpc_metadata_array* buffered_metadata[2] = {};

  grpc_metadata compression_md;

  // A char* indicating the peer name.
  gpr_atm peer_string = 0;

  /* Call data useful used for reporting. Only valid after the call has
   * completed */
  grpc_call_final_info final_info;

  /* Compression algorithm for *incoming* data */
  grpc_message_compression_algorithm incoming_message_compression_algorithm =
      GRPC_MESSAGE_COMPRESS_NONE;
  /* Stream compression algorithm for *incoming* data */
  grpc_stream_compression_algorithm incoming_stream_compression_algorithm =
      GRPC_STREAM_COMPRESS_NONE;
  /* Supported encodings (compression algorithms), a bitset.
   * Always support no compression. */
  uint32_t encodings_accepted_by_peer = 1 << GRPC_MESSAGE_COMPRESS_NONE;
  /* Supported stream encodings (stream compression algorithms), a bitset */
  uint32_t stream_encodings_accepted_by_peer = 0;

  /* Contexts for various subsystems (security, tracing, ...). */
  grpc_call_context_element context[GRPC_CONTEXT_COUNT] = {};

  /* for the client, extra metadata is initial metadata; for the
     server, it's trailing metadata */
  grpc_linked_mdelem send_extra_metadata[MAX_SEND_EXTRA_METADATA_COUNT];
  int send_extra_metadata_count;
  grpc_millis send_deadline;

  grpc_core::ManualConstructor<grpc_core::SliceBufferByteStream> sending_stream;

  grpc_core::OrphanablePtr<grpc_core::ByteStream> receiving_stream;
  bool call_failed_before_recv_message = false;
  grpc_byte_buffer** receiving_buffer = nullptr;
  grpc_slice receiving_slice = grpc_empty_slice();
  grpc_closure receiving_slice_ready;
  grpc_closure receiving_stream_ready;
  grpc_closure receiving_initial_metadata_ready;
  grpc_closure receiving_trailing_metadata_ready;
  uint32_t test_only_last_message_flags = 0;
  // Status about operation of call
  bool sent_server_trailing_metadata = false;
  gpr_atm cancelled_with_error = 0;

  grpc_closure release_call;

  union {
    struct {
      grpc_status_code* status;
      grpc_slice* status_details;
      const char** error_string;
    } client;
    struct {
      int* cancelled;
      // backpointer to owning server if this is a server side call.
      grpc_core::Server* core_server;
    } server;
  } final_op;
  AtomicError status_error;

  /* recv_state can contain one of the following values:
     RECV_NONE :                 :  no initial metadata and messages received
     RECV_INITIAL_METADATA_FIRST :  received initial metadata first
     a batch_control*            :  received messages first

                 +------1------RECV_NONE------3-----+
                 |                                  |
                 |                                  |
                 v                                  v
     RECV_INITIAL_METADATA_FIRST        receiving_stream_ready_bctlp
           |           ^                      |           ^
           |           |                      |           |
           +-----2-----+                      +-----4-----+

    For 1, 4: See receiving_initial_metadata_ready() function
    For 2, 3: See receiving_stream_ready() function */
  gpr_atm recv_state = 0;
};

grpc_core::TraceFlag grpc_call_error_trace(false, "call_error");
grpc_core::TraceFlag grpc_compression_trace(false, "compression");

#define CALL_STACK_FROM_CALL(call)   \
  (grpc_call_stack*)((char*)(call) + \
                     GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call)))
#define CALL_FROM_CALL_STACK(call_stack) \
  (grpc_call*)(((char*)(call_stack)) -   \
               GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call)))

#define CALL_ELEM_FROM_CALL(call, idx) \
  grpc_call_stack_element(CALL_STACK_FROM_CALL(call), idx)
#define CALL_FROM_TOP_ELEM(top_elem) \
  CALL_FROM_CALL_STACK(grpc_call_stack_from_top_element(top_elem))

static void execute_batch(grpc_call* call,
                          grpc_transport_stream_op_batch* batch,
                          grpc_closure* start_batch_closure);

static void cancel_with_status(grpc_call* c, grpc_status_code status,
                               const char* description);
static void cancel_with_error(grpc_call* c, grpc_error_handle error);
static void destroy_call(void* call_stack, grpc_error_handle error);
static void receiving_slice_ready(void* bctlp, grpc_error_handle error);
static void set_final_status(grpc_call* call, grpc_error_handle error);
static void process_data_after_md(batch_control* bctl);
static void post_batch_completion(batch_control* bctl);

static void add_init_error(grpc_error_handle* composite,
                           grpc_error_handle new_err) {
  if (new_err == GRPC_ERROR_NONE) return;
  if (*composite == GRPC_ERROR_NONE) {
    *composite = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Call creation failed");
  }
  *composite = grpc_error_add_child(*composite, new_err);
}

void* grpc_call_arena_alloc(grpc_call* call, size_t size) {
  return call->arena->Alloc(size);
}

static parent_call* get_or_create_parent_call(grpc_call* call) {
  parent_call* p =
      reinterpret_cast<parent_call*>(gpr_atm_acq_load(&call->parent_call_atm));
  if (p == nullptr) {
    p = call->arena->New<parent_call>();
    if (!gpr_atm_rel_cas(&call->parent_call_atm,
                         reinterpret_cast<gpr_atm>(nullptr),
                         reinterpret_cast<gpr_atm>(p))) {
      p->~parent_call();
      p = reinterpret_cast<parent_call*>(
          gpr_atm_acq_load(&call->parent_call_atm));
    }
  }
  return p;
}

static parent_call* get_parent_call(grpc_call* call) {
  return reinterpret_cast<parent_call*>(
      gpr_atm_acq_load(&call->parent_call_atm));
}

size_t grpc_call_get_initial_size_estimate() {
  return sizeof(grpc_call) + sizeof(batch_control) * MAX_CONCURRENT_BATCHES +
         sizeof(grpc_linked_mdelem) * ESTIMATED_MDELEM_COUNT;
}

grpc_error_handle grpc_call_create(const grpc_call_create_args* args,
                                   grpc_call** out_call) {
  GPR_TIMER_SCOPE("grpc_call_create", 0);

  GRPC_CHANNEL_INTERNAL_REF(args->channel, "call");

  grpc_core::Arena* arena;
  grpc_call* call;
  grpc_error_handle error = GRPC_ERROR_NONE;
  grpc_channel_stack* channel_stack =
      grpc_channel_get_channel_stack(args->channel);
  size_t initial_size = grpc_channel_get_call_size_estimate(args->channel);
  GRPC_STATS_INC_CALL_INITIAL_SIZE(initial_size);
  size_t call_and_stack_size =
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call)) +
      channel_stack->call_stack_size;
  size_t call_alloc_size =
      call_and_stack_size + (args->parent ? sizeof(child_call) : 0);

  std::pair<grpc_core::Arena*, void*> arena_with_call =
      grpc_core::Arena::CreateWithAlloc(initial_size, call_alloc_size);
  arena = arena_with_call.first;
  call = new (arena_with_call.second) grpc_call(arena, *args);
  *out_call = call;
  grpc_slice path = grpc_empty_slice();
  if (call->is_client) {
    call->final_op.client.status_details = nullptr;
    call->final_op.client.status = nullptr;
    call->final_op.client.error_string = nullptr;
    GRPC_STATS_INC_CLIENT_CALLS_CREATED();
    GPR_ASSERT(args->add_initial_metadata_count <
               MAX_SEND_EXTRA_METADATA_COUNT);
    for (size_t i = 0; i < args->add_initial_metadata_count; i++) {
      call->send_extra_metadata[i].md = args->add_initial_metadata[i];
      if (grpc_slice_eq_static_interned(
              GRPC_MDKEY(args->add_initial_metadata[i]), GRPC_MDSTR_PATH)) {
        path = grpc_slice_ref_internal(
            GRPC_MDVALUE(args->add_initial_metadata[i]));
      }
    }
    call->send_extra_metadata_count =
        static_cast<int>(args->add_initial_metadata_count);
  } else {
    GRPC_STATS_INC_SERVER_CALLS_CREATED();
    call->final_op.server.cancelled = nullptr;
    call->final_op.server.core_server = args->server;
    GPR_ASSERT(args->add_initial_metadata_count == 0);
    call->send_extra_metadata_count = 0;
  }

  grpc_millis send_deadline = args->send_deadline;
  bool immediately_cancel = false;

  if (args->parent != nullptr) {
    call->child = new (reinterpret_cast<char*>(arena_with_call.second) +
                       call_and_stack_size) child_call(args->parent);

    GRPC_CALL_INTERNAL_REF(args->parent, "child");
    GPR_ASSERT(call->is_client);
    GPR_ASSERT(!args->parent->is_client);

    if (args->propagation_mask & GRPC_PROPAGATE_DEADLINE) {
      send_deadline = std::min(send_deadline, args->parent->send_deadline);
    }
    /* for now GRPC_PROPAGATE_TRACING_CONTEXT *MUST* be passed with
     * GRPC_PROPAGATE_STATS_CONTEXT */
    /* TODO(ctiller): This should change to use the appropriate census start_op
     * call. */
    if (args->propagation_mask & GRPC_PROPAGATE_CENSUS_TRACING_CONTEXT) {
      if (0 == (args->propagation_mask & GRPC_PROPAGATE_CENSUS_STATS_CONTEXT)) {
        add_init_error(&error, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                   "Census tracing propagation requested "
                                   "without Census context propagation"));
      }
      grpc_call_context_set(call, GRPC_CONTEXT_TRACING,
                            args->parent->context[GRPC_CONTEXT_TRACING].value,
                            nullptr);
    } else if (args->propagation_mask & GRPC_PROPAGATE_CENSUS_STATS_CONTEXT) {
      add_init_error(&error, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                 "Census context propagation requested "
                                 "without Census tracing propagation"));
    }
    if (args->propagation_mask & GRPC_PROPAGATE_CANCELLATION) {
      call->cancellation_is_inherited = true;
      if (gpr_atm_acq_load(&args->parent->received_final_op_atm)) {
        immediately_cancel = true;
      }
    }
  }
  call->send_deadline = send_deadline;
  /* initial refcount dropped by grpc_call_unref */
  grpc_call_element_args call_args = {CALL_STACK_FROM_CALL(call),
                                      args->server_transport_data,
                                      call->context,
                                      path,
                                      call->start_time,
                                      send_deadline,
                                      call->arena,
                                      &call->call_combiner};
  add_init_error(&error, grpc_call_stack_init(channel_stack, 1, destroy_call,
                                              call, &call_args));
  // Publish this call to parent only after the call stack has been initialized.
  if (args->parent != nullptr) {
    child_call* cc = call->child;
    parent_call* pc = get_or_create_parent_call(args->parent);
    gpr_mu_lock(&pc->child_list_mu);
    if (pc->first_child == nullptr) {
      pc->first_child = call;
      cc->sibling_next = cc->sibling_prev = call;
    } else {
      cc->sibling_next = pc->first_child;
      cc->sibling_prev = pc->first_child->child->sibling_prev;
      cc->sibling_next->child->sibling_prev =
          cc->sibling_prev->child->sibling_next = call;
    }
    gpr_mu_unlock(&pc->child_list_mu);
  }

  if (error != GRPC_ERROR_NONE) {
    cancel_with_error(call, GRPC_ERROR_REF(error));
  }
  if (immediately_cancel) {
    cancel_with_error(call, GRPC_ERROR_CANCELLED);
  }
  if (args->cq != nullptr) {
    GPR_ASSERT(args->pollset_set_alternative == nullptr &&
               "Only one of 'cq' and 'pollset_set_alternative' should be "
               "non-nullptr.");
    GRPC_CQ_INTERNAL_REF(args->cq, "bind");
    call->pollent =
        grpc_polling_entity_create_from_pollset(grpc_cq_pollset(args->cq));
  }
  if (args->pollset_set_alternative != nullptr) {
    call->pollent = grpc_polling_entity_create_from_pollset_set(
        args->pollset_set_alternative);
  }
  if (!grpc_polling_entity_is_empty(&call->pollent)) {
    grpc_call_stack_set_pollset_or_pollset_set(CALL_STACK_FROM_CALL(call),
                                               &call->pollent);
  }

  if (call->is_client) {
    grpc_core::channelz::ChannelNode* channelz_channel =
        grpc_channel_get_channelz_node(call->channel);
    if (channelz_channel != nullptr) {
      channelz_channel->RecordCallStarted();
    }
  } else if (call->final_op.server.core_server != nullptr) {
    grpc_core::channelz::ServerNode* channelz_node =
        call->final_op.server.core_server->channelz_node();
    if (channelz_node != nullptr) {
      channelz_node->RecordCallStarted();
    }
  }

  grpc_slice_unref_internal(path);

  return error;
}

void grpc_call_set_completion_queue(grpc_call* call,
                                    grpc_completion_queue* cq) {
  GPR_ASSERT(cq);

  if (grpc_polling_entity_pollset_set(&call->pollent) != nullptr) {
    gpr_log(GPR_ERROR, "A pollset_set is already registered for this call.");
    abort();
  }
  call->cq = cq;
  GRPC_CQ_INTERNAL_REF(cq, "bind");
  call->pollent = grpc_polling_entity_create_from_pollset(grpc_cq_pollset(cq));
  grpc_call_stack_set_pollset_or_pollset_set(CALL_STACK_FROM_CALL(call),
                                             &call->pollent);
}

#ifndef NDEBUG
#define REF_REASON reason
#define REF_ARG , const char* reason
#else
#define REF_REASON ""
#define REF_ARG
#endif
void grpc_call_internal_ref(grpc_call* c REF_ARG) {
  GRPC_CALL_STACK_REF(CALL_STACK_FROM_CALL(c), REF_REASON);
}
void grpc_call_internal_unref(grpc_call* c REF_ARG) {
  GRPC_CALL_STACK_UNREF(CALL_STACK_FROM_CALL(c), REF_REASON);
}

static void release_call(void* call, grpc_error_handle /*error*/) {
  grpc_call* c = static_cast<grpc_call*>(call);
  grpc_channel* channel = c->channel;
  grpc_core::Arena* arena = c->arena;
  c->~grpc_call();
  grpc_channel_update_call_size_estimate(channel, arena->Destroy());
  GRPC_CHANNEL_INTERNAL_UNREF(channel, "call");
}

static void destroy_call(void* call, grpc_error_handle /*error*/) {
  GPR_TIMER_SCOPE("destroy_call", 0);
  grpc_call* c = static_cast<grpc_call*>(call);
  for (int i = 0; i < 2; i++) {
    c->metadata_batch[1 /* is_receiving */][i /* is_initial */].Clear();
  }
  c->receiving_stream.reset();
  parent_call* pc = get_parent_call(c);
  if (pc != nullptr) {
    pc->~parent_call();
  }
  for (int i = 0; i < c->send_extra_metadata_count; i++) {
    GRPC_MDELEM_UNREF(c->send_extra_metadata[i].md);
  }
  if (c->cq) {
    GRPC_CQ_INTERNAL_UNREF(c->cq, "bind");
  }

  grpc_error_handle status_error = c->status_error.get();
  grpc_error_get_status(status_error, c->send_deadline,
                        &c->final_info.final_status, nullptr, nullptr,
                        &(c->final_info.error_string));
  c->status_error.set(GRPC_ERROR_NONE);
  c->final_info.stats.latency =
      gpr_cycle_counter_sub(gpr_get_cycle_counter(), c->start_time);
  grpc_call_stack_destroy(CALL_STACK_FROM_CALL(c), &c->final_info,
                          GRPC_CLOSURE_INIT(&c->release_call, release_call, c,
                                            grpc_schedule_on_exec_ctx));
}

void grpc_call_ref(grpc_call* c) { c->ext_ref.Ref(); }

void grpc_call_unref(grpc_call* c) {
  if (GPR_LIKELY(!c->ext_ref.Unref())) return;

  GPR_TIMER_SCOPE("grpc_call_unref", 0);

  child_call* cc = c->child;
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;

  GRPC_API_TRACE("grpc_call_unref(c=%p)", 1, (c));

  if (cc) {
    parent_call* pc = get_parent_call(cc->parent);
    gpr_mu_lock(&pc->child_list_mu);
    if (c == pc->first_child) {
      pc->first_child = cc->sibling_next;
      if (c == pc->first_child) {
        pc->first_child = nullptr;
      }
    }
    cc->sibling_prev->child->sibling_next = cc->sibling_next;
    cc->sibling_next->child->sibling_prev = cc->sibling_prev;
    gpr_mu_unlock(&pc->child_list_mu);
    GRPC_CALL_INTERNAL_UNREF(cc->parent, "child");
  }

  GPR_ASSERT(!c->destroy_called);
  c->destroy_called = true;
  bool cancel = gpr_atm_acq_load(&c->any_ops_sent_atm) != 0 &&
                gpr_atm_acq_load(&c->received_final_op_atm) == 0;
  if (cancel) {
    cancel_with_error(c, GRPC_ERROR_CANCELLED);
  } else {
    // Unset the call combiner cancellation closure.  This has the
    // effect of scheduling the previously set cancellation closure, if
    // any, so that it can release any internal references it may be
    // holding to the call stack.
    c->call_combiner.SetNotifyOnCancel(nullptr);
  }
  GRPC_CALL_INTERNAL_UNREF(c, "destroy");
}

grpc_call_error grpc_call_cancel(grpc_call* call, void* reserved) {
  GRPC_API_TRACE("grpc_call_cancel(call=%p, reserved=%p)", 2, (call, reserved));
  GPR_ASSERT(!reserved);
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;
  cancel_with_error(call, GRPC_ERROR_CANCELLED);
  return GRPC_CALL_OK;
}

// This is called via the call combiner to start sending a batch down
// the filter stack.
static void execute_batch_in_call_combiner(void* arg,
                                           grpc_error_handle /*ignored*/) {
  GPR_TIMER_SCOPE("execute_batch_in_call_combiner", 0);
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  grpc_call* call = static_cast<grpc_call*>(batch->handler_private.extra_arg);
  grpc_call_element* elem = CALL_ELEM_FROM_CALL(call, 0);
  GRPC_CALL_LOG_OP(GPR_INFO, elem, batch);
  elem->filter->start_transport_stream_op_batch(elem, batch);
}

// start_batch_closure points to a caller-allocated closure to be used
// for entering the call combiner.
static void execute_batch(grpc_call* call,
                          grpc_transport_stream_op_batch* batch,
                          grpc_closure* start_batch_closure) {
  batch->handler_private.extra_arg = call;
  GRPC_CLOSURE_INIT(start_batch_closure, execute_batch_in_call_combiner, batch,
                    grpc_schedule_on_exec_ctx);
  GRPC_CALL_COMBINER_START(&call->call_combiner, start_batch_closure,
                           GRPC_ERROR_NONE, "executing batch");
}

char* grpc_call_get_peer(grpc_call* call) {
  char* peer_string =
      reinterpret_cast<char*>(gpr_atm_acq_load(&call->peer_string));
  if (peer_string != nullptr) return gpr_strdup(peer_string);
  peer_string = grpc_channel_get_target(call->channel);
  if (peer_string != nullptr) return peer_string;
  return gpr_strdup("unknown");
}

grpc_call* grpc_call_from_top_element(grpc_call_element* surface_element) {
  return CALL_FROM_TOP_ELEM(surface_element);
}

/*******************************************************************************
 * CANCELLATION
 */

grpc_call_error grpc_call_cancel_with_status(grpc_call* c,
                                             grpc_status_code status,
                                             const char* description,
                                             void* reserved) {
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;
  GRPC_API_TRACE(
      "grpc_call_cancel_with_status("
      "c=%p, status=%d, description=%s, reserved=%p)",
      4, (c, (int)status, description, reserved));
  GPR_ASSERT(reserved == nullptr);
  cancel_with_status(c, status, description);
  return GRPC_CALL_OK;
}

struct cancel_state {
  grpc_call* call;
  grpc_closure start_batch;
  grpc_closure finish_batch;
};
// The on_complete callback used when sending a cancel_stream batch down
// the filter stack.  Yields the call combiner when the batch is done.
static void done_termination(void* arg, grpc_error_handle /*error*/) {
  cancel_state* state = static_cast<cancel_state*>(arg);
  GRPC_CALL_COMBINER_STOP(&state->call->call_combiner,
                          "on_complete for cancel_stream op");
  GRPC_CALL_INTERNAL_UNREF(state->call, "termination");
  gpr_free(state);
}

static void cancel_with_error(grpc_call* c, grpc_error_handle error) {
  if (!gpr_atm_rel_cas(&c->cancelled_with_error, 0, 1)) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  GRPC_CALL_INTERNAL_REF(c, "termination");
  // Inform the call combiner of the cancellation, so that it can cancel
  // any in-flight asynchronous actions that may be holding the call
  // combiner.  This ensures that the cancel_stream batch can be sent
  // down the filter stack in a timely manner.
  c->call_combiner.Cancel(GRPC_ERROR_REF(error));
  cancel_state* state = static_cast<cancel_state*>(gpr_malloc(sizeof(*state)));
  state->call = c;
  GRPC_CLOSURE_INIT(&state->finish_batch, done_termination, state,
                    grpc_schedule_on_exec_ctx);
  grpc_transport_stream_op_batch* op =
      grpc_make_transport_stream_op(&state->finish_batch);
  op->cancel_stream = true;
  op->payload->cancel_stream.cancel_error = error;
  execute_batch(c, op, &state->start_batch);
}

void grpc_call_cancel_internal(grpc_call* call) {
  cancel_with_error(call, GRPC_ERROR_CANCELLED);
}

static grpc_error_handle error_from_status(grpc_status_code status,
                                           const char* description) {
  // copying 'description' is needed to ensure the grpc_call_cancel_with_status
  // guarantee that can be short-lived.
  return grpc_error_set_int(
      grpc_error_set_str(GRPC_ERROR_CREATE_FROM_COPIED_STRING(description),
                         GRPC_ERROR_STR_GRPC_MESSAGE,
                         grpc_slice_from_copied_string(description)),
      GRPC_ERROR_INT_GRPC_STATUS, status);
}

static void cancel_with_status(grpc_call* c, grpc_status_code status,
                               const char* description) {
  cancel_with_error(c, error_from_status(status, description));
}

static void set_final_status(grpc_call* call, grpc_error_handle error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_call_error_trace)) {
    gpr_log(GPR_DEBUG, "set_final_status %s", call->is_client ? "CLI" : "SVR");
    gpr_log(GPR_DEBUG, "%s", grpc_error_std_string(error).c_str());
  }
  if (call->is_client) {
    grpc_error_get_status(error, call->send_deadline,
                          call->final_op.client.status,
                          call->final_op.client.status_details, nullptr,
                          call->final_op.client.error_string);
    // explicitly take a ref
    grpc_slice_ref_internal(*call->final_op.client.status_details);
    call->status_error.set(error);
    GRPC_ERROR_UNREF(error);
    grpc_core::channelz::ChannelNode* channelz_channel =
        grpc_channel_get_channelz_node(call->channel);
    if (channelz_channel != nullptr) {
      if (*call->final_op.client.status != GRPC_STATUS_OK) {
        channelz_channel->RecordCallFailed();
      } else {
        channelz_channel->RecordCallSucceeded();
      }
    }
  } else {
    *call->final_op.server.cancelled =
        error != GRPC_ERROR_NONE || !call->sent_server_trailing_metadata;
    grpc_core::channelz::ServerNode* channelz_node =
        call->final_op.server.core_server->channelz_node();
    if (channelz_node != nullptr) {
      if (*call->final_op.server.cancelled || !call->status_error.ok()) {
        channelz_node->RecordCallFailed();
      } else {
        channelz_node->RecordCallSucceeded();
      }
    }
    GRPC_ERROR_UNREF(error);
  }
}

/*******************************************************************************
 * COMPRESSION
 */

static void set_incoming_message_compression_algorithm(
    grpc_call* call, grpc_message_compression_algorithm algo) {
  GPR_ASSERT(algo < GRPC_MESSAGE_COMPRESS_ALGORITHMS_COUNT);
  call->incoming_message_compression_algorithm = algo;
}

static void set_incoming_stream_compression_algorithm(
    grpc_call* call, grpc_stream_compression_algorithm algo) {
  GPR_ASSERT(algo < GRPC_STREAM_COMPRESS_ALGORITHMS_COUNT);
  call->incoming_stream_compression_algorithm = algo;
}

grpc_compression_algorithm grpc_call_test_only_get_compression_algorithm(
    grpc_call* call) {
  grpc_compression_algorithm algorithm = GRPC_COMPRESS_NONE;
  grpc_compression_algorithm_from_message_stream_compression_algorithm(
      &algorithm, call->incoming_message_compression_algorithm,
      call->incoming_stream_compression_algorithm);
  return algorithm;
}

static grpc_compression_algorithm compression_algorithm_for_level_locked(
    grpc_call* call, grpc_compression_level level) {
  return grpc_compression_algorithm_for_level(level,
                                              call->encodings_accepted_by_peer);
}

uint32_t grpc_call_test_only_get_message_flags(grpc_call* call) {
  uint32_t flags;
  flags = call->test_only_last_message_flags;
  return flags;
}

static void destroy_encodings_accepted_by_peer(void* /*p*/) {}

static void set_encodings_accepted_by_peer(grpc_call* /*call*/,
                                           grpc_mdelem mdel,
                                           uint32_t* encodings_accepted_by_peer,
                                           bool stream_encoding) {
  size_t i;
  uint32_t algorithm;
  grpc_slice_buffer accept_encoding_parts;
  grpc_slice accept_encoding_slice;
  void* accepted_user_data;

  accepted_user_data =
      grpc_mdelem_get_user_data(mdel, destroy_encodings_accepted_by_peer);
  if (accepted_user_data != nullptr) {
    *encodings_accepted_by_peer = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(accepted_user_data) - 1);
    return;
  }

  *encodings_accepted_by_peer = 0;

  accept_encoding_slice = GRPC_MDVALUE(mdel);
  grpc_slice_buffer_init(&accept_encoding_parts);
  grpc_slice_split_without_space(accept_encoding_slice, ",",
                                 &accept_encoding_parts);

  grpc_core::SetBit(encodings_accepted_by_peer, GRPC_COMPRESS_NONE);
  for (i = 0; i < accept_encoding_parts.count; i++) {
    int r;
    grpc_slice accept_encoding_entry_slice = accept_encoding_parts.slices[i];
    if (!stream_encoding) {
      r = grpc_message_compression_algorithm_parse(
          accept_encoding_entry_slice,
          reinterpret_cast<grpc_message_compression_algorithm*>(&algorithm));
    } else {
      r = grpc_stream_compression_algorithm_parse(
          accept_encoding_entry_slice,
          reinterpret_cast<grpc_stream_compression_algorithm*>(&algorithm));
    }
    if (r) {
      grpc_core::SetBit(encodings_accepted_by_peer, algorithm);
    } else {
      char* accept_encoding_entry_str =
          grpc_slice_to_c_string(accept_encoding_entry_slice);
      gpr_log(GPR_DEBUG,
              "Unknown entry in accept encoding metadata: '%s'. Ignoring.",
              accept_encoding_entry_str);
      gpr_free(accept_encoding_entry_str);
    }
  }

  grpc_slice_buffer_destroy_internal(&accept_encoding_parts);

  grpc_mdelem_set_user_data(
      mdel, destroy_encodings_accepted_by_peer,
      reinterpret_cast<void*>(
          static_cast<uintptr_t>(*encodings_accepted_by_peer) + 1));
}

uint32_t grpc_call_test_only_get_encodings_accepted_by_peer(grpc_call* call) {
  uint32_t encodings_accepted_by_peer;
  encodings_accepted_by_peer = call->encodings_accepted_by_peer;
  return encodings_accepted_by_peer;
}

grpc_stream_compression_algorithm
grpc_call_test_only_get_incoming_stream_encodings(grpc_call* call) {
  return call->incoming_stream_compression_algorithm;
}

static grpc_linked_mdelem* linked_from_md(grpc_metadata* md) {
  return reinterpret_cast<grpc_linked_mdelem*>(&md->internal_data);
}

static grpc_metadata* get_md_elem(grpc_metadata* metadata,
                                  grpc_metadata* additional_metadata, int i,
                                  int count) {
  grpc_metadata* res =
      i < count ? &metadata[i] : &additional_metadata[i - count];
  GPR_ASSERT(res);
  return res;
}

static int prepare_application_metadata(grpc_call* call, int count,
                                        grpc_metadata* metadata,
                                        int is_trailing,
                                        int prepend_extra_metadata,
                                        grpc_metadata* additional_metadata,
                                        int additional_metadata_count) {
  int total_count = count + additional_metadata_count;
  int i;
  grpc_metadata_batch* batch =
      &call->metadata_batch[0 /* is_receiving */][is_trailing];
  for (i = 0; i < total_count; i++) {
    grpc_metadata* md = get_md_elem(metadata, additional_metadata, i, count);
    grpc_linked_mdelem* l = linked_from_md(md);
    GPR_ASSERT(sizeof(grpc_linked_mdelem) == sizeof(md->internal_data));
    if (!GRPC_LOG_IF_ERROR("validate_metadata",
                           grpc_validate_header_key_is_legal(md->key))) {
      break;
    } else if (!grpc_is_binary_header_internal(md->key) &&
               !GRPC_LOG_IF_ERROR(
                   "validate_metadata",
                   grpc_validate_header_nonbin_value_is_legal(md->value))) {
      break;
    } else if (GRPC_SLICE_LENGTH(md->value) >= UINT32_MAX) {
      // HTTP2 hpack encoding has a maximum limit.
      break;
    }
    l->md = grpc_mdelem_from_grpc_metadata(const_cast<grpc_metadata*>(md));
  }
  if (i != total_count) {
    for (int j = 0; j < i; j++) {
      grpc_metadata* md = get_md_elem(metadata, additional_metadata, j, count);
      grpc_linked_mdelem* l = linked_from_md(md);
      GRPC_MDELEM_UNREF(l->md);
    }
    return 0;
  }
  if (prepend_extra_metadata) {
    if (call->send_extra_metadata_count == 0) {
      prepend_extra_metadata = 0;
    } else {
      for (i = 0; i < call->send_extra_metadata_count; i++) {
        GRPC_LOG_IF_ERROR("prepare_application_metadata",
                          batch->LinkTail(&call->send_extra_metadata[i]));
      }
    }
  }
  for (i = 0; i < total_count; i++) {
    grpc_metadata* md = get_md_elem(metadata, additional_metadata, i, count);
    grpc_linked_mdelem* l = linked_from_md(md);
    grpc_error_handle error = batch->LinkTail(l);
    if (error != GRPC_ERROR_NONE) {
      GRPC_MDELEM_UNREF(l->md);
    }
    GRPC_LOG_IF_ERROR("prepare_application_metadata", error);
  }
  call->send_extra_metadata_count = 0;

  return 1;
}

static grpc_message_compression_algorithm decode_message_compression(
    grpc_mdelem md) {
  grpc_message_compression_algorithm algorithm =
      grpc_message_compression_algorithm_from_slice(GRPC_MDVALUE(md));
  if (algorithm == GRPC_MESSAGE_COMPRESS_ALGORITHMS_COUNT) {
    char* md_c_str = grpc_slice_to_c_string(GRPC_MDVALUE(md));
    gpr_log(GPR_ERROR,
            "Invalid incoming message compression algorithm: '%s'. "
            "Interpreting incoming data as uncompressed.",
            md_c_str);
    gpr_free(md_c_str);
    return GRPC_MESSAGE_COMPRESS_NONE;
  }
  return algorithm;
}

static grpc_stream_compression_algorithm decode_stream_compression(
    grpc_mdelem md) {
  grpc_stream_compression_algorithm algorithm =
      grpc_stream_compression_algorithm_from_slice(GRPC_MDVALUE(md));
  if (algorithm == GRPC_STREAM_COMPRESS_ALGORITHMS_COUNT) {
    char* md_c_str = grpc_slice_to_c_string(GRPC_MDVALUE(md));
    gpr_log(GPR_ERROR,
            "Invalid incoming stream compression algorithm: '%s'. Interpreting "
            "incoming data as uncompressed.",
            md_c_str);
    gpr_free(md_c_str);
    return GRPC_STREAM_COMPRESS_NONE;
  }
  return algorithm;
}

static void publish_app_metadata(grpc_call* call, grpc_metadata_batch* b,
                                 int is_trailing) {
  if (b->non_deadline_count() == 0) return;
  if (!call->is_client && is_trailing) return;
  if (is_trailing && call->buffered_metadata[1] == nullptr) return;
  GPR_TIMER_SCOPE("publish_app_metadata", 0);
  grpc_metadata_array* dest;
  grpc_metadata* mdusr;
  dest = call->buffered_metadata[is_trailing];
  if (dest->count + b->non_deadline_count() > dest->capacity) {
    dest->capacity = std::max(dest->capacity + b->non_deadline_count(),
                              dest->capacity * 3 / 2);
    dest->metadata = static_cast<grpc_metadata*>(
        gpr_realloc(dest->metadata, sizeof(grpc_metadata) * dest->capacity));
  }
  b->ForEach([&](grpc_mdelem md) {
    mdusr = &dest->metadata[dest->count++];
    /* we pass back borrowed slices that are valid whilst the call is valid */
    mdusr->key = GRPC_MDKEY(md);
    mdusr->value = GRPC_MDVALUE(md);
  });
}

static void recv_initial_filter(grpc_call* call, grpc_metadata_batch* b) {
  if (b->legacy_index()->named.content_encoding != nullptr) {
    GPR_TIMER_SCOPE("incoming_stream_compression_algorithm", 0);
    set_incoming_stream_compression_algorithm(
        call, decode_stream_compression(
                  b->legacy_index()->named.content_encoding->md));
    b->Remove(GRPC_BATCH_CONTENT_ENCODING);
  }
  if (b->legacy_index()->named.grpc_encoding != nullptr) {
    GPR_TIMER_SCOPE("incoming_message_compression_algorithm", 0);
    set_incoming_message_compression_algorithm(
        call,
        decode_message_compression(b->legacy_index()->named.grpc_encoding->md));
    b->Remove(GRPC_BATCH_GRPC_ENCODING);
  }
  uint32_t message_encodings_accepted_by_peer = 1u;
  uint32_t stream_encodings_accepted_by_peer = 1u;
  if (b->legacy_index()->named.grpc_accept_encoding != nullptr) {
    GPR_TIMER_SCOPE("encodings_accepted_by_peer", 0);
    set_encodings_accepted_by_peer(
        call, b->legacy_index()->named.grpc_accept_encoding->md,
        &message_encodings_accepted_by_peer, false);
    b->Remove(GRPC_BATCH_GRPC_ACCEPT_ENCODING);
  }
  if (b->legacy_index()->named.accept_encoding != nullptr) {
    GPR_TIMER_SCOPE("stream_encodings_accepted_by_peer", 0);
    set_encodings_accepted_by_peer(call,
                                   b->legacy_index()->named.accept_encoding->md,
                                   &stream_encodings_accepted_by_peer, true);
    b->Remove(GRPC_BATCH_ACCEPT_ENCODING);
  }
  call->encodings_accepted_by_peer =
      grpc_compression_bitset_from_message_stream_compression_bitset(
          message_encodings_accepted_by_peer,
          stream_encodings_accepted_by_peer);
  publish_app_metadata(call, b, false);
}

static void recv_trailing_filter(void* args, grpc_metadata_batch* b,
                                 grpc_error_handle batch_error) {
  grpc_call* call = static_cast<grpc_call*>(args);
  if (batch_error != GRPC_ERROR_NONE) {
    set_final_status(call, batch_error);
  } else if (b->legacy_index()->named.grpc_status != nullptr) {
    grpc_status_code status_code = grpc_get_status_code_from_metadata(
        b->legacy_index()->named.grpc_status->md);
    grpc_error_handle error = GRPC_ERROR_NONE;
    if (status_code != GRPC_STATUS_OK) {
      char* peer = grpc_call_get_peer(call);
      error = grpc_error_set_int(GRPC_ERROR_CREATE_FROM_CPP_STRING(absl::StrCat(
                                     "Error received from peer ", peer)),
                                 GRPC_ERROR_INT_GRPC_STATUS,
                                 static_cast<intptr_t>(status_code));
      gpr_free(peer);
    }
    if (b->legacy_index()->named.grpc_message != nullptr) {
      error = grpc_error_set_str(
          error, GRPC_ERROR_STR_GRPC_MESSAGE,
          grpc_slice_ref_internal(
              GRPC_MDVALUE(b->legacy_index()->named.grpc_message->md)));
      b->Remove(GRPC_BATCH_GRPC_MESSAGE);
    } else if (error != GRPC_ERROR_NONE) {
      error = grpc_error_set_str(error, GRPC_ERROR_STR_GRPC_MESSAGE,
                                 grpc_empty_slice());
    }
    set_final_status(call, GRPC_ERROR_REF(error));
    b->Remove(GRPC_BATCH_GRPC_STATUS);
    GRPC_ERROR_UNREF(error);
  } else if (!call->is_client) {
    set_final_status(call, GRPC_ERROR_NONE);
  } else {
    gpr_log(GPR_DEBUG,
            "Received trailing metadata with no error and no status");
    set_final_status(
        call, grpc_error_set_int(
                  GRPC_ERROR_CREATE_FROM_STATIC_STRING("No status received"),
                  GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNKNOWN));
  }
  publish_app_metadata(call, b, true);
}

grpc_core::Arena* grpc_call_get_arena(grpc_call* call) { return call->arena; }

grpc_call_stack* grpc_call_get_call_stack(grpc_call* call) {
  return CALL_STACK_FROM_CALL(call);
}

/*******************************************************************************
 * BATCH API IMPLEMENTATION
 */

static bool are_write_flags_valid(uint32_t flags) {
  /* check that only bits in GRPC_WRITE_(INTERNAL?)_USED_MASK are set */
  const uint32_t allowed_write_positions =
      (GRPC_WRITE_USED_MASK | GRPC_WRITE_INTERNAL_USED_MASK);
  const uint32_t invalid_positions = ~allowed_write_positions;
  return !(flags & invalid_positions);
}

static bool are_initial_metadata_flags_valid(uint32_t flags, bool is_client) {
  /* check that only bits in GRPC_WRITE_(INTERNAL?)_USED_MASK are set */
  uint32_t invalid_positions = ~GRPC_INITIAL_METADATA_USED_MASK;
  if (!is_client) {
    invalid_positions |= GRPC_INITIAL_METADATA_IDEMPOTENT_REQUEST;
  }
  return !(flags & invalid_positions);
}

static size_t batch_slot_for_op(grpc_op_type type) {
  switch (type) {
    case GRPC_OP_SEND_INITIAL_METADATA:
      return 0;
    case GRPC_OP_SEND_MESSAGE:
      return 1;
    case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
    case GRPC_OP_SEND_STATUS_FROM_SERVER:
      return 2;
    case GRPC_OP_RECV_INITIAL_METADATA:
      return 3;
    case GRPC_OP_RECV_MESSAGE:
      return 4;
    case GRPC_OP_RECV_CLOSE_ON_SERVER:
    case GRPC_OP_RECV_STATUS_ON_CLIENT:
      return 5;
  }
  GPR_UNREACHABLE_CODE(return 123456789);
}

static batch_control* reuse_or_allocate_batch_control(grpc_call* call,
                                                      const grpc_op* ops) {
  size_t slot_idx = batch_slot_for_op(ops[0].op);
  batch_control** pslot = &call->active_batches[slot_idx];
  batch_control* bctl;
  if (*pslot != nullptr) {
    bctl = *pslot;
    if (bctl->call != nullptr) {
      return nullptr;
    }
    bctl->~batch_control();
    bctl->op = {};
    new (&bctl->batch_error) AtomicError();
  } else {
    bctl = call->arena->New<batch_control>();
    *pslot = bctl;
  }
  bctl->call = call;
  bctl->op.payload = &call->stream_op_payload;
  return bctl;
}

static void finish_batch_completion(void* user_data,
                                    grpc_cq_completion* /*storage*/) {
  batch_control* bctl = static_cast<batch_control*>(user_data);
  grpc_call* call = bctl->call;
  bctl->call = nullptr;
  GRPC_CALL_INTERNAL_UNREF(call, "completion");
}

static void reset_batch_errors(batch_control* bctl) {
  bctl->batch_error.set(GRPC_ERROR_NONE);
}

static void post_batch_completion(batch_control* bctl) {
  grpc_call* next_child_call;
  grpc_call* call = bctl->call;
  grpc_error_handle error = GRPC_ERROR_REF(bctl->batch_error.get());

  if (bctl->op.send_initial_metadata) {
    call->metadata_batch[0 /* is_receiving */][0 /* is_trailing */].Clear();
  }
  if (bctl->op.send_message) {
    if (bctl->op.payload->send_message.stream_write_closed &&
        error == GRPC_ERROR_NONE) {
      error = grpc_error_add_child(
          error, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                     "Attempt to send message after stream was closed."));
    }
    call->sending_message = false;
  }
  if (bctl->op.send_trailing_metadata) {
    call->metadata_batch[0 /* is_receiving */][1 /* is_trailing */].Clear();
  }
  if (bctl->op.recv_trailing_metadata) {
    /* propagate cancellation to any interested children */
    gpr_atm_rel_store(&call->received_final_op_atm, 1);
    parent_call* pc = get_parent_call(call);
    if (pc != nullptr) {
      grpc_call* child;
      gpr_mu_lock(&pc->child_list_mu);
      child = pc->first_child;
      if (child != nullptr) {
        do {
          next_child_call = child->child->sibling_next;
          if (child->cancellation_is_inherited) {
            GRPC_CALL_INTERNAL_REF(child, "propagate_cancel");
            cancel_with_error(child, GRPC_ERROR_CANCELLED);
            GRPC_CALL_INTERNAL_UNREF(child, "propagate_cancel");
          }
          child = next_child_call;
        } while (child != pc->first_child);
      }
      gpr_mu_unlock(&pc->child_list_mu);
    }
    GRPC_ERROR_UNREF(error);
    error = GRPC_ERROR_NONE;
  }
  if (error != GRPC_ERROR_NONE && bctl->op.recv_message &&
      *call->receiving_buffer != nullptr) {
    grpc_byte_buffer_destroy(*call->receiving_buffer);
    *call->receiving_buffer = nullptr;
  }
  reset_batch_errors(bctl);

  if (bctl->completion_data.notify_tag.is_closure) {
    /* unrefs error */
    bctl->call = nullptr;
    grpc_core::Closure::Run(
        DEBUG_LOCATION,
        static_cast<grpc_closure*>(bctl->completion_data.notify_tag.tag),
        error);
    GRPC_CALL_INTERNAL_UNREF(call, "completion");
  } else {
    /* unrefs error */
    grpc_cq_end_op(bctl->call->cq, bctl->completion_data.notify_tag.tag, error,
                   finish_batch_completion, bctl,
                   &bctl->completion_data.cq_completion);
  }
}

static void finish_batch_step(batch_control* bctl) {
  if (GPR_UNLIKELY(bctl->completed_batch_step())) {
    post_batch_completion(bctl);
  }
}

static void continue_receiving_slices(batch_control* bctl) {
  grpc_error_handle error;
  grpc_call* call = bctl->call;
  for (;;) {
    size_t remaining = call->receiving_stream->length() -
                       (*call->receiving_buffer)->data.raw.slice_buffer.length;
    if (remaining == 0) {
      call->receiving_message = false;
      call->receiving_stream.reset();
      finish_batch_step(bctl);
      return;
    }
    if (call->receiving_stream->Next(remaining, &call->receiving_slice_ready)) {
      error = call->receiving_stream->Pull(&call->receiving_slice);
      if (error == GRPC_ERROR_NONE) {
        grpc_slice_buffer_add(&(*call->receiving_buffer)->data.raw.slice_buffer,
                              call->receiving_slice);
      } else {
        call->receiving_stream.reset();
        grpc_byte_buffer_destroy(*call->receiving_buffer);
        *call->receiving_buffer = nullptr;
        call->receiving_message = false;
        finish_batch_step(bctl);
        GRPC_ERROR_UNREF(error);
        return;
      }
    } else {
      return;
    }
  }
}

static void receiving_slice_ready(void* bctlp, grpc_error_handle error) {
  batch_control* bctl = static_cast<batch_control*>(bctlp);
  grpc_call* call = bctl->call;
  bool release_error = false;

  if (error == GRPC_ERROR_NONE) {
    grpc_slice slice;
    error = call->receiving_stream->Pull(&slice);
    if (error == GRPC_ERROR_NONE) {
      grpc_slice_buffer_add(&(*call->receiving_buffer)->data.raw.slice_buffer,
                            slice);
      continue_receiving_slices(bctl);
    } else {
      /* Error returned by ByteStream::Pull() needs to be released manually */
      release_error = true;
    }
  }

  if (error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_trace_operation_failures)) {
      GRPC_LOG_IF_ERROR("receiving_slice_ready", GRPC_ERROR_REF(error));
    }
    call->receiving_stream.reset();
    grpc_byte_buffer_destroy(*call->receiving_buffer);
    *call->receiving_buffer = nullptr;
    call->receiving_message = false;
    finish_batch_step(bctl);
    if (release_error) {
      GRPC_ERROR_UNREF(error);
    }
  }
}

static void process_data_after_md(batch_control* bctl) {
  grpc_call* call = bctl->call;
  if (call->receiving_stream == nullptr) {
    *call->receiving_buffer = nullptr;
    call->receiving_message = false;
    finish_batch_step(bctl);
  } else {
    call->test_only_last_message_flags = call->receiving_stream->flags();
    if ((call->receiving_stream->flags() & GRPC_WRITE_INTERNAL_COMPRESS) &&
        (call->incoming_message_compression_algorithm >
         GRPC_MESSAGE_COMPRESS_NONE)) {
      grpc_compression_algorithm algo;
      GPR_ASSERT(
          grpc_compression_algorithm_from_message_stream_compression_algorithm(
              &algo, call->incoming_message_compression_algorithm,
              (grpc_stream_compression_algorithm)0));
      *call->receiving_buffer =
          grpc_raw_compressed_byte_buffer_create(nullptr, 0, algo);
    } else {
      *call->receiving_buffer = grpc_raw_byte_buffer_create(nullptr, 0);
    }
    GRPC_CLOSURE_INIT(&call->receiving_slice_ready, receiving_slice_ready, bctl,
                      grpc_schedule_on_exec_ctx);
    continue_receiving_slices(bctl);
  }
}

static void receiving_stream_ready(void* bctlp, grpc_error_handle error) {
  batch_control* bctl = static_cast<batch_control*>(bctlp);
  grpc_call* call = bctl->call;
  if (error != GRPC_ERROR_NONE) {
    call->receiving_stream.reset();
    if (bctl->batch_error.ok()) {
      bctl->batch_error.set(error);
    }
    cancel_with_error(call, GRPC_ERROR_REF(error));
  }
  /* If recv_state is RECV_NONE, we will save the batch_control
   * object with rel_cas, and will not use it after the cas. Its corresponding
   * acq_load is in receiving_initial_metadata_ready() */
  if (error != GRPC_ERROR_NONE || call->receiving_stream == nullptr ||
      !gpr_atm_rel_cas(&call->recv_state, RECV_NONE,
                       reinterpret_cast<gpr_atm>(bctlp))) {
    process_data_after_md(bctl);
  }
}

// The recv_message_ready callback used when sending a batch containing
// a recv_message op down the filter stack.  Yields the call combiner
// before processing the received message.
static void receiving_stream_ready_in_call_combiner(void* bctlp,
                                                    grpc_error_handle error) {
  batch_control* bctl = static_cast<batch_control*>(bctlp);
  grpc_call* call = bctl->call;
  GRPC_CALL_COMBINER_STOP(&call->call_combiner, "recv_message_ready");
  receiving_stream_ready(bctlp, error);
}

static void GPR_ATTRIBUTE_NOINLINE
handle_both_stream_and_msg_compression_set(grpc_call* call) {
  std::string error_msg = absl::StrFormat(
      "Incoming stream has both stream compression (%d) and message "
      "compression (%d).",
      call->incoming_stream_compression_algorithm,
      call->incoming_message_compression_algorithm);
  gpr_log(GPR_ERROR, "%s", error_msg.c_str());
  cancel_with_status(call, GRPC_STATUS_INTERNAL, error_msg.c_str());
}

static void GPR_ATTRIBUTE_NOINLINE
handle_error_parsing_compression_algorithm(grpc_call* call) {
  std::string error_msg = absl::StrFormat(
      "Error in incoming message compression (%d) or stream "
      "compression (%d).",
      call->incoming_stream_compression_algorithm,
      call->incoming_message_compression_algorithm);
  cancel_with_status(call, GRPC_STATUS_INTERNAL, error_msg.c_str());
}

static void GPR_ATTRIBUTE_NOINLINE handle_invalid_compression(
    grpc_call* call, grpc_compression_algorithm compression_algorithm) {
  std::string error_msg = absl::StrFormat(
      "Invalid compression algorithm value '%d'.", compression_algorithm);
  gpr_log(GPR_ERROR, "%s", error_msg.c_str());
  cancel_with_status(call, GRPC_STATUS_UNIMPLEMENTED, error_msg.c_str());
}

static void GPR_ATTRIBUTE_NOINLINE handle_compression_algorithm_disabled(
    grpc_call* call, grpc_compression_algorithm compression_algorithm) {
  const char* algo_name = nullptr;
  grpc_compression_algorithm_name(compression_algorithm, &algo_name);
  std::string error_msg =
      absl::StrFormat("Compression algorithm '%s' is disabled.", algo_name);
  gpr_log(GPR_ERROR, "%s", error_msg.c_str());
  cancel_with_status(call, GRPC_STATUS_UNIMPLEMENTED, error_msg.c_str());
}

static void GPR_ATTRIBUTE_NOINLINE handle_compression_algorithm_not_accepted(
    grpc_call* call, grpc_compression_algorithm compression_algorithm) {
  const char* algo_name = nullptr;
  grpc_compression_algorithm_name(compression_algorithm, &algo_name);
  gpr_log(GPR_ERROR,
          "Compression algorithm ('%s') not present in the bitset of "
          "accepted encodings ('0x%x')",
          algo_name, call->encodings_accepted_by_peer);
}

static void validate_filtered_metadata(batch_control* bctl) {
  grpc_compression_algorithm compression_algorithm;
  grpc_call* call = bctl->call;
  if (GPR_UNLIKELY(call->incoming_stream_compression_algorithm !=
                       GRPC_STREAM_COMPRESS_NONE &&
                   call->incoming_message_compression_algorithm !=
                       GRPC_MESSAGE_COMPRESS_NONE)) {
    handle_both_stream_and_msg_compression_set(call);
  } else if (
      GPR_UNLIKELY(
          grpc_compression_algorithm_from_message_stream_compression_algorithm(
              &compression_algorithm,
              call->incoming_message_compression_algorithm,
              call->incoming_stream_compression_algorithm) == 0)) {
    handle_error_parsing_compression_algorithm(call);
  } else {
    const grpc_compression_options compression_options =
        grpc_channel_compression_options(call->channel);
    if (GPR_UNLIKELY(compression_algorithm >= GRPC_COMPRESS_ALGORITHMS_COUNT)) {
      handle_invalid_compression(call, compression_algorithm);
    } else if (GPR_UNLIKELY(
                   grpc_compression_options_is_algorithm_enabled_internal(
                       &compression_options, compression_algorithm) == 0)) {
      /* check if algorithm is supported by current channel config */
      handle_compression_algorithm_disabled(call, compression_algorithm);
    }
    /* GRPC_COMPRESS_NONE is always set. */
    GPR_DEBUG_ASSERT(call->encodings_accepted_by_peer != 0);
    if (GPR_UNLIKELY(!grpc_core::GetBit(call->encodings_accepted_by_peer,
                                        compression_algorithm))) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_compression_trace)) {
        handle_compression_algorithm_not_accepted(call, compression_algorithm);
      }
    }
  }
}

static void receiving_initial_metadata_ready(void* bctlp,
                                             grpc_error_handle error) {
  batch_control* bctl = static_cast<batch_control*>(bctlp);
  grpc_call* call = bctl->call;

  GRPC_CALL_COMBINER_STOP(&call->call_combiner, "recv_initial_metadata_ready");

  if (error == GRPC_ERROR_NONE) {
    grpc_metadata_batch* md =
        &call->metadata_batch[1 /* is_receiving */][0 /* is_trailing */];
    recv_initial_filter(call, md);

    /* TODO(ctiller): this could be moved into recv_initial_filter now */
    GPR_TIMER_SCOPE("validate_filtered_metadata", 0);
    validate_filtered_metadata(bctl);

    grpc_millis deadline = md->deadline();
    if (deadline != GRPC_MILLIS_INF_FUTURE && !call->is_client) {
      call->send_deadline = deadline;
    }
  } else {
    if (bctl->batch_error.ok()) {
      bctl->batch_error.set(error);
    }
    cancel_with_error(call, GRPC_ERROR_REF(error));
  }

  grpc_closure* saved_rsr_closure = nullptr;
  while (true) {
    gpr_atm rsr_bctlp = gpr_atm_acq_load(&call->recv_state);
    /* Should only receive initial metadata once */
    GPR_ASSERT(rsr_bctlp != 1);
    if (rsr_bctlp == 0) {
      /* We haven't seen initial metadata and messages before, thus initial
       * metadata is received first.
       * no_barrier_cas is used, as this function won't access the batch_control
       * object saved by receiving_stream_ready() if the initial metadata is
       * received first. */
      if (gpr_atm_no_barrier_cas(&call->recv_state, RECV_NONE,
                                 RECV_INITIAL_METADATA_FIRST)) {
        break;
      }
    } else {
      /* Already received messages */
      saved_rsr_closure =
          GRPC_CLOSURE_CREATE(receiving_stream_ready, (batch_control*)rsr_bctlp,
                              grpc_schedule_on_exec_ctx);
      /* No need to modify recv_state */
      break;
    }
  }
  if (saved_rsr_closure != nullptr) {
    grpc_core::Closure::Run(DEBUG_LOCATION, saved_rsr_closure,
                            GRPC_ERROR_REF(error));
  }

  finish_batch_step(bctl);
}

static void receiving_trailing_metadata_ready(void* bctlp,
                                              grpc_error_handle error) {
  batch_control* bctl = static_cast<batch_control*>(bctlp);
  grpc_call* call = bctl->call;
  GRPC_CALL_COMBINER_STOP(&call->call_combiner, "recv_trailing_metadata_ready");
  grpc_metadata_batch* md =
      &call->metadata_batch[1 /* is_receiving */][1 /* is_trailing */];
  recv_trailing_filter(call, md, GRPC_ERROR_REF(error));
  finish_batch_step(bctl);
}

static void finish_batch(void* bctlp, grpc_error_handle error) {
  batch_control* bctl = static_cast<batch_control*>(bctlp);
  grpc_call* call = bctl->call;
  GRPC_CALL_COMBINER_STOP(&call->call_combiner, "on_complete");
  if (bctl->batch_error.ok()) {
    bctl->batch_error.set(error);
  }
  if (error != GRPC_ERROR_NONE) {
    cancel_with_error(call, GRPC_ERROR_REF(error));
  }
  finish_batch_step(bctl);
}

static void free_no_op_completion(void* /*p*/, grpc_cq_completion* completion) {
  gpr_free(completion);
}

static grpc_call_error call_start_batch(grpc_call* call, const grpc_op* ops,
                                        size_t nops, void* notify_tag,
                                        int is_notify_tag_closure) {
  GPR_TIMER_SCOPE("call_start_batch", 0);

  size_t i;
  const grpc_op* op;
  batch_control* bctl;
  bool has_send_ops = false;
  int num_recv_ops = 0;
  grpc_call_error error = GRPC_CALL_OK;
  grpc_transport_stream_op_batch* stream_op;
  grpc_transport_stream_op_batch_payload* stream_op_payload;

  GRPC_CALL_LOG_BATCH(GPR_INFO, ops, nops);

  if (nops == 0) {
    if (!is_notify_tag_closure) {
      GPR_ASSERT(grpc_cq_begin_op(call->cq, notify_tag));
      grpc_cq_end_op(call->cq, notify_tag, GRPC_ERROR_NONE,
                     free_no_op_completion, nullptr,
                     static_cast<grpc_cq_completion*>(
                         gpr_malloc(sizeof(grpc_cq_completion))));
    } else {
      grpc_core::Closure::Run(DEBUG_LOCATION,
                              static_cast<grpc_closure*>(notify_tag),
                              GRPC_ERROR_NONE);
    }
    error = GRPC_CALL_OK;
    goto done;
  }

  bctl = reuse_or_allocate_batch_control(call, ops);
  if (bctl == nullptr) {
    return GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
  }
  bctl->completion_data.notify_tag.tag = notify_tag;
  bctl->completion_data.notify_tag.is_closure =
      static_cast<uint8_t>(is_notify_tag_closure != 0);

  stream_op = &bctl->op;
  stream_op_payload = &call->stream_op_payload;

  /* rewrite batch ops into a transport op */
  for (i = 0; i < nops; i++) {
    op = &ops[i];
    if (op->reserved != nullptr) {
      error = GRPC_CALL_ERROR;
      goto done_with_error;
    }
    switch (op->op) {
      case GRPC_OP_SEND_INITIAL_METADATA: {
        /* Flag validation: currently allow no flags */
        if (!are_initial_metadata_flags_valid(op->flags, call->is_client)) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->sent_initial_metadata) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        // TODO(juanlishen): If the user has already specified a compression
        // algorithm by setting the initial metadata with key of
        // GRPC_COMPRESSION_REQUEST_ALGORITHM_MD_KEY, we shouldn't override that
        // with the compression algorithm mapped from compression level.
        /* process compression level */
        grpc_metadata& compression_md = call->compression_md;
        compression_md.key = grpc_empty_slice();
        compression_md.value = grpc_empty_slice();
        size_t additional_metadata_count = 0;
        grpc_compression_level effective_compression_level =
            GRPC_COMPRESS_LEVEL_NONE;
        bool level_set = false;
        if (op->data.send_initial_metadata.maybe_compression_level.is_set) {
          effective_compression_level =
              op->data.send_initial_metadata.maybe_compression_level.level;
          level_set = true;
        } else {
          const grpc_compression_options copts =
              grpc_channel_compression_options(call->channel);
          if (copts.default_level.is_set) {
            level_set = true;
            effective_compression_level = copts.default_level.level;
          }
        }
        // Currently, only server side supports compression level setting.
        if (level_set && !call->is_client) {
          const grpc_compression_algorithm calgo =
              compression_algorithm_for_level_locked(
                  call, effective_compression_level);
          // The following metadata will be checked and removed by the message
          // compression filter. It will be used as the call's compression
          // algorithm.
          compression_md.key = GRPC_MDSTR_GRPC_INTERNAL_ENCODING_REQUEST;
          compression_md.value = grpc_compression_algorithm_slice(calgo);
          additional_metadata_count++;
        }
        if (op->data.send_initial_metadata.count + additional_metadata_count >
            INT_MAX) {
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        stream_op->send_initial_metadata = true;
        call->sent_initial_metadata = true;
        if (!prepare_application_metadata(
                call, static_cast<int>(op->data.send_initial_metadata.count),
                op->data.send_initial_metadata.metadata, 0, call->is_client,
                &compression_md, static_cast<int>(additional_metadata_count))) {
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        /* TODO(ctiller): just make these the same variable? */
        if (call->is_client) {
          call->metadata_batch[0][0].SetDeadline(call->send_deadline);
        }
        stream_op_payload->send_initial_metadata.send_initial_metadata =
            &call->metadata_batch[0 /* is_receiving */][0 /* is_trailing */];
        stream_op_payload->send_initial_metadata.send_initial_metadata_flags =
            op->flags;
        if (call->is_client) {
          stream_op_payload->send_initial_metadata.peer_string =
              &call->peer_string;
        }
        has_send_ops = true;
        break;
      }
      case GRPC_OP_SEND_MESSAGE: {
        if (!are_write_flags_valid(op->flags)) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (op->data.send_message.send_message == nullptr) {
          error = GRPC_CALL_ERROR_INVALID_MESSAGE;
          goto done_with_error;
        }
        if (call->sending_message) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        uint32_t flags = op->flags;
        /* If the outgoing buffer is already compressed, mark it as so in the
           flags. These will be picked up by the compression filter and further
           (wasteful) attempts at compression skipped. */
        if (op->data.send_message.send_message->data.raw.compression >
            GRPC_COMPRESS_NONE) {
          flags |= GRPC_WRITE_INTERNAL_COMPRESS;
        }
        stream_op->send_message = true;
        call->sending_message = true;
        call->sending_stream.Init(
            &op->data.send_message.send_message->data.raw.slice_buffer, flags);
        stream_op_payload->send_message.send_message.reset(
            call->sending_stream.get());
        has_send_ops = true;
        break;
      }
      case GRPC_OP_SEND_CLOSE_FROM_CLIENT: {
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (!call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_SERVER;
          goto done_with_error;
        }
        if (call->sent_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        stream_op->send_trailing_metadata = true;
        call->sent_final_op = true;
        stream_op_payload->send_trailing_metadata.send_trailing_metadata =
            &call->metadata_batch[0 /* is_receiving */][1 /* is_trailing */];
        has_send_ops = true;
        break;
      }
      case GRPC_OP_SEND_STATUS_FROM_SERVER: {
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_CLIENT;
          goto done_with_error;
        }
        if (call->sent_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        if (op->data.send_status_from_server.trailing_metadata_count >
            INT_MAX) {
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        stream_op->send_trailing_metadata = true;
        call->sent_final_op = true;
        GPR_ASSERT(call->send_extra_metadata_count == 0);
        call->send_extra_metadata_count = 1;
        call->send_extra_metadata[0].md = grpc_get_reffed_status_elem(
            op->data.send_status_from_server.status);
        grpc_error_handle status_error =
            op->data.send_status_from_server.status == GRPC_STATUS_OK
                ? GRPC_ERROR_NONE
                : grpc_error_set_int(
                      GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                          "Server returned error"),
                      GRPC_ERROR_INT_GRPC_STATUS,
                      static_cast<intptr_t>(
                          op->data.send_status_from_server.status));
        if (op->data.send_status_from_server.status_details != nullptr) {
          call->send_extra_metadata[1].md = grpc_mdelem_from_slices(
              GRPC_MDSTR_GRPC_MESSAGE,
              grpc_slice_ref_internal(
                  *op->data.send_status_from_server.status_details));
          call->send_extra_metadata_count++;
          if (status_error != GRPC_ERROR_NONE) {
            char* msg = grpc_slice_to_c_string(
                GRPC_MDVALUE(call->send_extra_metadata[1].md));
            status_error =
                grpc_error_set_str(status_error, GRPC_ERROR_STR_GRPC_MESSAGE,
                                   grpc_slice_from_copied_string(msg));
            gpr_free(msg);
          }
        }

        call->status_error.set(status_error);
        GRPC_ERROR_UNREF(status_error);

        if (!prepare_application_metadata(
                call,
                static_cast<int>(
                    op->data.send_status_from_server.trailing_metadata_count),
                op->data.send_status_from_server.trailing_metadata, 1, 1,
                nullptr, 0)) {
          for (int n = 0; n < call->send_extra_metadata_count; n++) {
            GRPC_MDELEM_UNREF(call->send_extra_metadata[n].md);
          }
          call->send_extra_metadata_count = 0;
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        stream_op_payload->send_trailing_metadata.send_trailing_metadata =
            &call->metadata_batch[0 /* is_receiving */][1 /* is_trailing */];
        stream_op_payload->send_trailing_metadata.sent =
            &call->sent_server_trailing_metadata;
        has_send_ops = true;
        break;
      }
      case GRPC_OP_RECV_INITIAL_METADATA: {
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->received_initial_metadata) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->received_initial_metadata = true;
        call->buffered_metadata[0] =
            op->data.recv_initial_metadata.recv_initial_metadata;
        GRPC_CLOSURE_INIT(&call->receiving_initial_metadata_ready,
                          receiving_initial_metadata_ready, bctl,
                          grpc_schedule_on_exec_ctx);
        stream_op->recv_initial_metadata = true;
        stream_op_payload->recv_initial_metadata.recv_initial_metadata =
            &call->metadata_batch[1 /* is_receiving */][0 /* is_trailing */];
        stream_op_payload->recv_initial_metadata.recv_initial_metadata_ready =
            &call->receiving_initial_metadata_ready;
        if (call->is_client) {
          stream_op_payload->recv_initial_metadata.trailing_metadata_available =
              &call->is_trailers_only;
        } else {
          stream_op_payload->recv_initial_metadata.peer_string =
              &call->peer_string;
        }
        ++num_recv_ops;
        break;
      }
      case GRPC_OP_RECV_MESSAGE: {
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->receiving_message) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->receiving_message = true;
        stream_op->recv_message = true;
        call->receiving_buffer = op->data.recv_message.recv_message;
        stream_op_payload->recv_message.recv_message = &call->receiving_stream;
        stream_op_payload->recv_message.call_failed_before_recv_message =
            &call->call_failed_before_recv_message;
        GRPC_CLOSURE_INIT(&call->receiving_stream_ready,
                          receiving_stream_ready_in_call_combiner, bctl,
                          grpc_schedule_on_exec_ctx);
        stream_op_payload->recv_message.recv_message_ready =
            &call->receiving_stream_ready;
        ++num_recv_ops;
        break;
      }
      case GRPC_OP_RECV_STATUS_ON_CLIENT: {
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (!call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_SERVER;
          goto done_with_error;
        }
        if (call->requested_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->requested_final_op = true;
        call->buffered_metadata[1] =
            op->data.recv_status_on_client.trailing_metadata;
        call->final_op.client.status = op->data.recv_status_on_client.status;
        call->final_op.client.status_details =
            op->data.recv_status_on_client.status_details;
        call->final_op.client.error_string =
            op->data.recv_status_on_client.error_string;
        stream_op->recv_trailing_metadata = true;
        stream_op_payload->recv_trailing_metadata.recv_trailing_metadata =
            &call->metadata_batch[1 /* is_receiving */][1 /* is_trailing */];
        stream_op_payload->recv_trailing_metadata.collect_stats =
            &call->final_info.stats.transport_stream_stats;
        GRPC_CLOSURE_INIT(&call->receiving_trailing_metadata_ready,
                          receiving_trailing_metadata_ready, bctl,
                          grpc_schedule_on_exec_ctx);
        stream_op_payload->recv_trailing_metadata.recv_trailing_metadata_ready =
            &call->receiving_trailing_metadata_ready;
        ++num_recv_ops;
        break;
      }
      case GRPC_OP_RECV_CLOSE_ON_SERVER: {
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_CLIENT;
          goto done_with_error;
        }
        if (call->requested_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->requested_final_op = true;
        call->final_op.server.cancelled =
            op->data.recv_close_on_server.cancelled;
        stream_op->recv_trailing_metadata = true;
        stream_op_payload->recv_trailing_metadata.recv_trailing_metadata =
            &call->metadata_batch[1 /* is_receiving */][1 /* is_trailing */];
        stream_op_payload->recv_trailing_metadata.collect_stats =
            &call->final_info.stats.transport_stream_stats;
        GRPC_CLOSURE_INIT(&call->receiving_trailing_metadata_ready,
                          receiving_trailing_metadata_ready, bctl,
                          grpc_schedule_on_exec_ctx);
        stream_op_payload->recv_trailing_metadata.recv_trailing_metadata_ready =
            &call->receiving_trailing_metadata_ready;
        ++num_recv_ops;
        break;
      }
    }
  }

  GRPC_CALL_INTERNAL_REF(call, "completion");
  if (!is_notify_tag_closure) {
    GPR_ASSERT(grpc_cq_begin_op(call->cq, notify_tag));
  }
  bctl->set_num_steps_to_complete((has_send_ops ? 1 : 0) + num_recv_ops);

  if (has_send_ops) {
    GRPC_CLOSURE_INIT(&bctl->finish_batch, finish_batch, bctl,
                      grpc_schedule_on_exec_ctx);
    stream_op->on_complete = &bctl->finish_batch;
  }

  gpr_atm_rel_store(&call->any_ops_sent_atm, 1);
  execute_batch(call, stream_op, &bctl->start_batch);

done:
  return error;

done_with_error:
  /* reverse any mutations that occurred */
  if (stream_op->send_initial_metadata) {
    call->sent_initial_metadata = false;
    call->metadata_batch[0][0].Clear();
  }
  if (stream_op->send_message) {
    call->sending_message = false;
    call->sending_stream->Orphan();
  }
  if (stream_op->send_trailing_metadata) {
    call->sent_final_op = false;
    call->metadata_batch[0][1].Clear();
  }
  if (stream_op->recv_initial_metadata) {
    call->received_initial_metadata = false;
  }
  if (stream_op->recv_message) {
    call->receiving_message = false;
  }
  if (stream_op->recv_trailing_metadata) {
    call->requested_final_op = false;
  }
  goto done;
}

grpc_call_error grpc_call_start_batch(grpc_call* call, const grpc_op* ops,
                                      size_t nops, void* tag, void* reserved) {
  grpc_call_error err;

  GRPC_API_TRACE(
      "grpc_call_start_batch(call=%p, ops=%p, nops=%lu, tag=%p, "
      "reserved=%p)",
      5, (call, ops, (unsigned long)nops, tag, reserved));

  if (reserved != nullptr) {
    err = GRPC_CALL_ERROR;
  } else {
    grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
    grpc_core::ExecCtx exec_ctx;
    err = call_start_batch(call, ops, nops, tag, 0);
  }

  return err;
}

grpc_call_error grpc_call_start_batch_and_execute(grpc_call* call,
                                                  const grpc_op* ops,
                                                  size_t nops,
                                                  grpc_closure* closure) {
  return call_start_batch(call, ops, nops, closure, 1);
}

void grpc_call_context_set(grpc_call* call, grpc_context_index elem,
                           void* value, void (*destroy)(void* value)) {
  if (call->context[elem].destroy) {
    call->context[elem].destroy(call->context[elem].value);
  }
  call->context[elem].value = value;
  call->context[elem].destroy = destroy;
}

void* grpc_call_context_get(grpc_call* call, grpc_context_index elem) {
  return call->context[elem].value;
}

uint8_t grpc_call_is_client(grpc_call* call) { return call->is_client; }

grpc_compression_algorithm grpc_call_compression_for_level(
    grpc_call* call, grpc_compression_level level) {
  grpc_compression_algorithm algo =
      compression_algorithm_for_level_locked(call, level);
  return algo;
}

bool grpc_call_is_trailers_only(const grpc_call* call) {
  bool result = call->is_trailers_only;
  GPR_DEBUG_ASSERT(
      !result ||
      call->metadata_batch[1 /* is_receiving */][0 /* is_trailing */].empty());
  return result;
}

int grpc_call_failed_before_recv_message(const grpc_call* c) {
  return c->call_failed_before_recv_message;
}

const char* grpc_call_error_to_string(grpc_call_error error) {
  switch (error) {
    case GRPC_CALL_ERROR:
      return "GRPC_CALL_ERROR";
    case GRPC_CALL_ERROR_ALREADY_ACCEPTED:
      return "GRPC_CALL_ERROR_ALREADY_ACCEPTED";
    case GRPC_CALL_ERROR_ALREADY_FINISHED:
      return "GRPC_CALL_ERROR_ALREADY_FINISHED";
    case GRPC_CALL_ERROR_ALREADY_INVOKED:
      return "GRPC_CALL_ERROR_ALREADY_INVOKED";
    case GRPC_CALL_ERROR_BATCH_TOO_BIG:
      return "GRPC_CALL_ERROR_BATCH_TOO_BIG";
    case GRPC_CALL_ERROR_INVALID_FLAGS:
      return "GRPC_CALL_ERROR_INVALID_FLAGS";
    case GRPC_CALL_ERROR_INVALID_MESSAGE:
      return "GRPC_CALL_ERROR_INVALID_MESSAGE";
    case GRPC_CALL_ERROR_INVALID_METADATA:
      return "GRPC_CALL_ERROR_INVALID_METADATA";
    case GRPC_CALL_ERROR_NOT_INVOKED:
      return "GRPC_CALL_ERROR_NOT_INVOKED";
    case GRPC_CALL_ERROR_NOT_ON_CLIENT:
      return "GRPC_CALL_ERROR_NOT_ON_CLIENT";
    case GRPC_CALL_ERROR_NOT_ON_SERVER:
      return "GRPC_CALL_ERROR_NOT_ON_SERVER";
    case GRPC_CALL_ERROR_NOT_SERVER_COMPLETION_QUEUE:
      return "GRPC_CALL_ERROR_NOT_SERVER_COMPLETION_QUEUE";
    case GRPC_CALL_ERROR_PAYLOAD_TYPE_MISMATCH:
      return "GRPC_CALL_ERROR_PAYLOAD_TYPE_MISMATCH";
    case GRPC_CALL_ERROR_TOO_MANY_OPERATIONS:
      return "GRPC_CALL_ERROR_TOO_MANY_OPERATIONS";
    case GRPC_CALL_ERROR_COMPLETION_QUEUE_SHUTDOWN:
      return "GRPC_CALL_ERROR_COMPLETION_QUEUE_SHUTDOWN";
    case GRPC_CALL_OK:
      return "GRPC_CALL_OK";
  }
  GPR_UNREACHABLE_CODE(return "GRPC_CALL_ERROR_UNKNOW");
}
