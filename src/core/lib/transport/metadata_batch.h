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

#ifndef GRPC_CORE_LIB_TRANSPORT_METADATA_BATCH_H
#define GRPC_CORE_LIB_TRANSPORT_METADATA_BATCH_H

#include <grpc/support/port_platform.h>

#include <stdbool.h>

#include "absl/strings/str_join.h"
#include "absl/types/optional.h"

#include <grpc/grpc.h>
#include <grpc/slice.h>
#include <grpc/support/time.h>

#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/transport/metadata.h"
#include "src/core/lib/transport/static_metadata.h"

typedef struct grpc_linked_mdelem {
  grpc_linked_mdelem() {}

  grpc_mdelem md;
  struct grpc_linked_mdelem* next = nullptr;
  struct grpc_linked_mdelem* prev = nullptr;
  void* reserved;
} grpc_linked_mdelem;

typedef struct grpc_mdelem_list {
  size_t count;
  size_t default_count;  // Number of default keys.
  grpc_linked_mdelem* head;
  grpc_linked_mdelem* tail;
} grpc_mdelem_list;

struct grpc_filtered_mdelem {
  grpc_error_handle error;
  grpc_mdelem md;
};
#define GRPC_FILTERED_ERROR(error) \
  { (error), GRPC_MDNULL }
#define GRPC_FILTERED_MDELEM(md) \
  { GRPC_ERROR_NONE, (md) }
#define GRPC_FILTERED_REMOVE() \
  { GRPC_ERROR_NONE, GRPC_MDNULL }

grpc_error_handle grpc_attach_md_to_error(grpc_error_handle src,
                                          grpc_mdelem md);

namespace grpc_core {

// MetadataMap encodes the mapping of metadata keys to metadata values.
// Right now the API presented is the minimal one that will allow us to
// substitute this type for grpc_metadata_batch in a relatively easy fashion. At
// that point we'll start iterating this API into something that's ergonomic
// again, whilst minimally holding the performance bar already set (and
// hopefully improving some things).
// In the meantime, we're not going to invest much time in ephemeral API
// documentation, so if you must use one of these API's and it's not obvious
// how, reach out to ctiller.
template <typename... TraitsIgnored>
class MetadataMap {
 public:
  MetadataMap();
  ~MetadataMap();

  MetadataMap(const MetadataMap&) = delete;
  MetadataMap& operator=(const MetadataMap&) = delete;
  MetadataMap(MetadataMap&&) noexcept;
  MetadataMap& operator=(MetadataMap&&) noexcept;

  template <typename Encoder>
  void Encode(Encoder* encoder) const {
    for (auto* l = list_.head; l; l = l->next) {
      encoder->Encode(l->md);
    }
    if (deadline_ != GRPC_MILLIS_INF_FUTURE) encoder->EncodeDeadline(deadline_);
  }

  template <typename F>
  void ForEach(F f) const {
    for (auto* l = list_.head; l; l = l->next) {
      f(l->md);
    }
  }

  template <typename F>
  grpc_error_handle Filter(F f, const char* composite_error_string) {
    grpc_linked_mdelem* l = list_.head;
    grpc_error_handle error = GRPC_ERROR_NONE;
    auto add_error = [&](grpc_error_handle new_error) {
      if (new_error == GRPC_ERROR_NONE) return;
      if (error == GRPC_ERROR_NONE) {
        error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(composite_error_string);
      }
      error = grpc_error_add_child(error, new_error);
    };
    while (l) {
      grpc_linked_mdelem* next = l->next;
      grpc_filtered_mdelem new_mdelem = f(l->md);
      add_error(new_mdelem.error);
      if (GRPC_MDISNULL(new_mdelem.md)) {
        Remove(l);
      } else if (new_mdelem.md.payload != l->md.payload) {
        add_error(Substitute(l, new_mdelem.md));
      }
      l = next;
    }
    return error;
  }

