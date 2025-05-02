#include <optional>

#include "runtime/include/lib.h"

namespace spec {
struct BufferedChannelVerifier {
  bool Verify(const std::string& task_name, size_t thread_id) {
    if (task_name == "Send") {
      if (senders_ == 0) {
        ++senders_;
        ++size_;
        return true;
      }
      return false;
    } else if (task_name == "TryRecv") {
      if (size_ > 0) {
        --size_;
      }
      return true;
    } else {
      assert(false);
    }
  }

  void OnFinished(Task& task, size_t thread_id) {
    auto task_name = task->GetName();
    if (task_name == "Send") {
      --senders_;
      return;
    } else if (task_name == "TryRecv") {
      return;
    } else {
      assert(false);
    }
  }

  std::optional<std::string> ReleaseTask(size_t thread_id) {
    if (size_ > 0) {
      return {"TryRecv"};
    }
    return std::nullopt;
  }

  size_t senders_;
  size_t size_;
};
}  // namespace spec