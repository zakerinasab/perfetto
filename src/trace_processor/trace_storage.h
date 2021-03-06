/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
#define SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_

#include <array>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/metadata.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/tables/android_tables.h"
#include "src/trace_processor/tables/counter_tables.h"
#include "src/trace_processor/tables/metadata_tables.h"
#include "src/trace_processor/tables/profiler_tables.h"
#include "src/trace_processor/tables/slice_tables.h"
#include "src/trace_processor/tables/track_tables.h"
#include "src/trace_processor/variadic.h"

namespace perfetto {
namespace trace_processor {

// UniquePid is an offset into |unique_processes_|. This is necessary because
// Unix pids are reused and thus not guaranteed to be unique over a long
// period of time.
using UniquePid = uint32_t;

// UniqueTid is an offset into |unique_threads_|. Necessary because tids can
// be reused.
using UniqueTid = uint32_t;

// StringId is an offset into |string_pool_|.
using StringId = StringPool::Id;
static const StringId kNullStringId = StringId(0);

// Identifiers for all the tables in the database.
enum class TableId : uint8_t {
  kInvalid = 0,
  kCounterValues = 1,
  kRawEvents = 2,
  kInstants = 3,
  kSched = 4,
  kNestableSlices = 5,
  kMetadataTable = 6,
  kTrack = 7,
  kVulkanMemoryAllocation = 8,
};

using ArgSetId = uint32_t;
static const ArgSetId kInvalidArgSetId = 0;

using TrackId = tables::TrackTable::Id;

using CounterId = tables::CounterTable::Id;

using SliceId = tables::SliceTable::Id;

using InstantId = tables::InstantTable::Id;

using MappingId = tables::StackProfileMappingTable::Id;

using MetadataId = tables::MetadataTable::Id;

// TODO(lalitm): this is a temporary hack while migrating the counters table and
// will be removed when the migration is complete.
static const TrackId kInvalidTrackId =
    TrackId(std::numeric_limits<TrackId>::max());

enum class RefType {
  kRefNoRef = 0,
  kRefUtid = 1,
  kRefCpuId = 2,
  kRefIrq = 3,
  kRefSoftIrq = 4,
  kRefUpid = 5,
  kRefGpuId = 6,
  kRefTrack = 7,
  kRefMax
};

const std::vector<NullTermStringView>& GetRefTypeStringMap();

// Stores a data inside a trace file in a columnar form. This makes it efficient
// to read or search across a single field of the trace (e.g. all the thread
// names for a given CPU).
class TraceStorage {
 public:
  TraceStorage(const Config& = Config());

  virtual ~TraceStorage();

  // Information about a unique process seen in a trace.
  struct Process {
    explicit Process(uint32_t p) : pid(p) {}
    int64_t start_ns = 0;
    int64_t end_ns = 0;
    StringId name_id = 0;
    uint32_t pid = 0;
    base::Optional<UniquePid> parent_upid;
    base::Optional<uint32_t> uid;
  };

  // Information about a unique thread seen in a trace.
  struct Thread {
    explicit Thread(uint32_t t) : tid(t) {}
    int64_t start_ns = 0;
    int64_t end_ns = 0;
    StringId name_id = 0;
    base::Optional<UniquePid> upid;
    uint32_t tid = 0;
  };

  // Generic key value storage which can be referenced by other tables.
  class Args {
   public:
    struct Arg {
      StringId flat_key = 0;
      StringId key = 0;
      Variadic value = Variadic::Integer(0);

      TableId table;
      uint32_t row;
    };

    struct ArgHasher {
      uint64_t operator()(const Arg& arg) const noexcept {
        base::Hash hash;
        hash.Update(arg.key);
        // We don't hash arg.flat_key because it's a subsequence of arg.key.
        switch (arg.value.type) {
          case Variadic::Type::kInt:
            hash.Update(arg.value.int_value);
            break;
          case Variadic::Type::kUint:
            hash.Update(arg.value.uint_value);
            break;
          case Variadic::Type::kString:
            hash.Update(arg.value.string_value);
            break;
          case Variadic::Type::kReal:
            hash.Update(arg.value.real_value);
            break;
          case Variadic::Type::kPointer:
            hash.Update(arg.value.pointer_value);
            break;
          case Variadic::Type::kBool:
            hash.Update(arg.value.bool_value);
            break;
          case Variadic::Type::kJson:
            hash.Update(arg.value.json_value);
            break;
        }
        return hash.digest();
      }
    };

