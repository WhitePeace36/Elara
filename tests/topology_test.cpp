// SPDX-License-Identifier: GPL-2.0
//
// Tests of elara::read_topology() (source/elara_topology.h) on made-up cpus: each
// test writes the sysfs files of a machine into a temporary directory and checks
// what the loader makes of them.

#include "elara_topology.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                              \
  do                                                                             \
  {                                                                              \
    if (!(cond))                                                                 \
    {                                                                            \
      std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
      ++failures;                                                                \
    }                                                                            \
  } while (0)

static void put(const fs::path& file, const std::string& content)
{
  fs::create_directories(file.parent_path());
  std::ofstream(file) << content << "\n";
}

// The files of a made-up machine: sys/devices/system/cpu, sys/devices and
// proc/cpuinfo below a directory of its own.
struct Machine
{
  fs::path root;
  fs::path cpu_root;
  fs::path devices_root;
  fs::path cpuinfo;

  explicit Machine(const std::string& name)
  {
    root = fs::temp_directory_path() / ("elara_test_" + std::to_string(getpid())) / name;
    fs::remove_all(root);
    devices_root = root / "sys" / "devices";
    cpu_root = devices_root / "system" / "cpu";
    cpuinfo = root / "proc" / "cpuinfo";
    fs::create_directories(cpu_root);
  }

  ~Machine() { fs::remove_all(root); }

  void online(const std::string& list) { put(cpu_root / "online", list); }
  void possible(const std::string& list) { put(cpu_root / "possible", list); }

  // An online cpu: its package, the cpus of its core and of its level 3 cache
  // (empty: no cache information), its capacity (0: the kernel reports none).
  void cpu(unsigned id, long package, const std::string& siblings, const std::string& l3, long capacity = 0)
  {
    const fs::path dir = cpu_root / ("cpu" + std::to_string(id));
    put(dir / "topology" / "physical_package_id", std::to_string(package));
    put(dir / "topology" / "thread_siblings_list", siblings);
    if (!l3.empty())
    {
      // like on a real machine: data and instruction cache of level 1 first
      put(dir / "cache" / "index0" / "level", "1");
      put(dir / "cache" / "index0" / "shared_cpu_list", siblings);
      put(dir / "cache" / "index3" / "level", "3");
      put(dir / "cache" / "index3" / "shared_cpu_list", l3);
    }
    if (capacity)
      put(dir / "cpu_capacity", std::to_string(capacity));
  }

  void max_freq(unsigned id, long khz) { put(cpu_root / ("cpu" + std::to_string(id)) / "cpufreq" / "cpuinfo_max_freq", std::to_string(khz)); }

  std::optional<elara::Topology> read() const { return elara::read_topology(cpu_root, devices_root, cpuinfo); }
};

static void test_cpulist()
{
  using elara::detail::parse_cpulist;
  const auto list = parse_cpulist("0-3,8,10-11");
  CHECK(list && *list == (std::vector<std::uint32_t>{0, 1, 2, 3, 8, 10, 11}));
  const auto one = parse_cpulist("5");
  CHECK(one && *one == std::vector<std::uint32_t>{5});
  const auto empty = parse_cpulist("");
  CHECK(empty && empty->empty());
  CHECK(!parse_cpulist("3-1"));
  CHECK(!parse_cpulist("a-b"));
  CHECK(!parse_cpulist("1,x"));
}

// 8 cpus, 4 cores with SMT, two level 3 caches (like two CCDs).
static void test_smt_two_llcs()
{
  Machine m("smt_two_llcs");
  m.online("0-7");
  m.possible("0-7");
  for (unsigned c = 0; c < 8; ++c)
  {
    const unsigned first = c & ~1u;
    m.cpu(c, 0, std::to_string(first) + "-" + std::to_string(first + 1), c < 4 ? "0-3" : "4-7");
  }

  const auto topo = m.read();
  CHECK(topo.has_value());
  if (!topo)
    return;
  CHECK(topo->nr_cpu_ids == 8);
  CHECK(topo->nr_llcs == 2);
  CHECK(topo->smt);
  CHECK(!topo->capacity_from_core_types);
  for (unsigned c = 0; c < 8; ++c)
  {
    CHECK(topo->cpu_online[c]);
    CHECK(topo->cpu_to_llc[c] == topo->cpu_to_llc[c < 4 ? 0 : 4]);
    CHECK(topo->cpu_sibling[c] == static_cast<std::int32_t>(c ^ 1u));
    CHECK(topo->cpu_capacity[c] == 1024);
  }
  CHECK(topo->cpu_to_llc[0] != topo->cpu_to_llc[4]);
}

