// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/alarm.h"

#include <algorithm>

#include "util/osp_logging.h"

namespace openscreen {

Alarm::Alarm(ClockNowFunctionPtr now_function, TaskRunner& task_runner)
    : now_function_(now_function), task_runner_(task_runner) {
  OSP_CHECK(now_function_);
}

Alarm::~Alarm() = default;

void Alarm::Cancel() {
  scheduled_task_ = TaskRunner::Task();
  has_queued_fire_ = false;
  ++current_fire_id_;
}

void Alarm::ScheduleWithTask(TaskRunner::Task task,
                             Clock::time_point desired_alarm_time) {
  OSP_CHECK(task.valid());

  scheduled_task_ = std::move(task);

  const Clock::time_point now = now_function_();
  alarm_time_ = std::max(now, desired_alarm_time);

  // Ensure that a later firing will occur, and not too late.
  if (has_queued_fire_) {
    if (next_fire_time_ <= alarm_time_) {
      return;
    }
  }
  InvokeLater(now, alarm_time_);
}

void Alarm::InvokeLater(Clock::time_point now, Clock::time_point fire_time) {
  has_queued_fire_ = true;
  next_fire_time_ = fire_time;
  const uint64_t fire_id = ++current_fire_id_;
  task_runner_->PostTaskWithDelay(
      [weak_this = weak_factory_.GetWeakPtr(), fire_id]() {
        if (auto* self = weak_this.get()) {
          self->OnFire(fire_id);
        }
      },
      fire_time - now);
}

void Alarm::OnFire(uint64_t fire_id) {
  if (fire_id != current_fire_id_) {
    return;  // Superceded by a newer scheduling or canceled.
  }
  has_queued_fire_ = false;
  TryInvoke();
}

void Alarm::TryInvoke() {
  if (!scheduled_task_.valid()) {
    return;  // This Alarm was canceled in the meantime.
  }

  // If this is an early firing, re-schedule for later. This happens if
  // Schedule() was called again before this firing had occurred.
  const Clock::time_point now = now_function_();
  if (now < alarm_time_) {
    InvokeLater(now, alarm_time_);
    return;
  }

  // Move the client Task to the stack before executing, just in case the task
  // itself: a) calls any Alarm methods re-entrantly, or b) causes the
  // destruction of this Alarm instance.
  // WARNING: `this` is not valid after here!
  TaskRunner::Task task = std::move(scheduled_task_);
  task();
}

// static
constexpr Clock::time_point Alarm::kImmediately;

}  // namespace openscreen
