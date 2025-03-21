#pragma once
#include <limits>
#include <optional>
#include <utility>

#include "lib.h"
#include "lincheck.h"
#include "logger.h"
#include "pretty_print.h"
#include "stable_vector.h"

/// Generated by some strategy task, 
/// that may be not executed due to constraints of data structure 
struct CreatedTaskMetaData {
  std::string name;
  bool is_new;
  size_t thread_id;
};

struct TaskWithMetaData {
  Task& task;
  bool is_new;
  size_t thread_id;
};

/// StrategyVerifier is required for scheduling only allowed tasks
/// Some data structures doesn't allow us to schedule one tasks before another
/// e.g. Mutex -- we are not allowed to schedule unlock before lock call, it is
/// UB.
template <typename T>
concept StrategyVerifier = requires(T a) {
  {
    a.Verify(CreatedTaskMetaData(string(), bool(), int()))
  } -> std::same_as<bool>;
  {
    a.OnFinished(TaskWithMetaData(std::declval<Task&>(), bool(), int()))
  } -> std::same_as<void>;
  { a.Reset() } -> std::same_as<void>;
};

template <typename T>
concept ManualReleased = requires (T a) {
  { a.Release() } -> std::same_as<void>;
};


// Strategy is the general strategy interface which decides which task
// will be the next one it can be implemented by different strategies, such as:
// randomized/tla/fair
template <StrategyVerifier Verifier>
struct Strategy {
  virtual TaskWithMetaData Next() = 0;

  // Strategy should stop all tasks that already have been started
  virtual void StartNextRound() = 0;

  virtual ~Strategy() = default;
  Verifier sched_checker{};
};

struct Scheduler {
  using FullHistory = std::vector<std::reference_wrapper<Task>>;
  using SeqHistory = std::vector<std::variant<Invoke, Response>>;
  using Result = std::optional<std::pair<FullHistory, SeqHistory>>;

  virtual Result Run() = 0;

  virtual ~Scheduler() = default;
};

// StrategyScheduler generates different sequential histories(using Strategy)
// and then checks them with the ModelChecker
template <StrategyVerifier Verifier>
struct StrategyScheduler : public Scheduler {
  // max_switches represents the maximal count of switches. After this count
  // scheduler will end execution of the Run function
  StrategyScheduler(Strategy<Verifier>& sched_class, ModelChecker& checker,
                    PrettyPrinter& pretty_printer, size_t max_tasks,
                    size_t max_rounds)
      : strategy(sched_class),
        checker(checker),
        pretty_printer(pretty_printer),
        max_tasks(max_tasks),
        max_rounds(max_rounds) {}

  // Run returns full unliniarizable history if such a history is found. Full
  // history is a history with all events, where each element in the vector is a
  // Resume operation on the corresponding task
  Scheduler::Result Run() override {
    for (size_t i = 0; i < max_rounds; ++i) {
      log() << "run round: " << i << "\n";
      debug(stderr, "run round: %d\n", i);
      auto seq_history = runRound();
      if (seq_history.has_value()) {
        return seq_history;
      }
      log() << "===============================================\n\n";
      log().flush();
      strategy.StartNextRound();
    }

    return std::nullopt;
  }

 private:
  Scheduler::Result runRound() {
    // History of invoke and response events which is required for the checker
    std::vector<std::variant<Invoke, Response>> sequential_history;
    // Full history of the current execution in the Run function
    std::vector<std::reference_wrapper<Task>> full_history;

    for (size_t finished_tasks = 0; finished_tasks < max_tasks;) {
      auto t = strategy.Next();
      debug(stderr, "Tasks finished: %d\n", finished_tasks);
      auto [next_task, is_new, thread_id] = t;

      // fill the sequential history
      if (is_new) {
        sequential_history.emplace_back(Invoke(next_task, thread_id));
      }
      full_history.emplace_back(next_task);

      next_task->Resume();
      if (next_task->IsReturned()) {
        finished_tasks++;
        strategy.sched_checker.OnFinished(t);

        auto result = next_task->GetRetVal();
        sequential_history.emplace_back(Response(next_task, result, thread_id));
      }
    }

    pretty_printer.PrettyPrint(sequential_history, log());

    if (!checker.Check(sequential_history)) {
      return std::make_pair(full_history, sequential_history);
    }

    return std::nullopt;
  }

