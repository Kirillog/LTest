#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include "../specs/queue.h"
#include "runtime/include/verifying.h"
#include "verifiers/buffered_channel_verifier.h"

constexpr int N = 5;

namespace spec {
struct BufferedChannel {
  int Send(int v) {
    deq.push_back(v);
    return 0;
  }

  int TryRecv() {
    if (deq.empty()) {
      return -1;
    }
    auto value = deq.front();
    deq.pop_front();
    return value;
  }

  using method_t = std::function<int(BufferedChannel *l, void *args)>;
  static auto GetMethods() {
    method_t send_func = [](BufferedChannel *l, void *args) -> int {
      auto real_args = reinterpret_cast<std::tuple<int> *>(args);
      return l->Send(std::get<0>(*real_args));
    };

    method_t try_recv_func = [](BufferedChannel *l, void *args) -> int {
      return l->TryRecv();
    };

    return std::map<std::string, method_t>{
        {"Send", send_func},
        {"TryRecv", try_recv_func},
    };
  }

  std::deque<int> deq;
};

struct BufferedChannelHash {
  size_t operator()(const BufferedChannel &r) const {
    int res = 0;
    for (int elem : r.deq) {
      res += elem;
    }
    return res;
  }
};

struct BufferedChannelEquals {
  bool operator()(const BufferedChannel &lhs,
                  const BufferedChannel &rhs) const {
    return lhs.deq == rhs.deq;
  }
};
};  // namespace spec

struct BufferedChannel {
  non_atomic int Send(int v) {
    std::unique_lock lock{mutex_};
    while (!closed_ && full_) {
      debug(stderr, "Waiting...\n");
      send_side_cv_.wait(lock);
    }
    debug(stderr, "Send\n");

    queue_[sidx_] = v;
    sidx_ = (sidx_ + 1) % N;
    full_ = (sidx_ == ridx_);
    empty_ = false;
    return 0;
  }

  // TryRecv is not blocking otherwise it is dual structure
  non_atomic int TryRecv() {
    std::lock_guard lock{mutex_};
    if (closed_ || empty_) {
      return -1;
    }
    auto val = queue_[ridx_];
    ridx_ = (ridx_ + 1) % 5;
    empty_ = (sidx_ == ridx_);
    full_ = false;
    send_side_cv_.notify_one();
    return val;
  }

  non_atomic int Close() {
    closed_.store(true);
    send_side_cv_.notify_all();
    return 0;
  }

  std::mutex mutex_;
  std::condition_variable send_side_cv_;
  std::atomic_bool closed_{false};

  bool full_{false};
  bool empty_{true};

  uint32_t sidx_{0}, ridx_{0};

  std::array<int, N> queue_{};
};

auto generateInt(size_t) {
  return ltest::generators::makeSingleArg(rand() % 10 + 1);
}

using spec_t =
    ltest::Spec<BufferedChannel, spec::BufferedChannel,
                spec::BufferedChannelHash, spec::BufferedChannelEquals>;

LTEST_ENTRYPOINT_CONSTRAINT(spec_t, spec::BufferedChannelVerifier);

target_method(generateInt, void, BufferedChannel, Send, int);
target_method(ltest::generators::genEmpty, int, BufferedChannel, TryRecv);
// target_method(ltest::generators::genEmpty, int, BufferedChannel, Close);