    const std::deque<ArgSetId>& set_ids() const { return set_ids_; }
    const std::deque<StringId>& flat_keys() const { return flat_keys_; }
    const std::deque<StringId>& keys() const { return keys_; }
    const std::deque<Variadic>& arg_values() const { return arg_values_; }
    uint32_t args_count() const {
      return static_cast<uint32_t>(set_ids_.size());
    }

    ArgSetId AddArgSet(const std::vector<Arg>& args,
                       uint32_t begin,
                       uint32_t end) {
      base::Hash hash;
      for (uint32_t i = begin; i < end; i++) {
        hash.Update(ArgHasher()(args[i]));
      }

      ArgSetHash digest = hash.digest();
      auto it = arg_row_for_hash_.find(digest);
      if (it != arg_row_for_hash_.end()) {
        return set_ids_[it->second];
      }

      // The +1 ensures that nothing has an id == kInvalidArgSetId == 0.
      ArgSetId id = static_cast<uint32_t>(arg_row_for_hash_.size()) + 1;
      arg_row_for_hash_.emplace(digest, args_count());
      for (uint32_t i = begin; i < end; i++) {
        const auto& arg = args[i];
        set_ids_.emplace_back(id);
        flat_keys_.emplace_back(arg.flat_key);
        keys_.emplace_back(arg.key);
        arg_values_.emplace_back(arg.value);
      }
      return id;
    }

   private:
    using ArgSetHash = uint64_t;

    std::deque<ArgSetId> set_ids_;
    std::deque<StringId> flat_keys_;
    std::deque<StringId> keys_;
    std::deque<Variadic> arg_values_;

    std::unordered_map<ArgSetHash, uint32_t> arg_row_for_hash_;
  };

  class Slices {
   public:
    inline size_t AddSlice(uint32_t cpu,
                           int64_t start_ns,
                           int64_t duration_ns,
                           UniqueTid utid,
                           ftrace_utils::TaskState end_state,
                           int32_t priority) {
      cpus_.emplace_back(cpu);
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);
      utids_.emplace_back(utid);
      end_states_.emplace_back(end_state);
      priorities_.emplace_back(priority);

      if (utid >= rows_for_utids_.size())
        rows_for_utids_.resize(utid + 1);
      rows_for_utids_[utid].emplace_back(slice_count() - 1);
      return slice_count() - 1;
    }

    void set_duration(size_t index, int64_t duration_ns) {
      durations_[index] = duration_ns;
    }

    void set_end_state(size_t index, ftrace_utils::TaskState end_state) {
      end_states_[index] = end_state;
    }

    size_t slice_count() const { return start_ns_.size(); }

    const std::deque<uint32_t>& cpus() const { return cpus_; }

    const std::deque<int64_t>& start_ns() const { return start_ns_; }

    const std::deque<int64_t>& durations() const { return durations_; }

    const std::deque<UniqueTid>& utids() const { return utids_; }

    const std::deque<ftrace_utils::TaskState>& end_state() const {
      return end_states_;
    }

    const std::deque<int32_t>& priorities() const { return priorities_; }

    const std::deque<std::vector<uint32_t>>& rows_for_utids() const {
      return rows_for_utids_;
    }

   private:
    // Each deque below has the same number of entries (the number of slices
    // in the trace for the CPU).
    std::deque<uint32_t> cpus_;
    std::deque<int64_t> start_ns_;
    std::deque<int64_t> durations_;
    std::deque<UniqueTid> utids_;
    std::deque<ftrace_utils::TaskState> end_states_;
    std::deque<int32_t> priorities_;

    // One row per utid.
    std::deque<std::vector<uint32_t>> rows_for_utids_;
  };

  class ThreadSlices {
   public:
    inline uint32_t AddThreadSlice(uint32_t slice_id,
                                   int64_t thread_timestamp_ns,
                                   int64_t thread_duration_ns,
                                   int64_t thread_instruction_count,
                                   int64_t thread_instruction_delta) {
      slice_ids_.emplace_back(slice_id);
      thread_timestamp_ns_.emplace_back(thread_timestamp_ns);
      thread_duration_ns_.emplace_back(thread_duration_ns);
      thread_instruction_counts_.emplace_back(thread_instruction_count);
      thread_instruction_deltas_.emplace_back(thread_instruction_delta);
      return slice_count() - 1;
    }

