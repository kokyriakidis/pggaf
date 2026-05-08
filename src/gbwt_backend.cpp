#include "pggaf/gbwt_backend.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gbwt/fast_locate.h>
#include <gbwtgraph/gbz.h>
#include <gbwtgraph/utils.h>
#include <handlegraph/path_metadata.hpp>

#include "pggaf/error.hpp"

namespace pggaf {
namespace {

class GbwtGraphIndex final : public GraphIndex {
  struct RefNodePlacement {
    std::uint64_t path_id = 0;
    std::uint64_t step_index = 0;
    std::uint32_t chrom_index = 0;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    bool reverse = false;
  };

  struct RefNodeMapEntry {
    RefNodePlacement first;
    std::vector<RefNodePlacement> alternates;
  };

public:
  explicit GbwtGraphIndex(const Config& config) {
    {
      auto gbz = std::make_unique<gbwtgraph::GBZ>();
      std::ifstream input(config.gbz_path, std::ios::binary);
      if (!input) {
        throw Error("cannot open GBZ file: " + config.gbz_path);
      }
      gbz->simple_sds_load(input);
      index_ = &gbz->index;
      gbz_ = std::move(gbz);
    }

    reference_samples_ = gbwtgraph::parse_reference_samples_tag(*index_);

    if (!config.r_index_path.empty()) {
      auto fast_locate = std::make_unique<gbwt::FastLocate>();
      std::ifstream input(config.r_index_path, std::ios::binary);
      if (!input) {
        throw Error("cannot open r-index file: " + config.r_index_path);
      }
      fast_locate->load(input);
      fast_locate->setGBWT(*index_);
      fast_locate_ = std::move(fast_locate);
    }

    if (config.build_ref_intervals) {
      build_reference_map(config.ref_sample);
    }
  }

  std::vector<SubpathResult> find_subpaths(const std::vector<OrientedNode>& walk) const override {
    std::vector<gbwt::node_type> encoded;
    encoded.reserve(walk.size());
    for (const OrientedNode& node : walk) {
      encoded.push_back(gbwt::Node::encode(node.id, node.reverse));
    }

    std::vector<SubpathResult> result;
    std::size_t start = 0;
    while (start < encoded.size()) {
      gbwt::SearchState state;
      gbwt::FastLocate::size_type first_occurrence = gbwt::FastLocate::NO_POSITION;
      if (fast_locate_) {
        state = fast_locate_->find(encoded[start], first_occurrence);
      } else {
        state = index_->find(encoded[start]);
      }

      if (state.empty()) {
        ++start;
        continue;
      }

      std::size_t end = start + 1;
      while (end < encoded.size()) {
        gbwt::SearchState next_state;
        gbwt::FastLocate::size_type next_first = first_occurrence;
        if (fast_locate_) {
          next_state = fast_locate_->extend(state, encoded[end], next_first);
        } else {
          next_state = index_->extend(state, encoded[end]);
        }
        if (next_state.empty()) {
          break;
        }
        state = next_state;
        first_occurrence = next_first;
        ++end;
      }

      std::vector<gbwt::size_type> located;
      if (fast_locate_) {
        located = fast_locate_->locate(state, first_occurrence);
      } else {
        located = index_->locate(state);
      }

      std::vector<std::uint64_t> path_ids;
      path_ids.reserve(located.size());
      for (gbwt::size_type sequence_id : located) {
        path_ids.push_back(static_cast<std::uint64_t>(gbwt::Path::id(sequence_id)));
      }
      path_ids.erase(std::unique(path_ids.begin(), path_ids.end()), path_ids.end());

      result.push_back(SubpathResult{
        static_cast<std::uint32_t>(start),
        static_cast<std::uint32_t>(end),
        std::move(path_ids),
      });
      start = end;
    }

    return result;
  }

