// A sandboxed Luau VM with a memory cap and a CPU budget per call.
//
// Same semantics as engine-web/luau/src/bridge.cpp, natively:
//   - memory: the allocator refuses growth past `memory_limit` (Luau raises "not enough memory")
//   - CPU: every protected call gets a deadline; nested calls never extend the outer one. Once a
//     deadline passes, every interrupt re-raises the budget error, so a script's own pcall can't
//     swallow it; it keeps firing until the guarded call unwinds.
//   - errors: a message plus a traceback (lua_debugtrace) captured by the error handler
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/result.h"

struct lua_State;

namespace dreamsy::script {

struct VmOptions {
  size_t memory_limit = 64u * 1024 * 1024;
  double budget_ms = 250;
};

struct CallError {
  std::string message;
  std::string traceback;
  bool out_of_memory = false;
};

class Vm {
 public:
  static std::unique_ptr<Vm> create(const VmOptions& options);
  ~Vm();
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  lua_State* main() const { return L_; }

  // Compiles `source` and pushes the chunk onto T's stack. `chunkname` follows Lua rules
  // ("@scripts/x.luau" makes errors read "scripts/x.luau:12: ..."). On error nothing is pushed.
  Status load(lua_State* T, std::string_view chunkname, std::string_view source);

  // Protected call with the error handler and a CPU deadline. On error the stack is left as
  // lua_pcall leaves it (message on top); the details are returned.
  std::optional<CallError> pcall(lua_State* T, int nargs, int nresults);

  // Pushes a new thread onto the main stack and sandboxes it (own writable globals over the
  // frozen _G). Returns the thread.
  lua_State* new_sandbox_thread();

  // Freezes the globals and libraries (luaL_sandbox). Call once, after the prelude has run.
  void sandbox();

  size_t memory_used() const { return memory_used_; }

 private:
  Vm() = default;
  static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
  static void interrupt(lua_State* L, int gc);
  static int error_handler(lua_State* L);

  lua_State* L_ = nullptr;
  VmOptions options_;
  size_t memory_used_ = 0;

  using Clock = std::chrono::steady_clock;
  std::optional<Clock::time_point> deadline_;  // nullopt = no protected call running
  // Outermost calls start their deadline at the first clock check, so short callbacks never read the clock.
  bool deadline_pending_ = false;
  void start_deadline();  // deadline_ = now + budget
  bool exceeded_ = false;
  int interrupt_counter_ = 0;
  int budget_message_ref_ = -1;
  int error_handler_ref_ = -1;
  std::string traceback_;
};

}  // namespace dreamsy::script
