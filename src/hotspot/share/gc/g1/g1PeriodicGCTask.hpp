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

#ifndef SHARE_GC_G1_G1PERIODICGCTASK_HPP
#define SHARE_GC_G1_G1PERIODICGCTASK_HPP

#include "gc/g1/g1ServiceThread.hpp"

// Task handling periodic GCs
class G1PeriodicGCTask : public G1ServiceTask {
  static const uint CPU_UTIL_HISTORY_SIZE = 3;
  
  // History of CPU availability checks (true = enough free cores, false = not enough)
  double _cpu_util_history[CPU_UTIL_HISTORY_SIZE];
  uint _history_index;  // Current position in circular buffer
  uint _history_count;  // Number of valid entries in history (0 to CPU_UTIL_HISTORY_SIZE)
  
  // [Skipswap] CPU utilization tracking
  double _last_wall_time_sec;  // Wall time of last check (in seconds)
  jlong _last_process_cpu_time_ns;  // Process CPU time of last check (in nanoseconds)
  bool _cpu_tracking_initialized;  // Whether we have valid initial values
  
  bool should_abort_periodic_gc();
  bool should_start_periodic_gc();
  void check_for_periodic_gc();

  // [Skipswap] Sysmtem and process status tracking methods
  jlong get_process_cpu_time_ns();  // Sum CPU time of all threads
  double check_cpu_util();
  void record_cpu_util(double util);
  bool has_sufficient_cpu_history(uint required_count) const;
  bool has_sufficient_cpu();
  bool is_cpu_pressure_high_history(uint required_count) const;
  bool is_cpu_pressure_high();
  
public:
  G1PeriodicGCTask(const char* name);
  virtual void execute();
};

#endif // SHARE_GC_G1_G1PERIODICGCTASK_HPP