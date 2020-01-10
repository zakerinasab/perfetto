-- Copyright 2019 Google LLC.
-- SPDX-License-Identifier: Apache-2.0

CREATE VIEW top_five_processes_by_cpu AS
SELECT
  process.name as process_name,
  CAST(SUM(sched.dur) / 1e6 as INT64) as cpu_time_ms,
  COUNT(DISTINCT utid) as num_threads
FROM sched
INNER JOIN thread USING(utid)
INNER JOIN process USING(upid)
GROUP BY process.name
ORDER BY cpu_time_ms DESC
LIMIT 5;

CREATE VIEW top_five_processes_output AS
SELECT TopFiveProcesses(
  'process_info', (
    SELECT RepeatedField(
      ProcessInfo(
        'process_name', process_name,
        'cpu_time_ms', cpu_time_ms,
        'num_threads', num_threads
      )
    )
    FROM top_five_processes_by_cpu
  )
);
