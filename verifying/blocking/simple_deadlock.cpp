#include <map>
#include <mutex>

#include "runtime/include/generators.h"
#include "runtime/include/verifying.h"
#include "runtime/include/verifying_macro.h"

namespace spec {

struct MutexDeadlock {
  int LockFirstSecond() {
    x1 -= 1;
    x2 += 1;
    return 0;
  }

  int LockSecondFirst() {
    x2 -= 1;
    x1 += 1;
    return 0;
  }

  int x1{0}, x2{0};

  using method_t = std::function<int(MutexDeadlock *l, void *args)>;
  static auto GetMethods() {
    method_t fs_func = [](MutexDeadlock *l, void *args) -> int {
      return l->LockFirstSecond();
    };

    method_t sf_func = [](MutexDeadlock *l, void *args) -> int {
      return l->LockSecondFirst();
    };

    return std::map<std::string, method_t>{
        {"LockFirstSecond", fs_func},
        {"LockSecondFirst", sf_func},
    };
  }
};

struct MutexDeadlockHash {
  size_t operator()(const MutexDeadlock &r) const { return r.x1 ^ r.x2; }
};

struct MutexDeadlockEquals {
  bool operator()(const MutexDeadlock &lhs, const MutexDeadlock &rhs) const {
    return lhs.x1 == rhs.x1 && lhs.x2 == rhs.x2;
  }
};
}  // namespace spec

struct MutexDeadlock {
  non_atomic int LockFirstSecond() {
    std::lock_guard g1{m1_};
    x1 -= 1;
    {
      std::lock_guard g2{m2_};
      x2 += 1;
      return 0;
    }
  }

  non_atomic int LockSecondFirst() {
    std::lock_guard g2{m2_};
    x2 -= 1;
    {
      std::lock_guard g1{m1_};
      x1 += 1;
      return 0;
    }
  }

  int x1{0}, x2{0};

  std::mutex m1_;
  std::mutex m2_;
};

using spec_t = ltest::Spec<MutexDeadlock, spec::MutexDeadlock,
                           spec::MutexDeadlockHash, spec::MutexDeadlockEquals>;

LTEST_ENTRYPOINT(spec_t);

target_method(ltest::generators::genEmpty, int, MutexDeadlock, LockFirstSecond);
target_method(ltest::generators::genEmpty, int, MutexDeadlock, LockSecondFirst);