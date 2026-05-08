#include "pggaf/pggaf.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <vector>

#include "pggaf/error.hpp"
#include "pggaf/fingerprint.hpp"
#include "pggaf/gbwt_backend.hpp"
#include "pggaf/sidecar.hpp"

namespace pggaf {

int run_decode(const DecodeOptions& options) {
  const LoadedSidecar sidecar = read_sidecar(options.sets_path);
  const std::string fingerprint = sha256_file(options.gbz_path);
  if (sidecar.header.fingerprint != fingerprint) {
    throw Error("sidecar fingerprint does not match the provided GBWT/GBZ");
  }

  const std::unique_ptr<GraphIndex> graph = make_graph_index(GraphIndex::Config{
    options.gbz_path,
  });

  std::ofstream output(options.out_path);
  if (!output) {
    throw Error("cannot open decode output file: " + options.out_path);
  }

  output << "set_id\tthread_id\tpath_id\tsample\thaplotype\tlocus\tpath_name\n";

  std::vector<std::uint32_t> set_ids;
  set_ids.reserve(sidecar.sets.size());
  for (const auto& [set_id, _] : sidecar.sets) {
    set_ids.push_back(set_id);
  }
  std::sort(set_ids.begin(), set_ids.end());

  for (std::uint32_t set_id : set_ids) {
    const auto found = sidecar.sets.find(set_id);
    if (found == sidecar.sets.end()) {
      continue;
    }
    for (std::uint64_t thread_id : found->second) {
      const GraphIndex::ThreadMetadata metadata = graph->decode_thread(thread_id);
      output << set_id << '\t'
             << metadata.thread_id << '\t'
             << metadata.path_id << '\t'
             << metadata.sample << '\t';
      if (metadata.haplotype_known) {
        output << metadata.haplotype;
      }
      output << '\t'
             << metadata.locus << '\t'
             << metadata.path_name << '\n';
    }
  }

  return 0;
}

}  // namespace pggaf
