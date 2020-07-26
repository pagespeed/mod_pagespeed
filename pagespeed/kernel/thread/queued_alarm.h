/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */


#ifndef PAGESPEED_KERNEL_THREAD_QUEUED_ALARM_H_
#define PAGESPEED_KERNEL_THREAD_QUEUED_ALARM_H_

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/thread/scheduler.h"
#include "pagespeed/kernel/thread/sequence.h"

namespace net_instaweb {

// A helper for managing alarms that need to both run in a sequence and be
// cancellable (in the CancelAlarm sense) safely; note that
// QueuedWorkerPool::Sequence::AddFunction does not provide alarm awareness.
class QueuedAlarm : public Function {
 public:
  // Schedules a function to run at a given time in a given sequence.
  // (Note that the function's invocation may be delayed by other work
  //  present in the sequence at time of alarm going off).
  //
  // This constructor must be invoked from that sequence as well.
  //
  // The object will be destroyed automatically when either the callback
  // is invoked or the cancellation is complete. You should not free the
  // sequence until one of these points is reached.
  QueuedAlarm(Scheduler* scheduler,
              Sequence* sequence,
              int64 wakeup_time_us,
              Function* callback);

  // Cancels the alarm. This method must be run from the sequence given to the
  // constructor; and should not be called when the callback has already been
  // invoked. It is suggested that as both invocations of CancelAlarm and
  // the callback are deallocation points that you defensively clear any
  // pointers to the QueuedAlarm object when they occur.
  //
  // The function's Cancel method will be invoked; but no guarantee is made
  // as to when or in what thread context. The class does guarantee, however,
  // that it will not access the sequence_ once CancelAlarm() completes.
  void CancelAlarm();

 private:
  ~QueuedAlarm() override;

  // Runs in an arbitrary thread.
  void Run() override;

  // Runs in the sequence case.
  void SequencePortionOfRun();

  // This can get invoked if our client freed the sequence upon calling
  // CancelAlarm, in the case where what normally would be SequencePortionOfRun
  // is already on the queue.
  void SequencePortionOfRunCancelled();

  std::unique_ptr<AbstractMutex> mutex_;
  Scheduler* scheduler_;
  Sequence* sequence_;
  Function* callback_;
  Scheduler::Alarm* alarm_;

  bool canceled_;
  bool queued_sequence_portion_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_QUEUED_ALARM_H_
