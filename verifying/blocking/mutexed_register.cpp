#include <mutex>

#include "runtime/include/lib.h"
#include "runtime/include/verifying.h"
#include "verifying/specs/register.h"

struct Register {
  non_atomic void add() {
    while (!m_.try_lock()) {
      CoroYield();
    }
    ++x_;
    m_.unlock();
  }
  non_atomic int get() {
    std::lock_guard lock{m_};
    return x_;
  }

  int x_{};
  std::mutex m_;
};

using spec_t =
    ltest::Spec<Register, spec::LinearRegister, spec::LinearRegisterHash,
                spec::LinearRegisterEquals>;

LTEST_ENTRYPOINT(spec_t);

target_method(ltest::generators::genEmpty, void, Register, add);

target_method(ltest::generators::genEmpty, int, Register, get);
