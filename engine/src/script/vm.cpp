#include "script/vm.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/check.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace dreamsy::script {

namespace {

// Compile options identical to engine-web (bridge.cpp): `game` is a mutable global so
// game.timeScale isn't import-cached.
const char* const kMutableGlobals[] = {"game", nullptr};

}  // namespace

void* Vm::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  Vm* vm = static_cast<Vm*>(ud);
  size_t old = ptr ? osize : 0;
  if (nsize == 0) {
    std::free(ptr);
    vm->memory_used_ -= old;
    return nullptr;
  }
  if (nsize > old && vm->memory_used_ - old + nsize > vm->options_.memory_limit) return nullptr;
  void* block = std::realloc(ptr, nsize);
  if (block) vm->memory_used_ = vm->memory_used_ - old + nsize;
  return block;
}

void Vm::interrupt(lua_State* L, int gc) {
  if (gc >= 0) return;  // GC step, not a safepoint
  Vm* vm = static_cast<Vm*>(lua_callbacks(L)->userdata);
  if (!vm->deadline_) return;
  if (!vm->exceeded_message_ref_) {
    if (++vm->interrupt_counter_ < 256) return;  // read the clock every 256 safepoints
    vm->interrupt_counter_ = 0;
    if (vm->deadline_pending_) {
      vm->start_deadline();
      return;
    }
    if (Clock::now() < *vm->deadline_) return;
    vm->exceeded_message_ref_ = vm->active_budget_->message_ref;
  }
  lua_getref(L, vm->exceeded_message_ref_);
  lua_error(L);
}

int Vm::error_handler(lua_State* L) {
  Vm* vm = static_cast<Vm*>(lua_callbacks(L)->userdata);
  if (!lua_isstring(L, 1)) {
    luaL_tolstring(L, 1, nullptr);
    lua_replace(L, 1);
  }
  vm->traceback_ = lua_debugtrace(L);
  lua_pushvalue(L, 1);
  return 1;
}

std::unique_ptr<Vm> Vm::create(const VmOptions& options) {
  std::unique_ptr<Vm> vm(new Vm());
  vm->options_ = options;
  lua_State* L = lua_newstate(&Vm::alloc, vm.get());
  DREAMSY_CHECK(L, "cannot create a Luau state (memory limit too small?)");
  vm->L_ = L;
  lua_callbacks(L)->userdata = vm.get();
  lua_callbacks(L)->interrupt = &Vm::interrupt;
  luaL_openlibs(L);

  // os: only os.clock (engine-web keeps the real one); no loadstring.
  lua_getglobal(L, "os");
  lua_getfield(L, -1, "clock");
  lua_newtable(L);
  lua_insert(L, -2);
  lua_setfield(L, -2, "clock");
  lua_setglobal(L, "os");
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "loadstring");

  lua_pushcfunction(L, &Vm::error_handler, "error_handler");
  vm->error_handler_ref_ = lua_ref(L, -1);
  lua_pop(L, 1);

  vm->callback_budget_ = vm->make_budget(options.budget_ms);
  vm->load_budget_ = vm->make_budget(std::max(options.budget_ms, options.load_budget_ms));
  return vm;
}

Vm::Budget Vm::make_budget(double ms) {
  char msg[96];
  std::snprintf(msg, sizeof msg, "script exceeded its execution budget (%d ms)", static_cast<int>(ms));
  lua_pushstring(L_, msg);
  int ref = lua_ref(L_, -1);
  lua_pop(L_, 1);
  return {ms, ref};
}

Vm::~Vm() {
  if (L_) lua_close(L_);
}

Status Vm::load(lua_State* T, std::string_view chunkname, std::string_view source) {
  lua_CompileOptions opts{};
  opts.optimizationLevel = 1;
  opts.debugLevel = 1;
  opts.mutableGlobals = kMutableGlobals;
  size_t size = 0;
  std::unique_ptr<char, decltype(&std::free)> bytecode(luau_compile(source.data(), source.size(), &opts, &size),
                                                       &std::free);
  if (!bytecode) return Error{"out of memory while compiling"};
  std::string name(chunkname);
  if (luau_load(T, name.c_str(), bytecode.get(), size, 0) != 0) {
    std::string message = lua_tostring(T, -1) ? lua_tostring(T, -1) : "compile error";
    lua_pop(T, 1);
    return Error{message};
  }
  return Status::success();
}

std::optional<CallError> Vm::pcall(lua_State* T, int nargs, int nresults, CallKind kind) {
  int fidx = lua_gettop(T) - nargs;
  lua_getref(T, error_handler_ref_);
  lua_insert(T, fidx);

  // Top-level chunks get more time for setup. Nested requires still share the caller's deadline.
  if (deadline_pending_) start_deadline();
  const Budget* budget = kind == CallKind::Load ? &load_budget_ : &callback_budget_;
  const Budget* prev_budget = active_budget_;
  auto prev_deadline = deadline_;
  int prev_exceeded = exceeded_message_ref_;
  if (prev_deadline) {
    auto deadline = deadline_after(budget->ms);
    if (deadline < *prev_deadline) {
      deadline_ = deadline;
      active_budget_ = budget;
    }
  } else {
    active_budget_ = budget;
    deadline_ = Clock::time_point{};  // set at the first clock check (start_deadline())
    deadline_pending_ = true;
  }
  exceeded_message_ref_ = 0;
  interrupt_counter_ = 0;

  int status = lua_pcall(T, nargs, nresults, fidx);

  active_budget_ = prev_budget;
  deadline_ = prev_deadline;
  deadline_pending_ = false;  // a nested call started the outer deadline above
  if (prev_exceeded || !prev_deadline) exceeded_message_ref_ = prev_exceeded;
  lua_remove(T, fidx);  // the handler

  if (status == LUA_OK) return std::nullopt;
  CallError err;
  const char* m = lua_tostring(T, -1);
  err.message = m ? m : "error";
  err.traceback = traceback_;
  err.out_of_memory = status == LUA_ERRMEM;
  if (err.out_of_memory && !prev_deadline) lua_gc(L_, LUA_GCCOLLECT, 0);
  return err;
}

Vm::Clock::time_point Vm::deadline_after(double ms) const {
  return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

void Vm::start_deadline() {
  deadline_ = deadline_after(active_budget_->ms);
  deadline_pending_ = false;
}

lua_State* Vm::new_sandbox_thread() {
  lua_State* T = lua_newthread(L_);
  luaL_sandboxthread(T);
  return T;
}

void Vm::sandbox() { luaL_sandbox(L_); }

}  // namespace dreamsy::script