  std::optional<RefInterval> walk_ref_interval(const std::vector<OrientedNode>& walk,
                                               std::uint64_t target_start,
                                               std::uint64_t target_end) const override {
    if (gbz_ == nullptr || node_ref_map_.empty() || walk.empty() || target_end <= target_start) {
      return std::nullopt;
    }

    struct Anchor {
      OrientedNode node;
      std::uint64_t local_start = 0;
      std::uint64_t local_end = 0;
      const RefNodeMapEntry* placements = nullptr;
    };

    std::vector<Anchor> anchors;
    std::uint64_t path_offset = 0;

    for (const OrientedNode& node : walk) {
      if (node.id > static_cast<std::uint64_t>(std::numeric_limits<gbwtgraph::nid_t>::max())) {
        return std::nullopt;
      }
      if (!gbz_->graph.has_node(static_cast<gbwtgraph::nid_t>(node.id))) {
        return std::nullopt;
      }

      const std::uint64_t node_length = static_cast<std::uint64_t>(
        gbz_->graph.get_length(gbwtgraph::GBWTGraph::node_to_handle(gbwt::Node::encode(node.id, false))));
      const std::uint64_t next_offset = path_offset + node_length;
      if (node_length != 0 && next_offset > target_start && path_offset < target_end) {
        const std::uint64_t local_start = (target_start > path_offset) ? (target_start - path_offset) : 0;
        const std::uint64_t local_end = (target_end < next_offset) ? (target_end - path_offset) : node_length;
        if (local_start < local_end) {
          const auto found = node_ref_map_.find(node.id);
          if (found != node_ref_map_.end()) {
            anchors.push_back(Anchor{node, local_start, local_end, &(found->second)});
          }
        }
      }

      path_offset = next_offset;
      if (path_offset >= target_end) {
        break;
      }
    }

    if (anchors.empty()) {
      return std::nullopt;
    }

    auto projected_interval = [](const Anchor& anchor, const RefNodePlacement& placement) {
      std::uint64_t ref_start = 0;
      std::uint64_t ref_end = 0;
      if (anchor.node.reverse == placement.reverse) {
        ref_start = placement.start + anchor.local_start;
        ref_end = placement.start + anchor.local_end;
      } else {
        ref_start = placement.start + (placement.length - anchor.local_end);
        ref_end = placement.start + (placement.length - anchor.local_start);
      }
      return std::pair<std::uint64_t, std::uint64_t>{ref_start, ref_end};
    };

    auto for_each_placement = [](const RefNodeMapEntry& entry, const auto& callback) {
      callback(entry.first);
      for (const RefNodePlacement& placement : entry.alternates) {
        callback(placement);
      }
    };

    auto try_candidate = [&](const RefNodePlacement& first_placement) -> std::optional<RefInterval> {
      if (first_placement.chrom_index >= ref_chroms_.size()) {
        return std::nullopt;
      }

      const bool forward_on_reference = (anchors.front().node.reverse == first_placement.reverse);
      std::uint64_t previous_step = first_placement.step_index;
      auto first_interval = projected_interval(anchors.front(), first_placement);
      std::uint64_t interval_start = first_interval.first;
      std::uint64_t interval_end = first_interval.second;

      for (std::size_t i = 1; i < anchors.size(); ++i) {
        const RefNodePlacement* match = nullptr;
        bool multiple_matches = false;
        for_each_placement(*(anchors[i].placements), [&](const RefNodePlacement& placement) {
          if (multiple_matches || placement.path_id != first_placement.path_id ||
              placement.chrom_index != first_placement.chrom_index ||
              (anchors[i].node.reverse == placement.reverse) != forward_on_reference) {
            return;
          }
          if (forward_on_reference) {
            if (placement.step_index <= previous_step) {
              return;
            }
          } else if (placement.step_index >= previous_step) {
            return;
          }
          if (match != nullptr) {
            multiple_matches = true;
            return;
          }
          match = &placement;
        });

        if (match == nullptr || multiple_matches) {
          return std::nullopt;
        }

        previous_step = match->step_index;
        auto ref_interval = projected_interval(anchors[i], *match);
        interval_start = std::min(interval_start, ref_interval.first);
        interval_end = std::max(interval_end, ref_interval.second);
      }

      if (interval_start >= interval_end) {
        return std::nullopt;
      }
      return RefInterval{ref_chroms_[first_placement.chrom_index], interval_start, interval_end};
    };

    std::optional<RefInterval> resolved;
    bool ambiguous = false;
    for_each_placement(*(anchors.front().placements), [&](const RefNodePlacement& placement) {
      if (ambiguous) {
        return;
      }
      std::optional<RefInterval> candidate = try_candidate(placement);
      if (!candidate) {
        return;
      }
      if (!resolved) {
        resolved = std::move(candidate);
        return;
      }
      if (resolved->chrom != candidate->chrom ||
          resolved->start != candidate->start ||
          resolved->end != candidate->end) {
        ambiguous = true;
      }
    });

    if (ambiguous) {
      return std::nullopt;
    }
    return resolved;
  }

  ThreadMetadata decode_thread(std::uint64_t thread_id) const override {
    const gbwt::size_type path_id = static_cast<gbwt::size_type>(thread_id);
    const gbwtgraph::PathSense sense = gbwtgraph::get_path_sense(*index_, path_id, reference_samples_);

    ThreadMetadata metadata;
    metadata.thread_id = thread_id;
    metadata.path_id = thread_id;
    metadata.sample = gbwtgraph::get_path_sample_name(*index_, path_id, sense);
    const std::size_t haplotype = gbwtgraph::get_path_haplotype(*index_, path_id, sense);
    metadata.haplotype_known = (haplotype != handlegraph::PathMetadata::NO_HAPLOTYPE);
    metadata.haplotype = static_cast<std::uint64_t>(haplotype);
    metadata.locus = gbwtgraph::get_path_locus_name(*index_, path_id, sense);
    metadata.path_name = gbwtgraph::compose_path_name(*index_, path_id, sense);
    return metadata;
  }

private:
  static bool same_placement(const RefNodePlacement& left, const RefNodePlacement& right) {
    return left.path_id == right.path_id &&
           left.step_index == right.step_index &&
           left.chrom_index == right.chrom_index &&
           left.start == right.start &&
           left.length == right.length &&
           left.reverse == right.reverse;
  }

