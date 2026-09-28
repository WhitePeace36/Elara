#pragma once

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

// Parse a kernel cpulist such as "0-3,8,10-11" into cpu ids.
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

// Build a grouping key for one cache level of one CPU: group by the
// "shared_cpu_list" string (the cpus that really share this cache), otherwise
// by the "id" attribute. The id alone is not reliable: QEMU for example gives
// every cpu its own L3 id although all of them share one L3.
// nullopt means no usable info at this level.
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

}  // namespace detail

// Read the topology from sysfs. cpu_root is overridable for testing and
// defaults to the real sysfs location.
inline std::optional<Topology> read_topology(const fs::path& cpu_root = "/sys/devices/system/cpu")
{
  using namespace detail;

  const auto online = read_cpulist_file(cpu_root / "online");
  if (!online || online->empty())
  {
    std::cerr << "elara: failed to read online CPUs from " << cpu_root << "/online\n";
    return std::nullopt;
  }

  // nr_cpu_ids: highest possible CPU id + 1 (matches scx_utils NR_CPU_IDS).
  // Fall back to the online list if "possible" is unreadable.
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

  // Dense LLC ids, assigned in CPU order over online CPUs only. The key
  // includes the physical package id like scx_utils' (node, package,
  // kernel_id) tuple, since kernel cache ids are only meaningful per package.
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

    const std::string full_key = "p" + std::to_string(package) + ":" + *key;
    const auto [it, inserted] = group_to_dense.try_emplace(full_key, static_cast<std::uint32_t>(group_to_dense.size()));
    (void)inserted;
    topo.cpu_to_llc[cpu] = it->second;
  }

  topo.nr_llcs = static_cast<std::uint32_t>(group_to_dense.size());
  return topo;
}

}  // namespace elara

template <typename Skel>
bool setup_elara_topology(Skel* skel, const std::filesystem::path& cpu_root = "/sys/devices/system/cpu")
{
  const auto topo = elara::read_topology(cpu_root);
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
  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    if (topo->cpu_online[cpu])
      ++nr_online;
  }

  std::cerr << "elara: topology: " << topo->nr_cpu_ids << " cpu ids (" << nr_online << " online), " << topo->nr_llcs << " llc domain(s)\n";
  for (std::uint32_t cpu = 0; cpu < topo->nr_cpu_ids; ++cpu)
  {
    if (topo->cpu_online[cpu])
      std::cerr << "  cpu" << cpu << " -> llc" << topo->cpu_to_llc[cpu] << "\n";
    else
      std::cerr << "  cpu" << cpu << " offline -> llc0 (unused)\n";
  }

  return true;
}