    uint32_t slice_count() const {
      return static_cast<uint32_t>(slice_ids_.size());
    }

    const std::deque<uint32_t>& slice_ids() const { return slice_ids_; }
    const std::deque<int64_t>& thread_timestamp_ns() const {
      return thread_timestamp_ns_;
    }
    const std::deque<int64_t>& thread_duration_ns() const {
      return thread_duration_ns_;
    }
    const std::deque<int64_t>& thread_instruction_counts() const {
      return thread_instruction_counts_;
    }
    const std::deque<int64_t>& thread_instruction_deltas() const {
      return thread_instruction_deltas_;
    }

    base::Optional<uint32_t> FindRowForSliceId(uint32_t slice_id) const {
      auto it =
          std::lower_bound(slice_ids().begin(), slice_ids().end(), slice_id);
      if (it != slice_ids().end() && *it == slice_id) {
        return static_cast<uint32_t>(std::distance(slice_ids().begin(), it));
      }
      return base::nullopt;
    }

    void UpdateThreadDeltasForSliceId(uint32_t slice_id,
                                      int64_t end_thread_timestamp_ns,
                                      int64_t end_thread_instruction_count) {
      uint32_t row = *FindRowForSliceId(slice_id);
      int64_t begin_ns = thread_timestamp_ns_[row];
      thread_duration_ns_[row] = end_thread_timestamp_ns - begin_ns;
      int64_t begin_ticount = thread_instruction_counts_[row];
      thread_instruction_deltas_[row] =
          end_thread_instruction_count - begin_ticount;
    }

   private:
    std::deque<uint32_t> slice_ids_;
    std::deque<int64_t> thread_timestamp_ns_;
    std::deque<int64_t> thread_duration_ns_;
    std::deque<int64_t> thread_instruction_counts_;
    std::deque<int64_t> thread_instruction_deltas_;
  };

  class VirtualTrackSlices {
   public:
    inline uint32_t AddVirtualTrackSlice(uint32_t slice_id,
                                         int64_t thread_timestamp_ns,
                                         int64_t thread_duration_ns,
                                         int64_t thread_instruction_count,
                                         int64_t thread_instruction_delta) {
      slice_ids_.emplace_back(slice_id);
      thread_timestamp_ns_.emplace_back(thread_timestamp_ns);
      thread_duration_ns_.emplace_back(thread_duration_ns);
      thread_instruction_counts_.emplace_back(thread_instruction_count);
      thread_instruction_deltas_.emplace_back(thread_instruction_delta);
      return slice_count() - 1;
    }

    uint32_t slice_count() const {
      return static_cast<uint32_t>(slice_ids_.size());
    }

    const std::deque<uint32_t>& slice_ids() const { return slice_ids_; }
    const std::deque<int64_t>& thread_timestamp_ns() const {
      return thread_timestamp_ns_;
    }
    const std::deque<int64_t>& thread_duration_ns() const {
      return thread_duration_ns_;
    }
    const std::deque<int64_t>& thread_instruction_counts() const {
      return thread_instruction_counts_;
    }
    const std::deque<int64_t>& thread_instruction_deltas() const {
      return thread_instruction_deltas_;
    }

    base::Optional<uint32_t> FindRowForSliceId(uint32_t slice_id) const {
      auto it =
          std::lower_bound(slice_ids().begin(), slice_ids().end(), slice_id);
      if (it != slice_ids().end() && *it == slice_id) {
        return static_cast<uint32_t>(std::distance(slice_ids().begin(), it));
      }
      return base::nullopt;
    }

    void UpdateThreadDeltasForSliceId(uint32_t slice_id,
                                      int64_t end_thread_timestamp_ns,
                                      int64_t end_thread_instruction_count) {
      uint32_t row = *FindRowForSliceId(slice_id);
      int64_t begin_ns = thread_timestamp_ns_[row];
      thread_duration_ns_[row] = end_thread_timestamp_ns - begin_ns;
      int64_t begin_ticount = thread_instruction_counts_[row];
      thread_instruction_deltas_[row] =
          end_thread_instruction_count - begin_ticount;
    }

   private:
    std::deque<uint32_t> slice_ids_;
    std::deque<int64_t> thread_timestamp_ns_;
    std::deque<int64_t> thread_duration_ns_;
    std::deque<int64_t> thread_instruction_counts_;
    std::deque<int64_t> thread_instruction_deltas_;
  };