// SMT switched off: the second threads (odd cpus) are offline.
static void test_offline_cpus()
{
  Machine m("offline_cpus");
  m.online("0,2,4,6");
  m.possible("0-7");
  for (unsigned c = 0; c < 8; c += 2)
    m.cpu(c, 0, std::to_string(c), "0,2,4,6");

  const auto topo = m.read();
  CHECK(topo.has_value());
  if (!topo)
    return;
  CHECK(topo->nr_cpu_ids == 8);
  CHECK(topo->nr_llcs == 1);
  CHECK(!topo->smt);
  for (unsigned c = 0; c < 8; ++c)
  {
    CHECK(topo->cpu_online[c] == (c % 2 == 0));
    CHECK(topo->cpu_sibling[c] == -1);
  }
}

// No cache information (some virtual machines and ARM boards): one llc per
// package.
static void test_no_cache_info()
{
  Machine m("no_cache_info");
  m.online("0-3");
  m.possible("0-3");
  for (unsigned c = 0; c < 4; ++c)
    m.cpu(c, c / 2, std::to_string(c), "");

  const auto topo = m.read();
  CHECK(topo.has_value());
  if (!topo)
    return;
  CHECK(topo->nr_llcs == 2);
  CHECK(topo->cpu_to_llc[0] == topo->cpu_to_llc[1]);
  CHECK(topo->cpu_to_llc[2] == topo->cpu_to_llc[3]);
  CHECK(topo->cpu_to_llc[0] != topo->cpu_to_llc[2]);
}

// Capacities from the kernel (ARM big.LITTLE): taken as they are.
static void test_kernel_capacities()
{
  Machine m("kernel_capacities");
  m.online("0-3");
  m.possible("0-3");
  for (unsigned c = 0; c < 4; ++c)
    m.cpu(c, 0, std::to_string(c), "0-3", c < 2 ? 1024 : 512);

  const auto topo = m.read();
  CHECK(topo.has_value());
  if (!topo)
    return;
  CHECK(topo->cpu_capacity[0] == 1024);
  CHECK(topo->cpu_capacity[3] == 512);
  CHECK(!topo->capacity_from_core_types);
}

// Intel with performance and efficiency cores and Hyper-Threading: the kernel
// reports no capacities, they come from the core types of the perf driver.
// Raptor Lake (family 6, model 0xb7), performance cores 5 GHz, efficiency cores
// 4 GHz: 4000000 * 1024 / 5000000 = 819 units, each worth 78741 / 100000 of a
// performance core's: 644.
static void test_intel_core_types()
{
  Machine m("intel_core_types");
  m.online("0-7");
  m.possible("0-7");
  for (unsigned c = 0; c < 4; ++c)
  {
    const unsigned first = c & ~1u;
    m.cpu(c, 0, std::to_string(first) + "-" + std::to_string(first + 1), "0-7");
    m.max_freq(c, 5000000);
  }
  for (unsigned c = 4; c < 8; ++c)
  {
    m.cpu(c, 0, std::to_string(c), "0-7");
    m.max_freq(c, 4000000);
  }
  put(m.devices_root / "cpu_core" / "cpus", "0-3");
  put(m.devices_root / "cpu_atom" / "cpus", "4-7");
  put(m.cpuinfo, "processor\t: 0\nvendor_id\t: GenuineIntel\ncpu family\t: 6\nmodel\t\t: 183\nmodel name\t: made up");

  const auto topo = m.read();
  CHECK(topo.has_value());
  if (!topo)
    return;
  CHECK(topo->capacity_from_core_types);
  CHECK(topo->smt);
  for (unsigned c = 0; c < 4; ++c)
    CHECK(topo->cpu_capacity[c] == 1024);
  for (unsigned c = 4; c < 8; ++c)
  {
    CHECK(topo->cpu_capacity[c] == 644);
    CHECK(topo->cpu_sibling[c] == -1);
  }
}

// Without the list of online cpus there is no topology.
static void test_missing_online()
{
  Machine m("missing_online");
  CHECK(!m.read().has_value());
}