  // Set key to value if it exists and return true, otherwise return false.
  bool ReplaceIfExists(grpc_slice key, grpc_slice value);

  void Clear();
  bool empty() const { return count() == 0; }

  size_t count() const {
    return list_.count + (deadline_ == GRPC_MILLIS_INF_FUTURE ? 0 : 1);
  }
  size_t non_deadline_count() const { return list_.count; }
  size_t default_count() const { return list_.default_count; }

  size_t TransportSize() const;

  void Remove(grpc_linked_mdelem* storage);
  void Remove(grpc_metadata_batch_callouts_index idx);

  absl::optional<grpc_slice> Remove(grpc_slice key);

  grpc_error_handle Substitute(grpc_linked_mdelem* storage,
                               grpc_mdelem new_mdelem);

  absl::optional<absl::string_view> GetValue(
      absl::string_view target_key, std::string* concatenated_value) const;

  grpc_error_handle LinkHead(grpc_linked_mdelem* storage) GRPC_MUST_USE_RESULT;
  grpc_error_handle LinkHead(grpc_linked_mdelem* storage,
                             grpc_metadata_batch_callouts_index idx)
      GRPC_MUST_USE_RESULT;
  grpc_error_handle LinkTail(grpc_linked_mdelem* storage) GRPC_MUST_USE_RESULT;
  grpc_error_handle LinkTail(grpc_linked_mdelem* storage,
                             grpc_metadata_batch_callouts_index idx)
      GRPC_MUST_USE_RESULT;

  grpc_error_handle AddHead(grpc_linked_mdelem* storage) GRPC_MUST_USE_RESULT;
  grpc_error_handle AddHead(grpc_linked_mdelem* storage,
                            grpc_mdelem elem_to_add) GRPC_MUST_USE_RESULT;
  grpc_error_handle AddTail(grpc_linked_mdelem* storage) GRPC_MUST_USE_RESULT;
  grpc_error_handle AddTail(grpc_linked_mdelem* storage,
                            grpc_mdelem elem_to_add) GRPC_MUST_USE_RESULT;

  void CopyFrom(MetadataMap* src, grpc_linked_mdelem* storage);

#ifndef NDEBUG
  void AssertOk();
#else
  void AssertOk() {}
#endif

  grpc_millis deadline() const { return deadline_; }
  void SetDeadline(grpc_millis deadline) { deadline_ = deadline; }
  void ClearDeadline() { SetDeadline(GRPC_MILLIS_INF_FUTURE); }

  const grpc_metadata_batch_callouts* legacy_index() const { return &idx_; }

 private:
  void AssertValidCallouts();
  grpc_error_handle LinkCallout(grpc_linked_mdelem* storage,
                                grpc_metadata_batch_callouts_index idx)
      GRPC_MUST_USE_RESULT;
  grpc_error_handle MaybeLinkCallout(grpc_linked_mdelem* storage)
      GRPC_MUST_USE_RESULT;
  void MaybeUnlinkCallout(grpc_linked_mdelem* storage);

  static void assert_valid_list(grpc_mdelem_list* list) {
#ifndef NDEBUG
    grpc_linked_mdelem* l;

    GPR_ASSERT((list->head == nullptr) == (list->tail == nullptr));
    if (!list->head) return;
    GPR_ASSERT(list->head->prev == nullptr);
    GPR_ASSERT(list->tail->next == nullptr);
    GPR_ASSERT((list->head == list->tail) == (list->head->next == nullptr));

    size_t verified_count = 0;
    for (l = list->head; l; l = l->next) {
      GPR_ASSERT(!GRPC_MDISNULL(l->md));
      GPR_ASSERT((l->prev == nullptr) == (l == list->head));
      GPR_ASSERT((l->next == nullptr) == (l == list->tail));
      if (l->next) GPR_ASSERT(l->next->prev == l);
      if (l->prev) GPR_ASSERT(l->prev->next == l);
      verified_count++;
    }
    GPR_ASSERT(list->count == verified_count);
#else
    // Avoid unused-parameter warning for debug-only parameter
    (void)list;
#endif /* NDEBUG */
  }