  Strategy<Verifier>& strategy;
  ModelChecker& checker;
  PrettyPrinter& pretty_printer;
  size_t max_tasks;
  size_t max_rounds;
};

// TLAScheduler generates all executions satisfying some conditions.
template <typename TargetObj>
struct TLAScheduler : Scheduler {
  TLAScheduler(size_t max_tasks, size_t max_rounds, size_t threads_count,
               size_t max_switches, std::vector<TaskBuilder> constructors,
               ModelChecker& checker, PrettyPrinter& pretty_printer)
      : max_tasks{max_tasks},
        max_rounds{max_rounds},
        max_switches{max_switches},
        constructors{std::move(constructors)},
        checker{checker},
        pretty_printer{pretty_printer} {
    for (size_t i = 0; i < threads_count; ++i) {
      threads.emplace_back(Thread{
          .id = i,
          .tasks = StableVector<Task>{},
      });
    }
  };

  Scheduler::Result Run() override {
    auto [_, res] = RunStep(0, 0);
    return res;
  }

  ~TLAScheduler() { TerminateTasks(); }

 private:
  struct Thread {
    size_t id;
    StableVector<Task> tasks;
  };

  // TLAScheduler enumerates all possible executions with finished max_tasks.
  // In fact, it enumerates tables (c = continue, f = finished):
  //         *---------*---------*--------*
  //         |   T1    |   T2    |   T3   |
  //         *---------*---------*--------*
  // state0  | task_i  |         |        |
  // state1  |    c    |         |        |
  // state2  |         | task_j  |        |
  // ...     |         |    c    |        |
  //         |    f    |         |        |
  //                      .....
  // Frame struct describes one row of this table.
  struct Frame {
    // Pointer to the in task thread.
    Task* task{};
    // Is true if the task was created at this step.
    bool is_new{};
  };

  // Terminates all running tasks.
  // We do it in a dangerous way: in random order.
  // Actually, we assume obstruction free here.
  // TODO: for non obstruction-free we need to take into account dependencies.
  void TerminateTasks() {
    for (size_t i = 0; i < threads.size(); ++i) {
      if (!threads[i].tasks.empty()) {
        threads[i].tasks.back()->Terminate();
      }
    }
  }

  // Replays all actions from 0 to the step_end.
  void Replay(size_t step_end) {
    // Firstly, terminate all running tasks.
    TerminateTasks();
    // In histories we store references, so there's no need to update it.
    state.Reset();
    for (size_t step = 0; step < step_end; ++step) {
      auto& frame = frames[step];
      auto task = frame.task;
      assert(task);
      if (frame.is_new) {
        // It was a new task.
        // So restart it from the beginning with the same args.
        *task = (*task)->Restart(&state);
      } else {
        // It was a not new task, hence, we recreated in early.
      }
      (*task)->Resume();
    }
  }