// The read-only data of the BPF skeleton that setup_elara_topology() fills in,
// with the names and sizes of source/datatypes.h (MAX_CPUS 512).
struct FakeSkel
{
  struct
  {
    std::uint32_t nr_llcs;
    std::uint32_t cpu_to_llc[512];
    std::uint8_t cpu_online[512];
    std::uint16_t cpu_capacity[512];
    std::uint8_t cpu_fast[512];
    std::uint16_t big_cpus[512];
    std::uint16_t little_cpus[512];
    std::uint32_t nr_big_cpus;
    std::uint32_t nr_little_cpus;
    bool hybrid;
    bool smt;
    std::int32_t cpu_sibling[512];
    std::uint16_t llc_cpus[512];
    std::uint16_t llc_start[512];
    std::uint16_t llc_len[512];
    std::uint32_t nr_llc_cpus;
  } storage{};
  decltype(storage)* rodata = &storage;
};

// The lists of the cpus of each llc (llc_cpus, llc_start, llc_len): the BPF
// part scans an llc through them (claim_idle_in_llc(), pick_balance_cpu(),
// pick_enqueue_cpu(), find_earliest()), so each must hold exactly the online
// cpus of its llc. Two llcs whose cpus interleave (even and odd ids), one cpu
// offline.
static void test_llc_lists()
{
  Machine m("llc_lists");
  m.online("0-4,6-7");
  m.possible("0-7");
  for (unsigned c = 0; c < 8; ++c)
    if (c != 5)
      m.cpu(c, 0, std::to_string(c), c % 2 ? "1,3,5,7" : "0,2,4,6");

  FakeSkel skel;
  CHECK(setup_elara_topology(&skel, m.cpu_root, m.devices_root, m.cpuinfo));
  const auto& ro = *skel.rodata;
  CHECK(ro.nr_llcs == 2);
  if (ro.nr_llcs != 2)
    return;

  std::vector<int> seen(8, 0);
  std::uint32_t total = 0;
  for (std::uint32_t llc = 0; llc < ro.nr_llcs; ++llc)
  {
    total += ro.llc_len[llc];
    for (std::uint32_t i = 0; i < ro.llc_len[llc]; ++i)
    {
      const std::uint32_t c = ro.llc_cpus[ro.llc_start[llc] + i];
      CHECK(c < 8);
      if (c >= 8)
        continue;
      ++seen[c];
      CHECK(ro.cpu_online[c]);
      CHECK(ro.cpu_to_llc[c] == llc);
    }
  }
  // every online cpu exactly once, the offline one nowhere; one llc after the
  // other (find_earliest() goes through the other llcs as one range)
  CHECK(total == 7);
  CHECK(ro.nr_llc_cpus == total);
  for (std::uint32_t llc = 1; llc < ro.nr_llcs; ++llc)
    CHECK(ro.llc_start[llc] == ro.llc_start[llc - 1] + ro.llc_len[llc - 1]);
  for (unsigned c = 0; c < 8; ++c)
    CHECK(seen[c] == (c == 5 ? 0 : 1));
  CHECK(ro.llc_len[ro.cpu_to_llc[0]] == 4);
  CHECK(ro.llc_len[ro.cpu_to_llc[1]] == 3);
}

// Without "possible" the cpu ids come from "online" alone: the sibling of cpu 1,
// cpu 3, is offline and has a higher id than any of them. It must not be
// looked up beyond the known cpus, and cpu 1 has no sibling to use.
static void test_sibling_beyond_ids()
{
  Machine m("sibling_beyond_ids");
  m.online("0-2");
  m.cpu(0, 0, "0,2", "0-2");
  m.cpu(1, 0, "1,3", "0-2");
  m.cpu(2, 0, "0,2", "0-2");

  FakeSkel skel;
  CHECK(setup_elara_topology(&skel, m.cpu_root, m.devices_root, m.cpuinfo));
  const auto& ro = *skel.rodata;
  CHECK(ro.smt);
  CHECK(ro.cpu_sibling[0] == 2);
  CHECK(ro.cpu_sibling[1] == -1);
  CHECK(ro.cpu_sibling[2] == 0);
}

int main()
{
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"cpulist", test_cpulist},
      {"smt_two_llcs", test_smt_two_llcs},
      {"offline_cpus", test_offline_cpus},
      {"no_cache_info", test_no_cache_info},
      {"kernel_capacities", test_kernel_capacities},
      {"intel_core_types", test_intel_core_types},
      {"missing_online", test_missing_online},
      {"llc_lists", test_llc_lists},
      {"sibling_beyond_ids", test_sibling_beyond_ids},
  };
  for (const auto& [name, fn] : tests)
  {
    const int before = failures;
    fn();
    std::cout << (failures == before ? "ok      " : "FAILED  ") << name << "\n";
  }
  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / ("elara_test_" + std::to_string(getpid())), ec);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