  void add_ref_node_placement(std::uint64_t node_id, const RefNodePlacement& placement) {
    auto inserted = node_ref_map_.emplace(node_id, RefNodeMapEntry{placement, {}});
    if (inserted.second || same_placement(inserted.first->second.first, placement)) {
      return;
    }
    for (const RefNodePlacement& alternate : inserted.first->second.alternates) {
      if (same_placement(alternate, placement)) {
        return;
      }
    }
    inserted.first->second.alternates.push_back(placement);
  }

  void build_reference_map(const std::string& ref_sample) {
    if (gbz_ == nullptr) {
      return;
    }
    if (index_ == nullptr || !index_->hasMetadata() || !index_->metadata.hasPathNames()) {
      return;
    }

    gbwtgraph::sample_name_set active_reference_samples = reference_samples_;
    if (!ref_sample.empty()) {
      active_reference_samples.clear();
      active_reference_samples.insert(ref_sample);
    }
    if (active_reference_samples.empty()) {
      return;
    }

    node_ref_map_.clear();
    ref_chroms_.clear();
    std::unordered_map<std::string, std::uint32_t> chrom_to_index;
    auto get_chrom_index = [&](const std::string& chrom) -> std::uint32_t {
      const auto found = chrom_to_index.find(chrom);
      if (found != chrom_to_index.end()) {
        return found->second;
      }
      if (ref_chroms_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw Error("too many reference contigs to index");
      }
      const std::uint32_t index = static_cast<std::uint32_t>(ref_chroms_.size());
      ref_chroms_.push_back(chrom);
      chrom_to_index.emplace(ref_chroms_.back(), index);
      return index;
    };

    std::size_t reference_paths = 0;
    for (gbwt::size_type path_id = 0; path_id < index_->metadata.paths(); ++path_id) {
      const gbwtgraph::PathSense sense = gbwtgraph::get_path_sense(*index_, path_id, active_reference_samples);
      if (sense != gbwtgraph::PathSense::REFERENCE) {
        continue;
      }
      const std::string sample = gbwtgraph::get_path_sample_name(*index_, path_id, sense);
      if (!ref_sample.empty() && sample != ref_sample) {
        continue;
      }
      const std::string chrom = gbwtgraph::get_path_locus_name(*index_, path_id, sense);
      if (chrom.empty()) {
        continue;
      }

      ++reference_paths;
      const std::uint32_t chrom_index = get_chrom_index(chrom);
      std::uint64_t position = 0;
      const gbwtgraph::subrange_t subrange = gbwtgraph::get_path_subrange(*index_, path_id, sense);
      if (subrange != gbwtgraph::PathMetadata::NO_SUBRANGE) {
        position = static_cast<std::uint64_t>(subrange.first);
      }

      const gbwt::vector_type path = index_->extract(gbwt::Path::encode(path_id, false));
      for (std::uint64_t step_index = 0; step_index < path.size(); ++step_index) {
        const gbwt::node_type encoded_node = path[step_index];
        const std::uint64_t node_id = static_cast<std::uint64_t>(gbwt::Node::id(encoded_node));
        const std::uint64_t node_length = static_cast<std::uint64_t>(
          gbz_->graph.get_length(gbwtgraph::GBWTGraph::node_to_handle(encoded_node)));
        add_ref_node_placement(node_id, RefNodePlacement{
          static_cast<std::uint64_t>(path_id),
          step_index,
          chrom_index,
          position,
          node_length,
          gbwt::Node::is_reverse(encoded_node),
        });
        position += node_length;
      }
    }

    if (!ref_sample.empty() && reference_paths == 0) {
      throw Error("no reference paths found for --ref-sample " + ref_sample);
    }
  }

  std::unique_ptr<gbwtgraph::GBZ> gbz_;
  const gbwt::GBWT* index_ = nullptr;
  std::unique_ptr<gbwt::FastLocate> fast_locate_;
  gbwtgraph::sample_name_set reference_samples_;
  std::vector<std::string> ref_chroms_;
  std::unordered_map<std::uint64_t, RefNodeMapEntry> node_ref_map_;
};

}  // namespace

std::unique_ptr<GraphIndex> make_graph_index(const GraphIndex::Config& config) {
  return std::make_unique<GbwtGraphIndex>(config);
}

}  // namespace pggaf
