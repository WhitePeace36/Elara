#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace elara
{
namespace fs = std::filesystem;

struct Topology
{
  std::uint32_t nr_cpu_ids = 0;  // highest possible CPU id + 1
  std::uint32_t nr_llcs = 0;
  std::vector<std::uint32_t> cpu_to_llc;  // nr_cpu_ids entries, offline -> 0
  std::vector<bool> cpu_online;           // nr_cpu_ids entries
  std::vector<std::uint32_t> cpu_capacity;  // nr_cpu_ids entries, 1024 = fastest core
  std::vector<std::int32_t> cpu_sibling;    // nr_cpu_ids entries, another cpu of the same core, -1: none
  bool smt = false;                         // a core runs more than one cpu
  bool capacity_from_core_types = false;    // capacities from the Intel core types (see intel_hybrid_capacity())
};

namespace detail
{

inline std::optional<std::string> read_first_line(const fs::path& p)
{
  std::ifstream in(p);
  if (!in.is_open())
    return std::nullopt;
  std::string line;
  if (!std::getline(in, line))
    return std::nullopt;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
    line.pop_back();
  return line;
}

inline std::optional<long> read_long_file(const fs::path& p)
{
  const auto line = read_first_line(p);
  if (!line)
    return std::nullopt;
  try
  {
    std::size_t pos = 0;
    const long v = std::stol(*line, &pos);
    return pos > 0 ? std::optional<long>(v) : std::nullopt;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

inline std::optional<std::vector<std::uint32_t>> parse_cpulist(const std::string& list)
{
  std::vector<std::uint32_t> cpus;
  std::stringstream ss(list);
  std::string tok;
  while (std::getline(ss, tok, ','))
  {
    if (tok.empty())
      continue;
    try
    {
      const auto dash = tok.find('-');
      if (dash == std::string::npos)
      {
        cpus.push_back(static_cast<std::uint32_t>(std::stoul(tok)));
      }
      else
      {
        const auto lo = std::stoul(tok.substr(0, dash));
        const auto hi = std::stoul(tok.substr(dash + 1));
        if (hi < lo)
          return std::nullopt;
        for (unsigned long c = lo; c <= hi; ++c)
          cpus.push_back(static_cast<std::uint32_t>(c));
      }
    }
    catch (...)
    {
      return std::nullopt;
    }
  }
  return cpus;
}

inline std::optional<std::vector<std::uint32_t>> read_cpulist_file(const fs::path& p)
{
  const auto line = read_first_line(p);
  if (!line)
    return std::nullopt;
  return parse_cpulist(*line);
}

// Find the cache/index* directory of the given level for a CPU.
inline std::optional<fs::path> find_cache_level_dir(const fs::path& cpu_dir, long wanted_level)
{
  const fs::path cache_dir = cpu_dir / "cache";
  std::error_code ec;
  if (!fs::exists(cache_dir, ec) || ec)
    return std::nullopt;

  for (const auto& entry : fs::directory_iterator(cache_dir, ec))
  {
    if (ec)
      break;
    const std::string name = entry.path().filename().string();
    if (name.rfind("index", 0) != 0)
      continue;
    const auto level = read_long_file(entry.path() / "level");
    if (level && *level == wanted_level)
      return entry.path();
  }
  return std::nullopt;
}

inline std::optional<std::string> cache_group_key(const fs::path& cpu_dir, long level)
{
  const auto index_dir = find_cache_level_dir(cpu_dir, level);
  if (!index_dir)
    return std::nullopt;

  if (const auto shared = read_first_line(*index_dir / "shared_cpu_list"))
    return "l" + std::to_string(level) + ":shared:" + *shared;

  if (const auto id = read_long_file(*index_dir / "id"))
    return "l" + std::to_string(level) + ":id:" + std::to_string(*id);

  return std::nullopt;
}

// kHz per HWP performance unit of a performance core, the "hybrid scaling factor"
// of intel_pstate (an efficiency core's unit is 100000 kHz). By model, from
// /proc/cpuinfo; models the kernel doesn't list get about the same.
inline std::uint32_t intel_pcore_scaling(const fs::path& cpuinfo)
{
  std::ifstream in(cpuinfo);
  std::string line;
  long family = -1, model = -1;
  while ((family < 0 || model < 0) && std::getline(in, line))
  {
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = line.substr(0, colon);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
      key.pop_back();
    try
    {
      if (key == "cpu family")
        family = std::stol(line.substr(colon + 1));
      else if (key == "model")
        model = std::stol(line.substr(colon + 1));
    }
    catch (...)
    {
    }
  }
  if (family == 6)
  {
    switch (model)
    {
      case 0x97:  // Alder Lake
      case 0x9a:  // Alder Lake L
      case 0xb7:  // Raptor Lake
      case 0xba:  // Raptor Lake P
      case 0xbf:  // Raptor Lake S
      case 0xd7:  // Bartlett Lake
        return 78741;
      case 0xaa:  // Meteor Lake L
        return 80000;
      case 0xbd:  // Lunar Lake M
        return 86957;
    }
  }
  return 80000;
}

// Intel cpus with performance and efficiency cores and Hyper-Threading (12th to
// 14th gen, Meteor Lake): the kernel only reports core capacities with SMT off
// (intel_pstate), so all cores look equally fast. The perf driver tells the core
// types apart (a PMU per type: cpu_core, cpu_atom, cpu_lowpower for the low power
// efficiency cores of Meteor Lake). The capacities are then worked out the way
// intel_pstate does: HWP performance units at the highest frequency, where a unit
// of a performance core is worth intel_pcore_scaling() kHz and one of an
// efficiency core 100000 kHz.
inline void intel_hybrid_capacity(Topology& topo, const fs::path& cpu_root, const fs::path& devices_root, const fs::path& cpuinfo)
{
  // Capacities from the kernel are better (and differ only on such cpus).
  std::uint32_t cap_min = UINT32_MAX, cap_max = 0;
  for (std::uint32_t cpu = 0; cpu < topo.nr_cpu_ids; ++cpu)
    if (topo.cpu_online[cpu])
    {
      cap_min = std::min(cap_min, topo.cpu_capacity[cpu]);
      cap_max = std::max(cap_max, topo.cpu_capacity[cpu]);
    }
  if (cap_min != cap_max)
    return;

  const auto pcores = read_cpulist_file(devices_root / "cpu_core" / "cpus");
  if (!pcores || pcores->empty())
    return;
  std::vector<bool> is_pcore(topo.nr_cpu_ids, false), is_ecore(topo.nr_cpu_ids, false);
  for (const auto cpu : *pcores)
    if (cpu < topo.nr_cpu_ids)
      is_pcore[cpu] = true;
  bool any_ecore = false;
  for (const char* pmu : {"cpu_atom", "cpu_lowpower"})
    if (const auto ecores = read_cpulist_file(devices_root / pmu / "cpus"))
      for (const auto cpu : *ecores)
        if (cpu < topo.nr_cpu_ids && topo.cpu_online[cpu] && !is_pcore[cpu])
          is_ecore[cpu] = any_ecore = true;
  if (!any_ecore)
    return;

  // Highest frequency in kHz (0: unknown), the highest of the performance cores.
  auto max_freq = [&](std::uint32_t cpu) { return read_long_file(cpu_root / ("cpu" + std::to_string(cpu)) / "cpufreq" / "cpuinfo_max_freq").value_or(0); };
  long pcore_freq = 0;
  for (std::uint32_t cpu = 0; cpu < topo.nr_cpu_ids; ++cpu)
    if (topo.cpu_online[cpu] && is_pcore[cpu])
      pcore_freq = std::max(pcore_freq, max_freq(cpu));
  const std::uint64_t scaling = intel_pcore_scaling(cpuinfo);

  for (std::uint32_t cpu = 0; cpu < topo.nr_cpu_ids; ++cpu)
  {
    if (!topo.cpu_online[cpu] || (!is_pcore[cpu] && !is_ecore[cpu]))
      continue;
    const long freq = max_freq(cpu);
    // Relative to the fastest performance core: same unit for performance cores,
    // a unit of an efficiency core is worth 100000 / scaling of theirs. Without
    // frequencies, as fast as a performance core per MHz.
    const std::uint64_t ratio_1024 = pcore_freq > 0 && freq > 0 ? static_cast<std::uint64_t>(freq) * 1024 / static_cast<std::uint64_t>(pcore_freq) : 1024;
    std::uint64_t cap = is_pcore[cpu] ? ratio_1024 : ratio_1024 * scaling / 100000;
    // An efficiency core always counts as one (see SLOW_CORE_PERCENT), whatever the
    // frequencies say; a performance core never as one.
    if (is_ecore[cpu])
      cap = std::min<std::uint64_t>(cap, 1024 * 80 / 100);
    else
      cap = std::max<std::uint64_t>(cap, 1024 * 90 / 100);
    topo.cpu_capacity[cpu] = static_cast<std::uint32_t>(std::max<std::uint64_t>(std::min<std::uint64_t>(cap, 1024), 1));
  }
  topo.capacity_from_core_types = true;
}

}  // namespace detail

inline std::optional<Topology> read_topology(const fs::path& cpu_root = "/sys/devices/system/cpu", const fs::path& devices_root = "/sys/devices",
                                             const fs::path& cpuinfo = "/proc/cpuinfo")
{
  using namespace detail;

  const auto online = read_cpulist_file(cpu_root / "online");
  if (!online || online->empty())
  {
    std::cerr << "elara: failed to read online CPUs from " << cpu_root << "/online\n";
    return std::nullopt;
  }

  std::uint32_t max_cpu_id = 0;
  const auto possible = read_cpulist_file(cpu_root / "possible");
  const auto& id_source = (possible && !possible->empty()) ? *possible : *online;
  for (const auto cpu : id_source)
    max_cpu_id = std::max(max_cpu_id, cpu);
  for (const auto cpu : *online)
    max_cpu_id = std::max(max_cpu_id, cpu);

  Topology topo;
  topo.nr_cpu_ids = max_cpu_id + 1;
  topo.cpu_to_llc.assign(topo.nr_cpu_ids, 0);
  topo.cpu_online.assign(topo.nr_cpu_ids, false);
  topo.cpu_capacity.assign(topo.nr_cpu_ids, 1024);
  topo.cpu_sibling.assign(topo.nr_cpu_ids, -1);

  std::map<std::string, std::uint32_t> group_to_dense;

  for (const auto cpu : *online)
  {
    topo.cpu_online[cpu] = true;

    const fs::path cpu_dir = cpu_root / ("cpu" + std::to_string(cpu));
    const long package = read_long_file(cpu_dir / "topology" / "physical_package_id").value_or(0);

    auto key = cache_group_key(cpu_dir, 3);
    if (!key)
      key = cache_group_key(cpu_dir, 2);
    if (!key)
      key = "nocache";

    // Relative speed of the core (1024: the fastest ones). Only differs on cpus
    // with performance and efficiency cores.
    const long capacity = read_long_file(cpu_dir / "cpu_capacity").value_or(1024);
    topo.cpu_capacity[cpu] = capacity > 0 ? static_cast<std::uint32_t>(capacity) : 1024;

    if (const auto siblings = read_cpulist_file(cpu_dir / "topology" / "thread_siblings_list"))
      for (const auto sibling : *siblings)
        if (sibling != cpu)
        {
          topo.smt = true;
          topo.cpu_sibling[cpu] = static_cast<std::int32_t>(sibling);
          break;
        }

    const std::string full_key = "p" + std::to_string(package) + ":" + *key;
    const auto [it, inserted] = group_to_dense.try_emplace(full_key, static_cast<std::uint32_t>(group_to_dense.size()));
    (void)inserted;
    topo.cpu_to_llc[cpu] = it->second;
  }

  topo.nr_llcs = static_cast<std::uint32_t>(group_to_dense.size());
  intel_hybrid_capacity(topo, cpu_root, devices_root, cpuinfo);
  return topo;
}

}  // namespace elara

// A core counts as slower (an efficiency core) when its capacity is below this
// percentage of the fastest one.
constexpr std::uint32_t SLOW_CORE_PERCENT = 85;

template <typename Skel>
bool setup_elara_topology(Skel* skel, const std::filesystem::path& cpu_root = "/sys/devices/system/cpu", const std::filesystem::path& devices_root = "/sys/devices",
                          const std::filesystem::path& cpuinfo = "/proc/cpuinfo")
{
  const auto topo = elara::read_topology(cpu_root, devices_root, cpuinfo);
  if (!topo)
  {
    return false;
  }

  const auto max_cpus = static_cast<std::uint32_t>(std::size(skel->rodata->cpu_to_llc));
  if (topo->nr_cpu_ids > max_cpus)
  {
    std::cerr << "elara: system has " << topo->nr_cpu_ids << " possible CPU ids, but MAX_CPUS is " << max_cpus << "; bump it in defines.h\n";
    return false;
  }
  if (topo->nr_llcs == 0)
  {
    std::cerr << "elara: topology reported zero LLC domains\n";
    return false;
  }

  skel->rodata->nr_llcs = topo->nr_llcs;

  std::cout << "Number of found llcs: " << topo->nr_llcs << std::endl;

  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    skel->rodata->cpu_to_llc[cpu] = topo->cpu_to_llc[cpu];
    skel->rodata->cpu_online[cpu] = topo->cpu_online[cpu] ? 1 : 0;
  }

  std::uint32_t nr_online = 0;
  std::uint32_t cap_max = 0;
  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    if (topo->cpu_online[cpu])
    {
      ++nr_online;
      cap_max = std::max(cap_max, topo->cpu_capacity[cpu]);
    }
  }

  // Cores clearly slower than the fastest ones (efficiency cores) and the fast
  // ones, each list sorted by capacity, fastest first. "Clearly": performance
  // cores differ a little too (intel_pstate reports the favored cores of Turbo
  // Boost Max 3.0 a few percent higher), those are still fast cores.
  std::vector<std::uint32_t> big, little;
  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    if (!topo->cpu_online[cpu])
      continue;
    (topo->cpu_capacity[cpu] * 100 < cap_max * SLOW_CORE_PERCENT ? little : big).push_back(cpu);
  }
  std::stable_sort(big.begin(), big.end(), [&](std::uint32_t a, std::uint32_t b) { return topo->cpu_capacity[a] > topo->cpu_capacity[b]; });
  std::stable_sort(little.begin(), little.end(), [&](std::uint32_t a, std::uint32_t b) { return topo->cpu_capacity[a] > topo->cpu_capacity[b]; });
  for (std::uint32_t i = 0; i < big.size(); ++i)
  {
    skel->rodata->big_cpus[i] = static_cast<std::uint16_t>(big[i]);
    skel->rodata->cpu_fast[big[i]] = 1;
  }
  for (std::uint32_t i = 0; i < little.size(); ++i)
    skel->rodata->little_cpus[i] = static_cast<std::uint16_t>(little[i]);
  // The cpus of each llc, one llc after the other.
  std::uint32_t pos = 0;
  for (std::uint32_t llc = 0; llc < topo->nr_llcs; ++llc)
  {
    skel->rodata->llc_start[llc] = static_cast<std::uint16_t>(pos);
    for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
      if (topo->cpu_online[cpu] && topo->cpu_to_llc[cpu] == llc)
        skel->rodata->llc_cpus[pos++] = static_cast<std::uint16_t>(cpu);
    skel->rodata->llc_len[llc] = static_cast<std::uint16_t>(pos - skel->rodata->llc_start[llc]);
  }
  skel->rodata->nr_big_cpus = static_cast<std::uint32_t>(big.size());
  skel->rodata->nr_little_cpus = static_cast<std::uint32_t>(little.size());
  skel->rodata->hybrid = !little.empty();
  skel->rodata->smt = topo->smt;
  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    skel->rodata->cpu_capacity[cpu] = static_cast<std::uint16_t>(cap_max ? topo->cpu_capacity[cpu] * 1024 / cap_max : 1024);
    const std::int32_t sibling = topo->cpu_sibling[cpu];
    skel->rodata->cpu_sibling[cpu] = sibling >= 0 && topo->cpu_online[sibling] ? sibling : -1;
  }

  std::cerr << "elara: topology: " << topo->nr_cpu_ids << " cpu ids (" << nr_online << " online), " << topo->nr_llcs << " llc domain(s), smt "
            << (topo->smt ? "on" : "off") << ", " << (little.empty() ? "all cores equally fast" : std::to_string(big.size()) + " fast and " + std::to_string(little.size()) + " slower cores")
            << (topo->capacity_from_core_types ? " (capacities from the Intel core types)" : "") << "\n";
  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    if (topo->cpu_online[cpu])
      std::cerr << "  cpu" << cpu << " -> llc" << topo->cpu_to_llc[cpu] << ", capacity " << topo->cpu_capacity[cpu] << "\n";
    else
      std::cerr << "  cpu" << cpu << " offline -> llc0 (unused)\n";
  }

  return true;
}
