// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/alarm.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "util/chrono_helpers.h"

namespace openscreen {
namespace {

class AlarmTest : public testing::Test {
 public:
  FakeClock* clock() { return &clock_; }
  FakeTaskRunner* task_runner() { return &task_runner_; }
  Alarm* alarm() { return &alarm_; }

 private:
  FakeClock clock_{Clock::now()};
  FakeTaskRunner task_runner_{clock_};
  Alarm alarm_{&FakeClock::now, task_runner_};
};

TEST_F(AlarmTest, RunsTaskAsClockAdvances) {
  constexpr Clock::duration kDelay = milliseconds(20);

  const Clock::time_point alarm_time = FakeClock::now() + kDelay;
  Clock::time_point actual_run_time{};
  alarm()->Schedule([&]() { actual_run_time = FakeClock::now(); }, alarm_time);
  // Confirm the lambda did not run immediately.
  ASSERT_EQ(Clock::time_point{}, actual_run_time);

  // Confirm the lambda does not run until the necessary delay has elapsed.
  clock()->Advance(kDelay / 2);
  ASSERT_EQ(Clock::time_point{}, actual_run_time);

  // Confirm the lambda is called when the necessary delay has elapsed.
  clock()->Advance(kDelay / 2);
  ASSERT_EQ(alarm_time, actual_run_time);

  // Confirm the lambda is only run once.
  clock()->Advance(kDelay * 100);
  ASSERT_EQ(alarm_time, actual_run_time);
}

TEST_F(AlarmTest, RunsTaskImmediately) {
  const Clock::time_point expected_run_time = FakeClock::now();
  Clock::time_point actual_run_time{};
  alarm()->Schedule([&]() { actual_run_time = FakeClock::now(); },
                    Alarm::kImmediately);
  // Confirm the lambda did not run yet, since it should run asynchronously, in
  // a separate TaskRunner task.
  ASSERT_EQ(Clock::time_point{}, actual_run_time);

  // Confirm the lambda runs without the clock having to tick forward.
  task_runner()->RunTasksUntilIdle();
  ASSERT_EQ(expected_run_time, actual_run_time);

  // Confirm the lambda is only run once.
  clock()->Advance(seconds(2));
  ASSERT_EQ(expected_run_time, actual_run_time);
}

TEST_F(AlarmTest, CancelsTaskWhenGoingOutOfScope) {
  constexpr Clock::duration kDelay = milliseconds(20);
  constexpr Clock::time_point kNever{};

  Clock::time_point actual_run_time{};
  {
    Alarm scoped_alarm(&FakeClock::now, *task_runner());
    const Clock::time_point alarm_time = FakeClock::now() + kDelay;
    scoped_alarm.Schedule([&]() { actual_run_time = FakeClock::now(); },
                          alarm_time);
    // `scoped_alarm` is destroyed.
  }

  // Confirm the lambda has never and will never run.
  ASSERT_EQ(kNever, actual_run_time);
  clock()->Advance(kDelay * 100);
  ASSERT_EQ(kNever, actual_run_time);
}

TEST_F(AlarmTest, Cancels) {
  constexpr Clock::duration kDelay = milliseconds(20);

  const Clock::time_point alarm_time = FakeClock::now() + kDelay;
  Clock::time_point actual_run_time{};
  alarm()->Schedule([&]() { actual_run_time = FakeClock::now(); }, alarm_time);

  // Advance the clock for half the delay, and confirm the lambda has not run
  // yet.
  clock()->Advance(kDelay / 2);
  ASSERT_EQ(Clock::time_point{}, actual_run_time);

  // Cancel and then advance the clock well past the delay, and confirm the
  // lambda has never run.
  alarm()->Cancel();
  clock()->Advance(kDelay * 100);
  ASSERT_EQ(Clock::time_point{}, actual_run_time);
}

TEST_F(AlarmTest, CancelsAndRearms) {
  constexpr Clock::duration kShorterDelay = milliseconds(10);
  constexpr Clock::duration kLongerDelay = milliseconds(100);

  // Run the test twice: Once when scheduling first with a long delay, then a
  // shorter delay; and once when scheduling first with a short delay, then a
  // longer delay. This is to test Alarm's internal scheduling/firing logic.
  for (int do_longer_then_shorter = 0; do_longer_then_shorter <= 1;
       ++do_longer_then_shorter) {
    const auto delay1 = do_longer_then_shorter ? kLongerDelay : kShorterDelay;
    const auto delay2 = do_longer_then_shorter ? kShorterDelay : kLongerDelay;

    int count1 = 0;
    alarm()->Schedule([&]() { ++count1; }, FakeClock::now() + delay1);

    // Advance the clock for half of `delay1`, and confirm the lambda that
    // increments the variable does not run.
    ASSERT_EQ(0, count1);
    clock()->Advance(delay1 / 2);
    ASSERT_EQ(0, count1);

    // Schedule a different lambda, that increments a different variable, to run
    // after `delay2`.
    int count2 = 0;
    alarm()->Schedule([&]() { ++count2; }, FakeClock::now() + delay2);

    // Confirm the second scheduling will fire at the right moment.
    clock()->Advance(delay2 / 2);
    ASSERT_EQ(0, count2);
    clock()->Advance(delay2 / 2);
    ASSERT_EQ(1, count2);

    // Confirm the second scheduling never fires a second time, and also that
    // the first one doesn't fire.
    clock()->Advance(std::max(delay1, delay2) * 100);
    ASSERT_EQ(0, count1);
    ASSERT_EQ(1, count2);
  }
}

// Tests that repeatedly canceling and re-scheduling an Alarm, to a time no
// earlier than the originally-scheduled one, reuses the already-queued firing
// instead of posting more tasks to the TaskRunner.
TEST_F(AlarmTest, CancelAndRescheduleReusesQueuedFiring) {
  constexpr Clock::duration kDelay = milliseconds(20);
  const Clock::time_point alarm_time = FakeClock::now() + kDelay;

  int count = 0;
  alarm()->Schedule([&] { ++count; }, alarm_time);
  ASSERT_EQ(1, task_runner()->delayed_task_count());

  for (int i = 0; i < 10; ++i) {
    alarm()->Cancel();
    alarm()->Schedule([&] { ++count; }, alarm_time);
  }
  EXPECT_EQ(1, task_runner()->delayed_task_count());

  clock()->Advance(kDelay);
  EXPECT_EQ(1, count);
  EXPECT_EQ(0, task_runner()->delayed_task_count());
  EXPECT_EQ(0, task_runner()->ready_task_count());
}

// Tests that the client's Task can re-schedule the Alarm from within itself.
TEST_F(AlarmTest, CanRescheduleFromWithinTask) {
  constexpr Clock::duration kDelay = milliseconds(20);
  constexpr int kNumRuns = 3;

  std::vector<Clock::time_point> run_times;
  std::function<void()> task = [&] {
    run_times.push_back(FakeClock::now());
    if (run_times.size() < kNumRuns) {
      alarm()->ScheduleFromNow(task, kDelay);
    }
  };
  const Clock::time_point start_time = FakeClock::now();
  alarm()->ScheduleFromNow(task, kDelay);

  clock()->Advance(kDelay * kNumRuns * 10);
  ASSERT_EQ(static_cast<size_t>(kNumRuns), run_times.size());
  for (int i = 0; i < kNumRuns; ++i) {
    EXPECT_EQ(start_time + kDelay * (i + 1), run_times[i]);
  }
}

// Tests that the Alarm can be destroyed by the client's Task.
TEST_F(AlarmTest, CanBeDestroyedFromWithinTask) {
  auto scoped_alarm = std::make_unique<Alarm>(&FakeClock::now, *task_runner());
  bool did_run = false;
  scoped_alarm->Schedule(
      [&] {
        did_run = true;
        scoped_alarm.reset();
      },
      Alarm::kImmediately);
  task_runner()->RunTasksUntilIdle();
  EXPECT_TRUE(did_run);
  EXPECT_FALSE(scoped_alarm);
}

}  // namespace
}  // namespace openscreen