  static grpc_error_handle GPR_ATTRIBUTE_NOINLINE
  error_with_md(grpc_mdelem md) {
    return grpc_attach_md_to_error(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("Unallowed duplicate metadata"),
        md);
  }

  static void link_head(grpc_mdelem_list* list, grpc_linked_mdelem* storage) {
    assert_valid_list(list);
    GPR_DEBUG_ASSERT(!GRPC_MDISNULL(storage->md));
    storage->prev = nullptr;
    storage->next = list->head;
    storage->reserved = nullptr;
    if (list->head != nullptr) {
      list->head->prev = storage;
    } else {
      list->tail = storage;
    }
    list->head = storage;
    list->count++;
    assert_valid_list(list);
  }

  static void link_tail(grpc_mdelem_list* list, grpc_linked_mdelem* storage) {
    assert_valid_list(list);
    GPR_DEBUG_ASSERT(!GRPC_MDISNULL(storage->md));
    storage->prev = list->tail;
    storage->next = nullptr;
    storage->reserved = nullptr;
    if (list->tail != nullptr) {
      list->tail->next = storage;
    } else {
      list->head = storage;
    }
    list->tail = storage;
    list->count++;
    assert_valid_list(list);
  }

  static void unlink_storage(grpc_mdelem_list* list,
                             grpc_linked_mdelem* storage) {
    assert_valid_list(list);
    if (storage->prev != nullptr) {
      storage->prev->next = storage->next;
    } else {
      list->head = storage->next;
    }
    if (storage->next != nullptr) {
      storage->next->prev = storage->prev;
    } else {
      list->tail = storage->prev;
    }
    list->count--;
    assert_valid_list(list);
  }

