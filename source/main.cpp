#include <iostream>
#include <csignal>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <string_view>

typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint8_t u8;
typedef int8_t s8;

#include "lunar_topology.h"

using namespace std;

#include <include/scx/common.h>
#include <include/scx/enums.h>
#include <include/scx/user_exit_info_common.h>
#include <include/scx/enums.autogen.h>
#include <source/lunar.skel.h>

std::atomic<bool> stop{};

static void sig_handler(int)
{
  stop = true;
}

enum class RunResult
{
  Stopped,  // SIGINT/SIGTERM
  Restart,  // kernel asked for a restart (e.g. CPU hotplug)
  Failed,
};

static RunResult run_once()
{
  lunar_bpf* skel = lunar_bpf__open();
  if (!skel)
  {
    std::cerr << "Failed to create BPF skeleton." << std::endl;
    return RunResult::Failed;
  }
  SCX_ENUM_INIT(skel);

  skel->struct_ops.lunar_ops->hotplug_seq = scx_hotplug_seq();

  UEI_SET_SIZE(skel, lunar_ops, uei);

  if (!setup_lunar_topology(skel))
  {
    std::cerr << "Failed to load llc information." << std::endl;
    lunar_bpf__destroy(skel);
    return RunResult::Failed;
  }

  int err = lunar_bpf__load(skel);
  if (err)
  {
    std::cerr << "Failed to load scheduler: " << err << std::endl;
    lunar_bpf__destroy(skel);
    return RunResult::Failed;
  }

  std::cerr << "Successfully opened and loaded the lunar scheduler." << std::endl;

  std::cout << "Attaching sched_ext scheduler..." << std::endl;

  err = lunar_bpf__attach(skel);
  if (err)
  {
    std::cerr << "Failed to attach BPF programs: " << err << std::endl;
    lunar_bpf__destroy(skel);
    return RunResult::Failed;
  }

  std::cout << "lunar scheduler is successfully running!" << std::endl;

  bool ejected = false;
  while (!stop)
  {
    if (UEI_EXITED(skel, uei))
    {
      ejected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RunResult result = RunResult::Stopped;
  if (ejected)
  {
    u64 ecode = UEI_REPORT(skel, uei);
    result = UEI_ECODE_RESTART(ecode) ? RunResult::Restart : RunResult::Failed;
  }

  lunar_bpf__destroy(skel);
  return result;
}

int main(int argc, const char** argv)
{
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  RunResult result;
  do
  {
    result = run_once();
    if (result == RunResult::Restart && !stop)
      std::cout << "Kernel requested a restart (e.g. CPU hotplug), restarting..." << std::endl;
  } while (result == RunResult::Restart && !stop);

  std::cout << "Shutting down and restoring default kernel scheduler..." << std::endl;

  return result == RunResult::Failed ? 1 : 0;
}