  // Resumes choosed task.
  // If task is finished and finished tasks == max_tasks, stops.
  std::tuple<bool, typename Scheduler::Result> ResumeTask(
      Frame& frame, size_t step, size_t switches, Thread& thread, bool is_new) {
    auto thread_id = thread.id;
    size_t previous_thread_id = thread_id_history.empty()
                                    ? std::numeric_limits<size_t>::max()
                                    : thread_id_history.back();
    size_t nxt_switches = switches;
    if (!is_new) {
      if (thread_id != previous_thread_id) {
        ++nxt_switches;
      }
      if (nxt_switches > max_switches) {
        // The limit of switches is achieved.
        // So, do not resume task.
        return {false, {}};
      }
    }
    auto& task = thread.tasks.back();
    frame.task = &task;

    full_history.push_back({thread_id, task});
    thread_id_history.push_back(thread_id);
    if (is_new) {
      sequential_history.emplace_back(Invoke(task, thread_id));
    }

    assert(!task->IsParked());
    task->Resume();
    bool is_finished = task->IsReturned();
    if (is_finished) {
      finished_tasks++;
      auto result = task->GetRetVal();
      sequential_history.emplace_back(Response(task, result, thread_id));
    }

    bool stop = finished_tasks == max_tasks;
    if (!stop) {
      // Run recursive step.
      auto [is_over, res] = RunStep(step + 1, nxt_switches);
      if (is_over || res.has_value()) {
        return {is_over, res};
      }
    } else {
      log() << "run round: " << finished_rounds << "\n";
      pretty_printer.PrettyPrint(full_history, log());
      log() << "===============================================\n\n";
      log().flush();
      // Stop, check if the the generated history is linearizable.
      ++finished_rounds;
      if (!checker.Check(sequential_history)) {
        return {false,
                std::make_pair(Scheduler::FullHistory{}, sequential_history)};
      }
      if (finished_rounds == max_rounds) {
        // It was the last round.
        return {true, {}};
      }
    }

    thread_id_history.pop_back();
    full_history.pop_back();
    if (is_finished) {
      --finished_tasks;
      // resp.
      sequential_history.pop_back();
    }
    if (is_new) {
      // inv.
      sequential_history.pop_back();
    }

    return {false, {}};
  }

  std::tuple<bool, typename Scheduler::Result> RunStep(size_t step,
                                                       size_t switches) {
    // Push frame to the stack.
    frames.emplace_back(Frame{});
    auto& frame = frames.back();

    bool all_parked = true;
    // Pick next task.
    for (size_t i = 0; i < threads.size(); ++i) {
      auto& thread = threads[i];
      auto& tasks = thread.tasks;
      if (!tasks.empty() && !tasks.back()->IsReturned()) {
        if (tasks.back()->IsParked()) {
          continue;
        }
        all_parked = false;
        // Task exists.
        frame.is_new = false;
        auto [is_over, res] = ResumeTask(frame, step, switches, thread, false);
        if (is_over || res.has_value()) {
          return {is_over, res};
        }
        // As we can't return to the past in coroutine, we need to replay all
        // tasks from the beginning.
        Replay(step);
        continue;
      }

      all_parked = false;
      // Choose constructor to create task.
      for (size_t cons_num = 0; auto cons : constructors) {
        frame.is_new = true;
        auto size_before = tasks.size();
        tasks.emplace_back(cons.Build(&state, i));

        auto [is_over, res] = ResumeTask(frame, step, switches, thread, true);
        if (is_over || res.has_value()) {
          return {is_over, res};
        }

        tasks.back()->Terminate();
        tasks.pop_back();
        auto size_after = thread.tasks.size();
        assert(size_before == size_after);
        // As we can't return to the past in coroutine, we need to replay all
        // tasks from the beginning.
        Replay(step);
        ++cons_num;
      }
    }

    assert(!all_parked && "deadlock");
    frames.pop_back();
    return {false, {}};
  }

  PrettyPrinter& pretty_printer;
  size_t max_tasks;
  size_t max_rounds;
  size_t max_switches;
  std::vector<TaskBuilder> constructors;
  ModelChecker& checker;

  // Running state.
  size_t finished_tasks{};
  size_t finished_rounds{};
  TargetObj state{};
  std::vector<std::variant<Invoke, Response>> sequential_history;
  std::vector<std::pair<int, std::reference_wrapper<Task>>> full_history;
  std::vector<size_t> thread_id_history;
  StableVector<Thread> threads;
  StableVector<Frame> frames;
};
