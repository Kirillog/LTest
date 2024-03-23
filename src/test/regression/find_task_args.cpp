#include <cassert>
#include <iostream>
#include <optional>

#include "../../runtime/include/lib.h"

Task find_task(TaskBuilderList l) {
  std::optional<Task> task;
  for (auto task_builder : *l) {
    auto cur_task = Task{nullptr, task_builder};
    if (cur_task.GetName() == "test") {
      auto args = cur_task.GetArgs();
      assert(args.size() == 3);
      assert(args[0] == 42);
      assert(args[1] == 43);
      assert(args[2] == 44);
      task = cur_task;
      break;
    }
  }
  assert(task.has_value() && "task `test` is not found");
  return task.value();
}