  class SqlStats {
   public:
    static constexpr size_t kMaxLogEntries = 100;
    uint32_t RecordQueryBegin(const std::string& query,
                              int64_t time_queued,
                              int64_t time_started);
    void RecordQueryFirstNext(uint32_t row, int64_t time_first_next);
    void RecordQueryEnd(uint32_t row, int64_t time_end);
    size_t size() const { return queries_.size(); }
    const std::deque<std::string>& queries() const { return queries_; }
    const std::deque<int64_t>& times_queued() const { return times_queued_; }
    const std::deque<int64_t>& times_started() const { return times_started_; }
    const std::deque<int64_t>& times_first_next() const {
      return times_first_next_;
    }
    const std::deque<int64_t>& times_ended() const { return times_ended_; }

   private:
    uint32_t popped_queries_ = 0;

    std::deque<std::string> queries_;
    std::deque<int64_t> times_queued_;
    std::deque<int64_t> times_started_;
    std::deque<int64_t> times_first_next_;
    std::deque<int64_t> times_ended_;
  };

  class RawEvents {
   public:
    inline uint32_t AddRawEvent(int64_t timestamp,
                                StringId name_id,
                                uint32_t cpu,
                                UniqueTid utid) {
      timestamps_.emplace_back(timestamp);
      name_ids_.emplace_back(name_id);
      cpus_.emplace_back(cpu);
      utids_.emplace_back(utid);
      arg_set_ids_.emplace_back(kInvalidArgSetId);
      return static_cast<uint32_t>(raw_event_count() - 1);
    }

    void set_arg_set_id(uint32_t row, ArgSetId id) { arg_set_ids_[row] = id; }

    size_t raw_event_count() const { return timestamps_.size(); }

    const std::deque<int64_t>& timestamps() const { return timestamps_; }

    const std::deque<StringId>& name_ids() const { return name_ids_; }

    const std::deque<uint32_t>& cpus() const { return cpus_; }

    const std::deque<UniqueTid>& utids() const { return utids_; }

    const std::deque<ArgSetId>& arg_set_ids() const { return arg_set_ids_; }

   private:
    std::deque<int64_t> timestamps_;
    std::deque<StringId> name_ids_;
    std::deque<uint32_t> cpus_;
    std::deque<UniqueTid> utids_;
    std::deque<ArgSetId> arg_set_ids_;
  };

  struct Stats {
    using IndexMap = std::map<int, int64_t>;
    int64_t value = 0;
    IndexMap indexed_values;
  };
  using StatsMap = std::array<Stats, stats::kNumKeys>;

  UniqueTid AddEmptyThread(uint32_t tid) {
    unique_threads_.emplace_back(tid);
    return static_cast<UniqueTid>(unique_threads_.size() - 1);
  }

  UniquePid AddEmptyProcess(uint32_t pid) {
    unique_processes_.emplace_back(pid);
    return static_cast<UniquePid>(unique_processes_.size() - 1);
  }

  // Return an unqiue identifier for the contents of each string.
  // The string is copied internally and can be destroyed after this called.
  // Virtual for testing.
  virtual StringId InternString(base::StringView str) {
    return string_pool_.InternString(str);
  }

  Process* GetMutableProcess(UniquePid upid) {
    PERFETTO_DCHECK(upid < unique_processes_.size());
    return &unique_processes_[upid];
  }

  Thread* GetMutableThread(UniqueTid utid) {
    PERFETTO_DCHECK(utid < unique_threads_.size());
    return &unique_threads_[utid];
  }

  // Example usage: SetStats(stats::android_log_num_failed, 42);
  void SetStats(size_t key, int64_t value) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
    stats_[key].value = value;
  }

