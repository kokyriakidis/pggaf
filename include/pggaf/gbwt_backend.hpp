#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pggaf/types.hpp"

namespace pggaf {

class GraphIndex {
public:
  struct Config {
    std::string gbz_path;
    std::string r_index_path;
    bool build_ref_intervals = false;
    std::string ref_sample;
  };

  struct RefInterval {
    std::string chrom;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
  };

  struct ThreadMetadata {
    std::uint64_t thread_id = 0;
    std::uint64_t path_id = 0;
    std::string sample;
    std::uint64_t haplotype = 0;
    bool haplotype_known = false;
    std::string locus;
    std::string path_name;
  };

  virtual ~GraphIndex() = default;

  virtual std::vector<SubpathResult> find_subpaths(const std::vector<OrientedNode>& walk) const = 0;
  virtual std::optional<RefInterval> walk_ref_interval(const std::vector<OrientedNode>& walk,
                                                       std::uint64_t target_start,
                                                       std::uint64_t target_end) const = 0;
  virtual ThreadMetadata decode_thread(std::uint64_t thread_id) const = 0;
};

std::unique_ptr<GraphIndex> make_graph_index(const GraphIndex::Config& config);

}  // namespace pggaf
