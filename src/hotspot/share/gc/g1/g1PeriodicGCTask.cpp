/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1PeriodicGCTask.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"

// [Skipswap] Get process CPU time
jlong G1PeriodicGCTask::get_process_cpu_time_ns() {
  if (os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(CLOCK_PROCESS_CPUTIME_ID);
  }
  return -1;
}

// [Skipswap] Check current CPU utilization
double G1PeriodicGCTask::check_cpu_util() {
  int available_cores = os::initial_active_processor_count();;
  uint conc_workers_needed = ConcGCThreads;
  
  if (available_cores <= 0 || conc_workers_needed == 0) {
    // Can't determine, assume CPU available
    return .0;
  }
  
  // Get current wall time and process CPU time
  double current_wall_time_sec = os::elapsedTime();
  jlong current_cpu_time_ns = get_process_cpu_time_ns();
  
  if (current_cpu_time_ns < 0) {
    // CPU time not supported, fall back to simple check
    log_info(gc, periodic)("CPU time not supported, skipping CPU check");
    return .0;
  }
  
  if (!_cpu_tracking_initialized) {
    // First call: initialize tracking
    _last_wall_time_sec = current_wall_time_sec;
    _last_process_cpu_time_ns = current_cpu_time_ns;
    _cpu_tracking_initialized = true;
    log_info(gc, periodic)("CPU tracking initialized. Wall time: %.3f, CPU time: "
                           JLONG_FORMAT " ns. Available cores: %d",
                           current_wall_time_sec, current_cpu_time_ns,
                           available_cores);
    // On first call, assume CPU available (need at least one interval to calculate)
    return .0;
  }
  
  // Calculate CPU utilization since last check
  double wall_time_delta_sec = current_wall_time_sec - _last_wall_time_sec;
  jlong cpu_time_delta_ns = current_cpu_time_ns - _last_process_cpu_time_ns;
  
  if (wall_time_delta_sec <= 0.0) {
    // Invalid time delta, skip this check
    log_info(gc, periodic)("Invalid wall time delta: %.6f sec. Skipping CPU check",
                           wall_time_delta_sec);
    return .0;
  }
  
  // Calculate CPU utilization: (CPU time delta) / (wall time delta * number of cores)
  // This gives us the average number of cores being used
  double cpu_time_delta_sec = (double)cpu_time_delta_ns / 1e9;
  double used_cores = cpu_time_delta_sec / wall_time_delta_sec;
  double util = used_cores / available_cores;
  
  // Update tracking for next check
  _last_wall_time_sec = current_wall_time_sec;
  _last_process_cpu_time_ns = current_cpu_time_ns;
  
  log_info(gc, periodic)("CPU util check: Wall delta: %.3f sec, CPU delta: %.3f sec, "
                          "Util: %.2f%%, Used cores: %.2f",
                          wall_time_delta_sec, cpu_time_delta_sec,
                          util * 100.0, used_cores);
  
  return util;
}

// [Skipswap] Record CPU availability in history
void G1PeriodicGCTask::record_cpu_util(double util) {
  _cpu_util_history[_history_index] = util;
  _history_index = (_history_index + 1) % CPU_UTIL_HISTORY_SIZE;
  if (_history_count < CPU_UTIL_HISTORY_SIZE) {
    _history_count++;
  }
}

// [Skipswap] Check if we have sufficient CPU util in history
bool G1PeriodicGCTask::has_sufficient_cpu_history(uint required_count) const {
  if (_history_count < required_count) {
    // Not enough history yet, be conservative
    return false;
  }

  // Count how many of the last N checks had sufficient CPU
  uint available_count = 0;
  for (uint i = 0; i < required_count; i++) {
    // Calculate index going backwards from current position
    uint idx = (_history_index + CPU_UTIL_HISTORY_SIZE - required_count + i) % CPU_UTIL_HISTORY_SIZE;
    if (_cpu_util_history[idx] <= 0.8) {
      available_count++;
    }
  }
  
  // Require all checks to have available CPU
  return (available_count == required_count);
}

// [Skipswap] Check if we have CPU pressure in history
bool G1PeriodicGCTask::is_cpu_pressure_high_history(uint required_count) const {
  if (_history_count < required_count) {
    // Not enough history yet, be conservative
    return false;
  }

  // Count how many of the last N checks had sufficient CPU
  uint pressure_count = 0;
  for (uint i = 0; i < required_count; i++) {
    // Calculate index going backwards from current position
    uint idx = (_history_index + CPU_UTIL_HISTORY_SIZE - required_count + i) % CPU_UTIL_HISTORY_SIZE;
    if (_cpu_util_history[idx] >= 0.99) {
      pressure_count++;
    }
  }
  
  // Require all checks to have available CPU
  return (pressure_count == required_count);
}

bool G1PeriodicGCTask::has_sufficient_cpu() {  
  // Decision based on history: require CPU available in last 1, 2, or 3 calls
  bool should_trigger = false;
  
  if (has_sufficient_cpu_history(3)) {
    // CPU was available in all last 3 calls - definitely trigger
    should_trigger = true;
    log_info(gc, periodic)("CPU available in last 3 checks. Triggering periodic GC");
  } else if (has_sufficient_cpu_history(2)) {
    // CPU was available in last 2 calls - trigger
    should_trigger = true;
    log_info(gc, periodic)("CPU available in last 2 checks. Triggering periodic GC");
  } else {
    // Not enough CPU availability in history
    log_info(gc, periodic)("Insufficient CPU availability in history."
                           "History count: %u. Skipping",
                           _history_count);
  }
  
  return should_trigger;
}