  // Example usage: IncrementStats(stats::android_log_num_failed, -1);
  void IncrementStats(size_t key, int64_t increment = 1) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
    stats_[key].value += increment;
  }

  // Example usage: IncrementIndexedStats(stats::cpu_failure, 1);
  void IncrementIndexedStats(size_t key, int index, int64_t increment = 1) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kIndexed);
    stats_[key].indexed_values[index] += increment;
  }

  // Example usage: SetIndexedStats(stats::cpu_failure, 1, 42);
  void SetIndexedStats(size_t key, int index, int64_t value) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kIndexed);
    stats_[key].indexed_values[index] = value;
  }

  class ScopedStatsTracer {
   public:
    ScopedStatsTracer(TraceStorage* storage, size_t key)
        : storage_(storage), key_(key), start_ns_(base::GetWallTimeNs()) {}

    ~ScopedStatsTracer() {
      if (!storage_)
        return;
      auto delta_ns = base::GetWallTimeNs() - start_ns_;
      storage_->IncrementStats(key_, delta_ns.count());
    }

    ScopedStatsTracer(ScopedStatsTracer&& other) noexcept { MoveImpl(&other); }

    ScopedStatsTracer& operator=(ScopedStatsTracer&& other) {
      MoveImpl(&other);
      return *this;
    }

   private:
    ScopedStatsTracer(const ScopedStatsTracer&) = delete;
    ScopedStatsTracer& operator=(const ScopedStatsTracer&) = delete;

    void MoveImpl(ScopedStatsTracer* other) {
      storage_ = other->storage_;
      key_ = other->key_;
      start_ns_ = other->start_ns_;
      other->storage_ = nullptr;
    }

    TraceStorage* storage_;
    size_t key_;
    base::TimeNanos start_ns_;
  };

  ScopedStatsTracer TraceExecutionTimeIntoStats(size_t key) {
    return ScopedStatsTracer(this, key);
  }

  // Reading methods.
  // Virtual for testing.
  virtual NullTermStringView GetString(StringId id) const {
    return string_pool_.Get(id);
  }

  const Process& GetProcess(UniquePid upid) const {
    PERFETTO_DCHECK(upid < unique_processes_.size());
    return unique_processes_[upid];
  }

  // Virtual for testing.
  virtual const Thread& GetThread(UniqueTid utid) const {
    // Allow utid == 0 for idle thread retrieval.
    PERFETTO_DCHECK(utid < unique_threads_.size());
    return unique_threads_[utid];
  }

  const tables::TrackTable& track_table() const { return track_table_; }
  tables::TrackTable* mutable_track_table() { return &track_table_; }

  const tables::ProcessTrackTable& process_track_table() const {
    return process_track_table_;
  }
  tables::ProcessTrackTable* mutable_process_track_table() {
    return &process_track_table_;
  }

  const tables::ThreadTrackTable& thread_track_table() const {
    return thread_track_table_;
  }
  tables::ThreadTrackTable* mutable_thread_track_table() {
    return &thread_track_table_;
  }

  const tables::CounterTrackTable& counter_track_table() const {
    return counter_track_table_;
  }
  tables::CounterTrackTable* mutable_counter_track_table() {
    return &counter_track_table_;
  }

  const tables::ThreadCounterTrackTable& thread_counter_track_table() const {
    return thread_counter_track_table_;
  }
  tables::ThreadCounterTrackTable* mutable_thread_counter_track_table() {
    return &thread_counter_track_table_;
  }

  const tables::ProcessCounterTrackTable& process_counter_track_table() const {
    return process_counter_track_table_;
  }
  tables::ProcessCounterTrackTable* mutable_process_counter_track_table() {
    return &process_counter_track_table_;
  }

  const tables::CpuCounterTrackTable& cpu_counter_track_table() const {
    return cpu_counter_track_table_;
  }
  tables::CpuCounterTrackTable* mutable_cpu_counter_track_table() {
    return &cpu_counter_track_table_;
  }

  const tables::IrqCounterTrackTable& irq_counter_track_table() const {
    return irq_counter_track_table_;
  }
  tables::IrqCounterTrackTable* mutable_irq_counter_track_table() {
    return &irq_counter_track_table_;
  }

  const tables::SoftirqCounterTrackTable& softirq_counter_track_table() const {
    return softirq_counter_track_table_;
  }
  tables::SoftirqCounterTrackTable* mutable_softirq_counter_track_table() {
    return &softirq_counter_track_table_;
  }

  const tables::GpuCounterTrackTable& gpu_counter_track_table() const {
    return gpu_counter_track_table_;
  }
  tables::GpuCounterTrackTable* mutable_gpu_counter_track_table() {
    return &gpu_counter_track_table_;
  }

  const Slices& slices() const { return slices_; }
  Slices* mutable_slices() { return &slices_; }

  const tables::SliceTable& slice_table() const { return slice_table_; }
  tables::SliceTable* mutable_slice_table() { return &slice_table_; }

  const ThreadSlices& thread_slices() const { return thread_slices_; }
  ThreadSlices* mutable_thread_slices() { return &thread_slices_; }

  const VirtualTrackSlices& virtual_track_slices() const {
    return virtual_track_slices_;
  }
  VirtualTrackSlices* mutable_virtual_track_slices() {
    return &virtual_track_slices_;
  }

  const tables::GpuSliceTable& gpu_slice_table() const {
    return gpu_slice_table_;
  }
  tables::GpuSliceTable* mutable_gpu_slice_table() { return &gpu_slice_table_; }

  const tables::CounterTable& counter_table() const { return counter_table_; }
  tables::CounterTable* mutable_counter_table() { return &counter_table_; }

  const SqlStats& sql_stats() const { return sql_stats_; }
  SqlStats* mutable_sql_stats() { return &sql_stats_; }

  const tables::InstantTable& instant_table() const { return instant_table_; }
  tables::InstantTable* mutable_instant_table() { return &instant_table_; }

  const tables::AndroidLogTable& android_log_table() const {
    return android_log_table_;
  }
  tables::AndroidLogTable* mutable_android_log_table() {
    return &android_log_table_;
  }

  const StatsMap& stats() const { return stats_; }

  const tables::MetadataTable& metadata_table() const {
    return metadata_table_;
  }
  tables::MetadataTable* mutable_metadata_table() { return &metadata_table_; }

  const Args& args() const { return args_; }
  Args* mutable_args() { return &args_; }

  const RawEvents& raw_events() const { return raw_events_; }
  RawEvents* mutable_raw_events() { return &raw_events_; }

  const tables::StackProfileMappingTable& stack_profile_mapping_table() const {
    return stack_profile_mapping_table_;
  }
  tables::StackProfileMappingTable* mutable_stack_profile_mapping_table() {
    return &stack_profile_mapping_table_;
  }

  const tables::StackProfileFrameTable& stack_profile_frame_table() const {
    return stack_profile_frame_table_;
  }
  tables::StackProfileFrameTable* mutable_stack_profile_frame_table() {
    return &stack_profile_frame_table_;
  }

  const tables::StackProfileCallsiteTable& stack_profile_callsite_table()
      const {
    return stack_profile_callsite_table_;
  }
  tables::StackProfileCallsiteTable* mutable_stack_profile_callsite_table() {
    return &stack_profile_callsite_table_;
  }

  const tables::HeapProfileAllocationTable& heap_profile_allocation_table()
      const {
    return heap_profile_allocation_table_;
  }
  tables::HeapProfileAllocationTable* mutable_heap_profile_allocation_table() {
    return &heap_profile_allocation_table_;
  }
  const tables::CpuProfileStackSampleTable& cpu_profile_stack_sample_table()
      const {
    return cpu_profile_stack_sample_table_;
  }
  tables::CpuProfileStackSampleTable* mutable_cpu_profile_stack_sample_table() {
    return &cpu_profile_stack_sample_table_;
  }

  const tables::SymbolTable& symbol_table() const { return symbol_table_; }

  tables::SymbolTable* mutable_symbol_table() { return &symbol_table_; }

  const tables::HeapGraphObjectTable& heap_graph_object_table() const {
    return heap_graph_object_table_;
  }

  tables::HeapGraphObjectTable* mutable_heap_graph_object_table() {
    return &heap_graph_object_table_;
  }

  const tables::HeapGraphReferenceTable& heap_graph_reference_table() const {
    return heap_graph_reference_table_;
  }

  tables::HeapGraphReferenceTable* mutable_heap_graph_reference_table() {
    return &heap_graph_reference_table_;
  }

  const tables::GpuTrackTable& gpu_track_table() const {
    return gpu_track_table_;
  }
  tables::GpuTrackTable* mutable_gpu_track_table() { return &gpu_track_table_; }

  const tables::VulkanMemoryAllocationsTable& vulkan_memory_allocations_table()
      const {
    return vulkan_memory_allocations_table_;
  }

  tables::VulkanMemoryAllocationsTable*
  mutable_vulkan_memory_allocations_table() {
    return &vulkan_memory_allocations_table_;
  }

  const StringPool& string_pool() const { return string_pool_; }

  // |unique_processes_| always contains at least 1 element because the 0th ID
  // is reserved to indicate an invalid process.
  size_t process_count() const { return unique_processes_.size(); }

  // |unique_threads_| always contains at least 1 element because the 0th ID
  // is reserved to indicate an invalid thread.
  size_t thread_count() const { return unique_threads_.size(); }

  // Number of interned strings in the pool. Includes the empty string w/ ID=0.
  size_t string_count() const { return string_pool_.size(); }

  // Start / end ts (in nanoseconds) across the parsed trace events.
  // Returns (0, 0) if the trace is empty.
  std::pair<int64_t, int64_t> GetTraceTimestampBoundsNs() const;

  // TODO(lalitm): remove this when we have a better home.
  std::vector<int64_t> FindMappingRow(StringId name, StringId build_id) const {
    auto it = stack_profile_mapping_index_.find(std::make_pair(name, build_id));
    if (it == stack_profile_mapping_index_.end())
      return {};
    return it->second;
  }

  // TODO(lalitm): remove this when we have a better home.
  void InsertMappingRow(StringId name, StringId build_id, uint32_t row) {
    auto pair = std::make_pair(name, build_id);
    stack_profile_mapping_index_[pair].emplace_back(row);
  }

  // TODO(lalitm): remove this when we have a better home.
  std::vector<int64_t> FindFrameRow(size_t mapping_row, uint64_t rel_pc) const {
    auto it =
        stack_profile_frame_index_.find(std::make_pair(mapping_row, rel_pc));
    if (it == stack_profile_frame_index_.end())
      return {};
    return it->second;
  }

  // TODO(lalitm): remove this when we have a better home.
  void InsertFrameRow(size_t mapping_row, uint64_t rel_pc, uint32_t row) {
    auto pair = std::make_pair(mapping_row, rel_pc);
    stack_profile_frame_index_[pair].emplace_back(row);
  }

 private:
  using StringHash = uint64_t;

  TraceStorage(const TraceStorage&) = delete;
  TraceStorage& operator=(const TraceStorage&) = delete;

  TraceStorage(TraceStorage&&) = delete;
  TraceStorage& operator=(TraceStorage&&) = delete;

  // TODO(lalitm): remove this when we find a better home for this.
  using MappingKey = std::pair<StringId /* name */, StringId /* build id */>;
  std::map<MappingKey, std::vector<int64_t>> stack_profile_mapping_index_;

  // TODO(lalitm): remove this when we find a better home for this.
  using FrameKey = std::pair<size_t /* mapping row */, uint64_t /* rel_pc */>;
  std::map<MappingKey, std::vector<int64_t>> stack_profile_frame_index_;

  // One entry for each unique string in the trace.
  StringPool string_pool_;

  // Stats about parsing the trace.
  StatsMap stats_{};

  // Extra data extracted from the trace. Includes:
  // * metadata from chrome and benchmarking infrastructure
  // * descriptions of android packages
  tables::MetadataTable metadata_table_{&string_pool_, nullptr};

  // Metadata for tracks.
  tables::TrackTable track_table_{&string_pool_, nullptr};
  tables::GpuTrackTable gpu_track_table_{&string_pool_, &track_table_};
  tables::ProcessTrackTable process_track_table_{&string_pool_, &track_table_};
  tables::ThreadTrackTable thread_track_table_{&string_pool_, &track_table_};

  // Track tables for counter events.
  tables::CounterTrackTable counter_track_table_{&string_pool_, &track_table_};
  tables::ThreadCounterTrackTable thread_counter_track_table_{
      &string_pool_, &counter_track_table_};
  tables::ProcessCounterTrackTable process_counter_track_table_{
      &string_pool_, &counter_track_table_};
  tables::CpuCounterTrackTable cpu_counter_track_table_{&string_pool_,
                                                        &counter_track_table_};
  tables::IrqCounterTrackTable irq_counter_track_table_{&string_pool_,
                                                        &counter_track_table_};
  tables::SoftirqCounterTrackTable softirq_counter_track_table_{
      &string_pool_, &counter_track_table_};
  tables::GpuCounterTrackTable gpu_counter_track_table_{&string_pool_,
                                                        &counter_track_table_};

  // One entry for each CPU in the trace.
  Slices slices_;

  // Args for all other tables.
  Args args_;

  // One entry for each UniquePid, with UniquePid as the index.
  // Never hold on to pointers to Process, as vector resize will
  // invalidate them.
  std::vector<Process> unique_processes_;

  // One entry for each UniqueTid, with UniqueTid as the index.
  std::deque<Thread> unique_threads_;

  // Slices coming from userspace events (e.g. Chromium TRACE_EVENT macros).
  tables::SliceTable slice_table_{&string_pool_, nullptr};

  // Additional attributes for threads slices (sub-type of NestableSlices).
  ThreadSlices thread_slices_;

  // Additional attributes for virtual track slices (sub-type of
  // NestableSlices).
  VirtualTrackSlices virtual_track_slices_;

  // Additional attributes for gpu track slices (sub-type of
  // NestableSlices).
  tables::GpuSliceTable gpu_slice_table_{&string_pool_, nullptr};

  // The values from the Counter events from the trace. This includes CPU
  // frequency events as well systrace trace_marker counter events.
  tables::CounterTable counter_table_{&string_pool_, nullptr};

  SqlStats sql_stats_;

  // These are instantaneous events in the trace. They have no duration
  // and do not have a value that make sense to track over time.
  // e.g. signal events
  tables::InstantTable instant_table_{&string_pool_, nullptr};

  // Raw events are every ftrace event in the trace. The raw event includes
  // the timestamp and the pid. The args for the raw event will be in the
  // args table. This table can be used to generate a text version of the
  // trace.
  RawEvents raw_events_;
  tables::AndroidLogTable android_log_table_{&string_pool_, nullptr};

  tables::StackProfileMappingTable stack_profile_mapping_table_{&string_pool_,
                                                                nullptr};
  tables::StackProfileFrameTable stack_profile_frame_table_{&string_pool_,
                                                            nullptr};
  tables::StackProfileCallsiteTable stack_profile_callsite_table_{&string_pool_,
                                                                  nullptr};
  tables::HeapProfileAllocationTable heap_profile_allocation_table_{
      &string_pool_, nullptr};
  tables::CpuProfileStackSampleTable cpu_profile_stack_sample_table_{
      &string_pool_, nullptr};

  // Symbol tables (mappings from frames to symbol names)
  tables::SymbolTable symbol_table_{&string_pool_, nullptr};
  tables::HeapGraphObjectTable heap_graph_object_table_{&string_pool_, nullptr};
  tables::HeapGraphReferenceTable heap_graph_reference_table_{&string_pool_,
                                                              nullptr};

  tables::VulkanMemoryAllocationsTable vulkan_memory_allocations_table_{
      &string_pool_, nullptr};
};

}  // namespace trace_processor
}  // namespace perfetto