  /** Metadata elements in this batch */
  grpc_mdelem_list list_;
  grpc_metadata_batch_callouts idx_;
  /** Used to calculate grpc-timeout at the point of sending,
      or GRPC_MILLIS_INF_FUTURE if this batch does not need to send a
      grpc-timeout */
  grpc_millis deadline_;
};

template <typename... Traits>
void MetadataMap<Traits...>::AssertValidCallouts() {
#ifndef NDEBUG
  for (grpc_linked_mdelem* l = list_.head; l != nullptr; l = l->next) {
    grpc_slice key_interned = grpc_slice_intern(GRPC_MDKEY(l->md));
    grpc_metadata_batch_callouts_index callout_idx =
        GRPC_BATCH_INDEX_OF(key_interned);
    if (callout_idx != GRPC_BATCH_CALLOUTS_COUNT) {
      GPR_ASSERT(idx_.array[callout_idx] == l);
    }
    grpc_slice_unref_internal(key_interned);
  }
#endif
}

#ifndef NDEBUG
template <typename... Traits>
void MetadataMap<Traits...>::AssertOk() {
  assert_valid_list(&list_);
}
#endif /* NDEBUG */

template <typename... Traits>
MetadataMap<Traits...>::MetadataMap() {
  memset(&list_, 0, sizeof(list_));
  memset(&idx_, 0, sizeof(idx_));
  deadline_ = GRPC_MILLIS_INF_FUTURE;
}

template <typename... Traits>
MetadataMap<Traits...>::MetadataMap(MetadataMap&& other) noexcept {
  list_ = other.list_;
  idx_ = other.idx_;
  deadline_ = other.deadline_;
  memset(&other.list_, 0, sizeof(list_));
  memset(&other.idx_, 0, sizeof(idx_));
  other.deadline_ = GRPC_MILLIS_INF_FUTURE;
}

template <typename... Traits>
MetadataMap<Traits...>& MetadataMap<Traits...>::operator=(
    MetadataMap&& other) noexcept {
  Clear();
  list_ = other.list_;
  idx_ = other.idx_;
  deadline_ = other.deadline_;
  memset(&other.list_, 0, sizeof(list_));
  memset(&other.idx_, 0, sizeof(idx_));
  other.deadline_ = GRPC_MILLIS_INF_FUTURE;
  return *this;
}

template <typename... Traits>
MetadataMap<Traits...>::~MetadataMap() {
  AssertValidCallouts();
  for (auto* l = list_.head; l; l = l->next) {
    GRPC_MDELEM_UNREF(l->md);
  }
}

template <typename... Traits>
absl::optional<grpc_slice> MetadataMap<Traits...>::Remove(grpc_slice key) {
  for (auto* l = list_.head; l; l = l->next) {
    if (grpc_slice_eq(GRPC_MDKEY(l->md), key)) {
      auto out = grpc_slice_ref_internal(GRPC_MDVALUE(l->md));
      Remove(l);
      return out;
    }
  }
  return {};
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::LinkCallout(
    grpc_linked_mdelem* storage, grpc_metadata_batch_callouts_index idx) {
  AssertValidCallouts();
  GPR_DEBUG_ASSERT(idx >= 0 && idx < GRPC_BATCH_CALLOUTS_COUNT);
  if (GPR_LIKELY(idx_.array[idx] == nullptr)) {
    ++list_.default_count;
    idx_.array[idx] = storage;
    AssertValidCallouts();
    return GRPC_ERROR_NONE;
  }
  AssertValidCallouts();
  return error_with_md(storage->md);
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::MaybeLinkCallout(
    grpc_linked_mdelem* storage) {
  grpc_metadata_batch_callouts_index idx =
      GRPC_BATCH_INDEX_OF(GRPC_MDKEY(storage->md));
  if (idx == GRPC_BATCH_CALLOUTS_COUNT) {
    return GRPC_ERROR_NONE;
  }
  return LinkCallout(storage, idx);
}

template <typename... Traits>
void MetadataMap<Traits...>::MaybeUnlinkCallout(grpc_linked_mdelem* storage) {
  grpc_metadata_batch_callouts_index idx =
      GRPC_BATCH_INDEX_OF(GRPC_MDKEY(storage->md));
  if (idx == GRPC_BATCH_CALLOUTS_COUNT) {
    return;
  }
  --list_.default_count;
  GPR_DEBUG_ASSERT(idx_.array[idx] != nullptr);
  idx_.array[idx] = nullptr;
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::AddHead(grpc_linked_mdelem* storage,
                                                  grpc_mdelem elem_to_add) {
  GPR_DEBUG_ASSERT(!GRPC_MDISNULL(elem_to_add));
  storage->md = elem_to_add;
  return LinkHead(storage);
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::LinkHead(
    grpc_linked_mdelem* storage) {
  AssertValidCallouts();
  grpc_error_handle err = MaybeLinkCallout(storage);
  if (err != GRPC_ERROR_NONE) {
    AssertValidCallouts();
    return err;
  }
  link_head(&list_, storage);
  AssertValidCallouts();
  return GRPC_ERROR_NONE;
}

// TODO(arjunroy): Need to revisit this and see what guarantees exist between
// C-core and the internal-metadata subsystem. E.g. can we ensure a particular
// metadata is never added twice, even in the presence of user supplied data?
template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::LinkHead(
    grpc_linked_mdelem* storage, grpc_metadata_batch_callouts_index idx) {
  GPR_DEBUG_ASSERT(GRPC_BATCH_INDEX_OF(GRPC_MDKEY(storage->md)) == idx);
  AssertValidCallouts();
  grpc_error_handle err = LinkCallout(storage, idx);
  if (GPR_UNLIKELY(err != GRPC_ERROR_NONE)) {
    AssertValidCallouts();
    return err;
  }
  link_head(&list_, storage);
  AssertValidCallouts();
  return GRPC_ERROR_NONE;
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::AddTail(grpc_linked_mdelem* storage,
                                                  grpc_mdelem elem_to_add) {
  GPR_DEBUG_ASSERT(!GRPC_MDISNULL(elem_to_add));
  storage->md = elem_to_add;
  return LinkTail(storage);
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::LinkTail(
    grpc_linked_mdelem* storage) {
  AssertValidCallouts();
  grpc_error_handle err = MaybeLinkCallout(storage);
  if (err != GRPC_ERROR_NONE) {
    AssertValidCallouts();
    return err;
  }
  link_tail(&list_, storage);
  AssertValidCallouts();
  return GRPC_ERROR_NONE;
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::LinkTail(
    grpc_linked_mdelem* storage, grpc_metadata_batch_callouts_index idx) {
  GPR_DEBUG_ASSERT(GRPC_BATCH_INDEX_OF(GRPC_MDKEY(storage->md)) == idx);
  AssertValidCallouts();
  grpc_error_handle err = LinkCallout(storage, idx);
  if (GPR_UNLIKELY(err != GRPC_ERROR_NONE)) {
    AssertValidCallouts();
    return err;
  }
  link_tail(&list_, storage);
  AssertValidCallouts();
  return GRPC_ERROR_NONE;
}

template <typename... Traits>
void MetadataMap<Traits...>::Remove(grpc_linked_mdelem* storage) {
  AssertValidCallouts();
  MaybeUnlinkCallout(storage);
  unlink_storage(&list_, storage);
  GRPC_MDELEM_UNREF(storage->md);
  AssertValidCallouts();
}

template <typename... Traits>
void MetadataMap<Traits...>::Remove(grpc_metadata_batch_callouts_index idx) {
  AssertValidCallouts();
  if (idx_.array[idx] == nullptr) return;
  --list_.default_count;
  unlink_storage(&list_, idx_.array[idx]);
  GRPC_MDELEM_UNREF(idx_.array[idx]->md);
  idx_.array[idx] = nullptr;
  AssertValidCallouts();
}

template <typename... Traits>
absl::optional<absl::string_view> MetadataMap<Traits...>::GetValue(
    absl::string_view target_key, std::string* concatenated_value) const {
  // Find all values for the specified key.
  absl::InlinedVector<absl::string_view, 1> values;
  for (grpc_linked_mdelem* md = list_.head; md != nullptr; md = md->next) {
    absl::string_view key = grpc_core::StringViewFromSlice(GRPC_MDKEY(md->md));
    absl::string_view value =
        grpc_core::StringViewFromSlice(GRPC_MDVALUE(md->md));
    if (target_key == key) values.push_back(value);
  }
  // If none found, no match.
  if (values.empty()) return absl::nullopt;
  // If exactly one found, return it as-is.
  if (values.size() == 1) return values.front();
  // If more than one found, concatenate the values, using
  // *concatenated_values as a temporary holding place for the
  // concatenated string.
  *concatenated_value = absl::StrJoin(values, ",");
  return *concatenated_value;
}

template <typename... Traits>
grpc_error_handle MetadataMap<Traits...>::Substitute(
    grpc_linked_mdelem* storage, grpc_mdelem new_mdelem) {
  AssertValidCallouts();
  grpc_error_handle error = GRPC_ERROR_NONE;
  grpc_mdelem old_mdelem = storage->md;
  if (!grpc_slice_eq(GRPC_MDKEY(new_mdelem), GRPC_MDKEY(old_mdelem))) {
    MaybeUnlinkCallout(storage);
    storage->md = new_mdelem;
    error = MaybeLinkCallout(storage);
    if (error != GRPC_ERROR_NONE) {
      unlink_storage(&list_, storage);
      GRPC_MDELEM_UNREF(storage->md);
    }
  } else {
    storage->md = new_mdelem;
  }
  GRPC_MDELEM_UNREF(old_mdelem);
  AssertValidCallouts();
  return error;
}

template <typename... Traits>
void MetadataMap<Traits...>::Clear() {
  this->~MetadataMap();
  new (this) MetadataMap();
}

template <typename... Traits>
size_t MetadataMap<Traits...>::TransportSize() const {
  size_t size = 0;
  for (grpc_linked_mdelem* elem = list_.head; elem != nullptr;
       elem = elem->next) {
    size += GRPC_MDELEM_LENGTH(elem->md);
  }
  return size;
}

template <typename... Traits>
bool MetadataMap<Traits...>::ReplaceIfExists(grpc_slice key, grpc_slice value) {
  AssertValidCallouts();
  for (grpc_linked_mdelem* l = list_.head; l != nullptr; l = l->next) {
    if (grpc_slice_eq(GRPC_MDKEY(l->md), key)) {
      auto new_mdelem = grpc_mdelem_from_slices(grpc_slice_ref_internal(key),
                                                grpc_slice_ref_internal(value));
      GRPC_MDELEM_UNREF(l->md);
      l->md = new_mdelem;
      AssertValidCallouts();
      return true;
    }
  }
  AssertValidCallouts();
  return false;
}

}  // namespace grpc_core

using grpc_metadata_batch = grpc_core::MetadataMap<>;

inline void grpc_metadata_batch_clear(grpc_metadata_batch* batch) {
  batch->Clear();
}
inline bool grpc_metadata_batch_is_empty(grpc_metadata_batch* batch) {
  return batch->empty();
}

/* Returns the transport size of the batch. */
inline size_t grpc_metadata_batch_size(grpc_metadata_batch* batch) {
  return batch->TransportSize();
}

/** Remove \a storage from the batch, unreffing the mdelem contained */
inline void grpc_metadata_batch_remove(grpc_metadata_batch* batch,
                                       grpc_linked_mdelem* storage) {
  batch->Remove(storage);
}
inline void grpc_metadata_batch_remove(grpc_metadata_batch* batch,
                                       grpc_metadata_batch_callouts_index idx) {
  batch->Remove(idx);
}

/** Substitute a new mdelem for an old value */
inline grpc_error_handle grpc_metadata_batch_substitute(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_mdelem new_mdelem) {
  return batch->Substitute(storage, new_mdelem);
}

void grpc_metadata_batch_set_value(grpc_linked_mdelem* storage,
                                   const grpc_slice& value);

/** Returns metadata value(s) for the specified key.
    If the key is not present in the batch, returns absl::nullopt.
    If the key is present exactly once in the batch, returns a string_view of
    that value.
    If the key is present more than once in the batch, constructs a
    comma-concatenated string of all values in concatenated_value and returns a
    string_view of that string. */
inline absl::optional<absl::string_view> grpc_metadata_batch_get_value(
    grpc_metadata_batch* batch, absl::string_view target_key,
    std::string* concatenated_value) {
  return batch->GetValue(target_key, concatenated_value);
}

/** Add \a storage to the beginning of \a batch. storage->md is
    assumed to be valid.
    \a storage is owned by the caller and must survive for the
    lifetime of batch. This usually means it should be around
    for the lifetime of the call. */
inline GRPC_MUST_USE_RESULT grpc_error_handle grpc_metadata_batch_link_head(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage) {
  return batch->LinkHead(storage);
}

inline GRPC_MUST_USE_RESULT grpc_error_handle grpc_metadata_batch_link_head(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_metadata_batch_callouts_index idx) {
  return batch->LinkHead(storage, idx);
}

/** Add \a storage to the end of \a batch. storage->md is
    assumed to be valid.
    \a storage is owned by the caller and must survive for the
    lifetime of batch. This usually means it should be around
    for the lifetime of the call. */
inline GRPC_MUST_USE_RESULT grpc_error_handle grpc_metadata_batch_link_tail(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage) {
  return batch->LinkTail(storage);
}

inline GRPC_MUST_USE_RESULT grpc_error_handle grpc_metadata_batch_link_tail(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_metadata_batch_callouts_index idx) {
  return batch->LinkTail(storage, idx);
}

/** Add \a elem_to_add as the first element in \a batch, using
    \a storage as backing storage for the linked list element.
    \a storage is owned by the caller and must survive for the
    lifetime of batch. This usually means it should be around
    for the lifetime of the call.
    Takes ownership of \a elem_to_add */
inline grpc_error_handle grpc_metadata_batch_add_head(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_mdelem elem_to_add) {
  return batch->AddHead(storage, elem_to_add);
}

// TODO(arjunroy, roth): Remove redundant methods.
// add/link_head/tail are almost identical.
inline grpc_error_handle GRPC_MUST_USE_RESULT grpc_metadata_batch_add_head(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_metadata_batch_callouts_index idx) {
  return grpc_metadata_batch_link_head(batch, storage, idx);
}

inline grpc_error_handle GRPC_MUST_USE_RESULT grpc_metadata_batch_add_head(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_mdelem elem_to_add, grpc_metadata_batch_callouts_index idx) {
  GPR_DEBUG_ASSERT(!GRPC_MDISNULL(elem_to_add));
  storage->md = elem_to_add;
  return grpc_metadata_batch_add_head(batch, storage, idx);
}

/** Add \a elem_to_add as the last element in \a batch, using
    \a storage as backing storage for the linked list element.
    \a storage is owned by the caller and must survive for the
    lifetime of batch. This usually means it should be around
    for the lifetime of the call.
    Takes ownership of \a elem_to_add */
inline GRPC_MUST_USE_RESULT grpc_error_handle grpc_metadata_batch_add_tail(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_mdelem elem_to_add) {
  return batch->AddTail(storage, elem_to_add);
}

inline grpc_error_handle GRPC_MUST_USE_RESULT grpc_metadata_batch_add_tail(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_metadata_batch_callouts_index idx) {
  return grpc_metadata_batch_link_tail(batch, storage, idx);
}

inline grpc_error_handle GRPC_MUST_USE_RESULT grpc_metadata_batch_add_tail(
    grpc_metadata_batch* batch, grpc_linked_mdelem* storage,
    grpc_mdelem elem_to_add, grpc_metadata_batch_callouts_index idx) {
  GPR_DEBUG_ASSERT(!GRPC_MDISNULL(elem_to_add));
  storage->md = elem_to_add;
  return grpc_metadata_batch_add_tail(batch, storage, idx);
}

typedef grpc_filtered_mdelem (*grpc_metadata_batch_filter_func)(
    void* user_data, grpc_mdelem elem);
inline GRPC_MUST_USE_RESULT grpc_error_handle grpc_metadata_batch_filter(
    grpc_metadata_batch* batch, grpc_metadata_batch_filter_func func,
    void* user_data, const char* composite_error_string) {
  return batch->Filter([=](grpc_mdelem elem) { return func(user_data, elem); },
                       composite_error_string);
}

inline void grpc_metadata_batch_assert_ok(grpc_metadata_batch* batch) {
  batch->AssertOk();
}

/// Copies \a src to \a dst.  \a storage must point to an array of
/// \a grpc_linked_mdelem structs of at least the same size as \a src.
///
/// For each mdelem in \a src, if the mdelem is of storage types
/// GRPC_MDELEM_STORAGE_INTERNED or GRPC_MDELEM_STORAGE_ALLOCATED,
/// refs the original mdelem for the copy.  Otherwise, makes a new
/// mdelem that will hold its own refs to the key and value slices.
///
/// Currently used only in the retry code.
void grpc_metadata_batch_copy(grpc_metadata_batch* src,
                              grpc_metadata_batch* dst,
                              grpc_linked_mdelem* storage);

#endif /* GRPC_CORE_LIB_TRANSPORT_METADATA_BATCH_H */
