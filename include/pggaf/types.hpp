#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pggaf {

struct OrientedNode {
  std::uint64_t id      : 63;
  std::uint64_t reverse : 1;

  bool operator==(const OrientedNode& other) const = default;
};

struct GafRecord {
  std::string qname;
  std::uint64_t query_length = 0;
  std::uint64_t query_start = 0;
  std::uint64_t query_end = 0;
  char strand = '+';
  std::vector<OrientedNode> nodes;
  std::uint64_t target_length = 0;
  std::uint64_t target_start = 0;
  std::uint64_t target_end = 0;
  std::uint64_t residue_matches = 0;
  std::uint64_t block_length = 0;
  std::uint32_t mapq = 0;
};

struct SubpathResult {
  std::uint32_t begin_offset = 0;
  std::uint32_t end_offset = 0;
  std::vector<std::uint64_t> thread_ids;
};

struct ReadAnnotation {
  std::vector<std::uint32_t> hs;
  std::vector<std::uint32_t> hb;
  std::vector<std::uint32_t> he;
};

struct DecodedThread {
  std::uint64_t thread_id = 0;
  std::uint64_t path_id = 0;
  std::string sample;
  std::optional<std::uint64_t> haplotype;
  std::string locus;
  std::string path_name;
};

}  // namespace pggaf