namespace std {

template <>
struct hash<::perfetto::trace_processor::TrackId> {
  using argument_type = ::perfetto::trace_processor::TrackId;
  using result_type = size_t;

  result_type operator()(const argument_type& r) const {
    return std::hash<uint32_t>{}(r.value);
  }
};

template <>
struct hash<::perfetto::trace_processor::tables::StackProfileFrameTable::Row> {
  using argument_type =
      ::perfetto::trace_processor::tables::StackProfileFrameTable::Row;
  using result_type = size_t;

  result_type operator()(const argument_type& r) const {
    return std::hash<::perfetto::trace_processor::StringId>{}(r.name) ^
           std::hash<int64_t>{}(r.mapping) ^ std::hash<int64_t>{}(r.rel_pc);
  }
};

template <>
struct hash<
    ::perfetto::trace_processor::tables::StackProfileCallsiteTable::Row> {
  using argument_type =
      ::perfetto::trace_processor::tables::StackProfileCallsiteTable::Row;
  using result_type = size_t;

  result_type operator()(const argument_type& r) const {
    return std::hash<int64_t>{}(r.depth) ^ std::hash<int64_t>{}(r.parent_id) ^
           std::hash<int64_t>{}(r.frame_id);
  }
};

template <>
struct hash<
    ::perfetto::trace_processor::tables::StackProfileMappingTable::Row> {
  using argument_type =
      ::perfetto::trace_processor::tables::StackProfileMappingTable::Row;
  using result_type = size_t;

  result_type operator()(const argument_type& r) const {
    return std::hash<::perfetto::trace_processor::StringId>{}(r.build_id) ^
           std::hash<int64_t>{}(r.exact_offset) ^
           std::hash<int64_t>{}(r.start_offset) ^
           std::hash<int64_t>{}(r.start) ^ std::hash<int64_t>{}(r.end) ^
           std::hash<int64_t>{}(r.load_bias) ^
           std::hash<::perfetto::trace_processor::StringId>{}(r.name);
  }
};

}  // namespace std

#endif  // SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