bool G1PeriodicGCTask::is_cpu_pressure_high() {
  bool should_abort = false;

  // Check CPU pressure of process
  if (is_cpu_pressure_high_history(3)) {
    // CPU was in pressure in all last 3 calls - definitely abort
    should_abort = true;
    log_info(gc, periodic)("CPU in pressure in last 3 checks. Aborting periodic GC");
  } else if (is_cpu_pressure_high_history(2)) {
    // CPU was in pressure in last 2 calls - abort
    should_abort = true;
    log_info(gc, periodic)("CPU in pressure in last 2 checks. Aborting periodic GC");
  } else {
    // Low CPU pressure in history
    log_info(gc, periodic)("Low CPU pressure in history."
                           "History count: %u. Skipping",
                           _history_count);
  }

  return should_abort;
}

bool G1PeriodicGCTask::should_start_periodic_gc() {
  // Ensure no GC safepoints while we're doing the checks, to avoid data races.
  SuspendibleThreadSetJoiner sts;

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  // If we are currently in a concurrent mark we are going to uncommit memory soon.
  if (g1h->concurrent_mark()->cm_thread()->in_progress()) {
    log_info(gc, periodic)("Concurrent cycle in progress. Skipping.");
    return false;
  }

  if (!UsePeriodicTraceOnly) {
    // Check if enough time has passed since the last GC.
    uintx time_since_last_gc = (uintx)g1h->time_since_last_collection().milliseconds();
    if ((time_since_last_gc < G1PeriodicGCInterval)) {
      log_info(gc, periodic)("Last GC occurred " UINTX_FORMAT "ms before which is below threshold " UINTX_FORMAT "ms. Skipping.",
                              time_since_last_gc, G1PeriodicGCInterval);
      return false;
    }
  } else {
    // Check if enough time has passed since the last concurrent GC.
    uintx time_since_last_concurrent_gc = (uintx)g1h->time_since_last_concurrent_gc().milliseconds();
    if ((time_since_last_concurrent_gc < G1PeriodicGCInterval)) {
      log_info(gc, periodic)("Last concurrent GC occurred " UINTX_FORMAT "ms before which is below threshold " UINTX_FORMAT "ms. Skipping.",
                              time_since_last_concurrent_gc, G1PeriodicGCInterval);
      return false;
    }
  }

  // Check if load is lower than max.
  double recent_load;
  if ((G1PeriodicGCSystemLoadThreshold > 0.0f) &&
      (os::loadavg(&recent_load, 1) == -1 || recent_load > G1PeriodicGCSystemLoadThreshold)) {
    log_info(gc, periodic)("Load %1.2f is higher than threshold %1.2f. Skipping.",
                            recent_load, G1PeriodicGCSystemLoadThreshold);
    return false;
  }

  if (UsePeriodicTraceCPUCheck && !has_sufficient_cpu()) {
    return false;
  }

  return true;
}

bool G1PeriodicGCTask::should_abort_periodic_gc() {
  // [Skipswap] Check if periodic concurrent cycle is in progress and should be aborted
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  if (g1h->concurrent_mark()->cm_thread()->in_progress() &&
      g1h->gc_cause() == GCCause::_g1_periodic_collection) {
    // Check if CPU pressure is high
    if (is_cpu_pressure_high()) {
      return true;
    }
  }
  return false;
}

void G1PeriodicGCTask::check_for_periodic_gc() {
  // If disabled, just return.
  if (G1PeriodicGCInterval == 0) {
    return;
  }

  // [Skipswap] update available CPU cores
  if (UsePeriodicTraceCPUCheck) {
    // Update available CPUs and time spent waiting for a CPU
    double util = check_cpu_util();
    record_cpu_util(util);

    log_debug(gc, periodic)("Aborting concurrent cycle if system load is high.");
    if (should_abort_periodic_gc()) {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();
      g1h->abort_concurrent_cycle();
      return;
    }
  }

  log_debug(gc, periodic)("Checking for periodic GC.");
  if (should_start_periodic_gc()) {
    if (!G1CollectedHeap::heap()->try_collect(GCCause::_g1_periodic_collection)) {
      log_debug(gc, periodic)("GC request denied. Skipping.");
    }
  }
}

G1PeriodicGCTask::G1PeriodicGCTask(const char* name) :
  G1ServiceTask(name),
  _history_index(0),
  _history_count(0),
  _last_wall_time_sec(0.0),
  _last_process_cpu_time_ns(0),
  _cpu_tracking_initialized(false) {
  // Initialize history array
  for (uint i = 0; i < CPU_UTIL_HISTORY_SIZE; i++) {
    _cpu_util_history[i] = 1.0;
  }
}

void G1PeriodicGCTask::execute() {
  check_for_periodic_gc();
  // G1PeriodicGCInterval is a manageable flag and can be updated
  // during runtime. If no value is set, wait a second and run it
  // again to see if the value has been updated. Otherwise use the
  // real value provided.
  schedule(G1PeriodicGCInterval == 0 ? 1000 : G1PeriodicGCInterval);
}
