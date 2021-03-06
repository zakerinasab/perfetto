/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/args_tracker.h"

#include <algorithm>

namespace perfetto {
namespace trace_processor {

ArgsTracker::ArgsTracker(TraceProcessorContext* context) : context_(context) {}

ArgsTracker::~ArgsTracker() {
  Flush();
}

void ArgsTracker::AddArg(TableId table,
                         uint32_t row,
                         StringId flat_key,
                         StringId key,
                         Variadic value) {
  args_.emplace_back();

  auto* rid_arg = &args_.back();
  rid_arg->table = table;
  rid_arg->row = row;
  rid_arg->flat_key = flat_key;
  rid_arg->key = key;
  rid_arg->value = value;
}

void ArgsTracker::Flush() {
  using Arg = TraceStorage::Args::Arg;

  if (args_.empty())
    return;

  // We sort here because a single packet may add multiple args with different
  // rowids.
  auto comparator = [](const Arg& f, const Arg& s) {
    return f.table < s.table && f.row < s.row;
  };
  std::stable_sort(args_.begin(), args_.end(), comparator);

  auto* storage = context_->storage.get();
  for (uint32_t i = 0; i < args_.size();) {
    const auto& arg = args_[i];
    auto table_id = arg.table;
    auto row = arg.row;

    uint32_t next_rid_idx = i + 1;
    while (next_rid_idx < args_.size() &&
           table_id == args_[next_rid_idx].table &&
           row == args_[next_rid_idx].row) {
      next_rid_idx++;
    }

    ArgSetId set_id =
        storage->mutable_args()->AddArgSet(args_, i, next_rid_idx);
    switch (table_id) {
      case TableId::kRawEvents:
        storage->mutable_raw_events()->set_arg_set_id(row, set_id);
        break;
      case TableId::kCounterValues:
        storage->mutable_counter_table()->mutable_arg_set_id()->Set(row,
                                                                    set_id);
        break;
      case TableId::kInstants:
        storage->mutable_instant_table()->mutable_arg_set_id()->Set(row,
                                                                    set_id);
        break;
      case TableId::kNestableSlices:
        storage->mutable_slice_table()->mutable_arg_set_id()->Set(row, set_id);
        break;
      // Special case: overwrites the metadata table row.
      case TableId::kMetadataTable:
        storage->mutable_metadata_table()->mutable_int_value()->Set(row,
                                                                    set_id);
        break;
      case TableId::kTrack:
        storage->mutable_track_table()->mutable_source_arg_set_id()->Set(
            row, set_id);
        break;
      case TableId::kVulkanMemoryAllocation:
        storage->mutable_vulkan_memory_allocations_table()
            ->mutable_arg_set_id()
            ->Set(row, set_id);
        break;
      case TableId::kInvalid:
      case TableId::kSched:
        PERFETTO_FATAL("Unsupported table to insert args into");
    }
    i = next_rid_idx;
  }
  args_.clear();
}

ArgsTracker::BoundInserter::~BoundInserter() {}

}  // namespace trace_processor
}  // namespace perfetto
