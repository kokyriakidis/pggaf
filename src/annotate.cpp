#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pggaf/error.hpp"
#include "pggaf/fingerprint.hpp"
#include "pggaf/gaf.hpp"
#include "pggaf/gbwt_backend.hpp"
#include "pggaf/options.hpp"
#include "pggaf/sidecar.hpp"

namespace pggaf {
namespace {

struct VectorHash {
  std::size_t operator()(const std::vector<std::uint64_t>& values) const noexcept {
    std::size_t seed = 0;
    for (std::uint64_t value : values) {
      seed ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    }
    return seed;
  }
};

class SidecarInterner {
public:
  explicit SidecarInterner(SidecarWriter& writer) : writer_(writer) {}

  std::uint32_t intern(const std::vector<std::uint64_t>& thread_ids) {
    auto found = set_to_id_.find(thread_ids);
    if (found != set_to_id_.end()) {
      return found->second;
    }

    const std::uint32_t assigned = next_set_id_++;
    set_to_id_.emplace(thread_ids, assigned);
    writer_.write_set(SidecarSetRecord{assigned, thread_ids});
    return assigned;
  }

private:
  SidecarWriter& writer_;
  std::unordered_map<std::vector<std::uint64_t>, std::uint32_t, VectorHash> set_to_id_;
  std::uint32_t next_set_id_ = 0;
};

bool subpath_less(const SubpathResult& left, const SubpathResult& right) {
  if (left.begin_offset != right.begin_offset) {
    return left.begin_offset < right.begin_offset;
  }
  if (left.end_offset != right.end_offset) {
    return left.end_offset < right.end_offset;
  }
  return std::lexicographical_compare(left.thread_ids.begin(), left.thread_ids.end(),
                                      right.thread_ids.begin(), right.thread_ids.end());
}

bool subpath_equal(const SubpathResult& left, const SubpathResult& right) {
  return left.begin_offset == right.begin_offset &&
         left.end_offset == right.end_offset &&
         left.thread_ids == right.thread_ids;
}

void deduplicate_subpaths(std::vector<SubpathResult>& subpaths) {
  std::sort(subpaths.begin(), subpaths.end(), subpath_less);
  subpaths.erase(std::unique(subpaths.begin(), subpaths.end(), subpath_equal), subpaths.end());
}

std::vector<SubpathResult> collect_subpaths(const GraphIndex& graph,
                                            const std::vector<std::vector<OrientedNode>>& walks) {
  std::vector<SubpathResult> subpaths;
  for (const std::vector<OrientedNode>& walk : walks) {
    std::vector<SubpathResult> walk_subpaths = graph.find_subpaths(walk);
    subpaths.insert(subpaths.end(),
                    std::make_move_iterator(walk_subpaths.begin()),
                    std::make_move_iterator(walk_subpaths.end()));
  }
  if (subpaths.size() > 1) {
    deduplicate_subpaths(subpaths);
  }
  return subpaths;
}

ReadAnnotation build_annotation(const std::vector<SubpathResult>& subpaths,
                                SidecarInterner& interner) {
  ReadAnnotation annotation;
  annotation.hs.reserve(subpaths.size());
  annotation.hb.reserve(subpaths.size());
  annotation.he.reserve(subpaths.size());

  for (const SubpathResult& subpath : subpaths) {
    if (subpath.thread_ids.empty()) {
      continue;
    }
    annotation.hs.push_back(interner.intern(subpath.thread_ids));
    annotation.hb.push_back(subpath.begin_offset);
    annotation.he.push_back(subpath.end_offset);
  }

  return annotation;
}

ReadAnnotation build_annotation(const GraphIndex& graph,
                                const std::vector<std::vector<OrientedNode>>& walks,
                                SidecarInterner& interner) {
  return build_annotation(collect_subpaths(graph, walks), interner);
}

std::string build_command_line(const AnnotateGafOptions& options) {
  std::ostringstream out;
  out << "pggaf annotate-gaf --gaf " << options.gaf_path
      << " --gbz " << options.gbz_path
      << " --out-gaf " << options.out_gaf_path
      << " --out-sets " << options.out_sets_path;
  if (options.use_r_index()) {
    out << " --r-index " << options.r_index_path;
  }
  if (!options.ref_sample.empty()) {
    out << " --ref-sample " << options.ref_sample;
  }
  if (options.primary_only) {
    out << " --primary-only";
  }
  return out.str();
}

void write_gaf_array_tag(std::ostream& out,
                         std::string_view tag,
                         const std::vector<std::uint32_t>& values) {
  if (values.empty()) {
    return;
  }

  out << '\t' << tag << ":B:I";
  for (std::uint32_t value : values) {
    out << ',' << value;
  }
}

void write_gaf_string_tag(std::ostream& out, std::string_view tag, std::string_view value) {
  if (value.empty()) {
    return;
  }
  out << '\t' << tag << ":Z:" << value;
}

void write_gaf_int_tag(std::ostream& out, std::string_view tag, std::uint64_t value) {
  out << '\t' << tag << ":i:" << value;
}

void write_gaf_annotation_line(std::ostream& out,
                               const std::string& line,
                               const ReadAnnotation& annotation,
                               const std::optional<GraphIndex::RefInterval>& ref_interval) {
  out << line;
  if (ref_interval) {
    write_gaf_string_tag(out, "rc", ref_interval->chrom);
    write_gaf_int_tag(out, "rb", ref_interval->start);
    write_gaf_int_tag(out, "re", ref_interval->end);
  }
  write_gaf_array_tag(out, "hs", annotation.hs);
  write_gaf_array_tag(out, "hb", annotation.hb);
  write_gaf_array_tag(out, "he", annotation.he);
  out << '\n';
}

}  // namespace

int run_annotate_gaf(const AnnotateGafOptions& options) {
  const std::unique_ptr<GraphIndex> graph = make_graph_index(GraphIndex::Config{
    options.gbz_path,
    options.r_index_path,
    true,
    options.ref_sample,
  });

  const std::string fingerprint = sha256_file(options.gbz_path);
  SidecarWriter sidecar(options.out_sets_path);
  sidecar.write_header(SidecarHeader{1, fingerprint, options.use_r_index()});
  SidecarInterner interner(sidecar);

  std::ifstream input(options.gaf_path);
  if (!input) {
    throw Error("cannot open GAF file: " + options.gaf_path);
  }
  std::ofstream output(options.out_gaf_path);
  if (!output) {
    throw Error("cannot open GAF output: " + options.out_gaf_path);
  }

  std::vector<std::string> lines;
  std::vector<std::vector<OrientedNode>> walks;
  std::vector<std::uint32_t> walk_mapqs;
  std::vector<std::optional<GraphIndex::RefInterval>> ref_intervals;
  std::string current_qname;
  std::size_t line_number = 0;

  auto flush_block = [&]() {
    if (lines.empty()) {
      return;
    }
    ReadAnnotation annotation;
    if (options.primary_only && !walks.empty()) {
      const std::size_t best = static_cast<std::size_t>(
          std::max_element(walk_mapqs.begin(), walk_mapqs.end()) - walk_mapqs.begin());
      const std::vector<std::vector<OrientedNode>> primary_walk = {walks[best]};
      annotation = build_annotation(*graph, primary_walk, interner);
    } else {
      annotation = build_annotation(*graph, walks, interner);
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
      write_gaf_annotation_line(output, lines[i], annotation, ref_intervals[i]);
    }
    if (!output) {
      throw Error("failed to write GAF record to " + options.out_gaf_path);
    }
    lines.clear();
    walks.clear();
    walk_mapqs.clear();
    ref_intervals.clear();
    current_qname.clear();
  };

  std::string line;
  while (std::getline(input, line)) {
    ++line_number;
    auto record = parse_gaf_line(line);
    if (!record) {
      flush_block();
      output << line << '\n';
      if (!output) {
        throw Error("failed to write GAF record to " + options.out_gaf_path);
      }
      continue;
    }

    if (current_qname.empty()) {
      current_qname = record->qname;
      lines.push_back(line);
      ref_intervals.push_back(graph->walk_ref_interval(record->nodes, record->target_start, record->target_end));
      walks.push_back(record->nodes);
      walk_mapqs.push_back(record->mapq);
      continue;
    }

    if (record->qname != current_qname) {
      flush_block();
      current_qname = record->qname;
      lines.push_back(line);
      ref_intervals.push_back(graph->walk_ref_interval(record->nodes, record->target_start, record->target_end));
      walks.push_back(record->nodes);
      walk_mapqs.push_back(record->mapq);
      continue;
    }

    lines.push_back(line);
    ref_intervals.push_back(graph->walk_ref_interval(record->nodes, record->target_start, record->target_end));
    walks.push_back(record->nodes);
    walk_mapqs.push_back(record->mapq);
  }

  flush_block();

  return 0;
}

}  // namespace pggaf
