#include <cassert>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <gbwt/dynamic_gbwt.h>
#include <gbwtgraph/gbz.h>
#include <gbwtgraph/naive_graph.h>
#include <gbwtgraph/utils.h>
#include <sdsl/bits.hpp>

#include "pggaf/cli.hpp"
#include "pggaf/error.hpp"
#include "pggaf/gaf.hpp"
#include "pggaf/gbwt_backend.hpp"
#include "pggaf/pggaf.hpp"
#include "pggaf/sidecar.hpp"

namespace {

gbwt::GBWT build_test_gbwt(const std::vector<gbwt::vector_type>& paths) {
  gbwt::size_type node_width = 1;
  gbwt::size_type total_length = 0;
  for (const gbwt::vector_type& path : paths) {
    for (gbwt::node_type node : path) {
      node_width = std::max(node_width, gbwt::size_type(sdsl::bits::length(gbwt::Node::encode(gbwt::Node::id(node), true))));
    }
    total_length += 2 * (path.size() + 1);
  }

  gbwt::Verbosity::set(gbwt::Verbosity::SILENT);
  gbwt::GBWTBuilder builder(node_width, total_length);
  for (const gbwt::vector_type& path : paths) {
    builder.insert(path, true);
  }
  builder.finish();
  return gbwt::GBWT(builder.index);
}

void add_path_edges(gbwtgraph::NaiveGraph& graph, const gbwt::vector_type& path) {
  for (std::size_t i = 1; i < path.size(); ++i) {
    graph.create_edge(path[i - 1], path[i]);
  }
}

gbwt::vector_type::value_type test_node(gbwt::size_type id, bool reverse = false) {
  return static_cast<gbwt::vector_type::value_type>(gbwt::Node::encode(id, reverse));
}

std::filesystem::path write_reference_test_gbz() {
  const gbwt::vector_type chr1{
    test_node(1),
    test_node(2),
    test_node(3),
  };
  const gbwt::vector_type chr2{
    test_node(4),
    test_node(2),
    test_node(5),
  };

  gbwt::GBWT index = build_test_gbwt({chr1, chr2});
  index.addMetadata();
  index.metadata.setSamples({"REF"});
  index.metadata.setContigs({"chr1", "chr2"});
  index.metadata.addPath(0, 0, 0, 0);
  index.metadata.addPath(0, 1, 0, 0);
  index.metadata.setHaplotypes(2);
  index.tags.set(gbwtgraph::REFERENCE_SAMPLE_LIST_GBWT_TAG, "REF");

  gbwtgraph::NaiveGraph graph;
  graph.create_node(1, "AA");
  graph.create_node(2, "CCC");
  graph.create_node(3, "GG");
  graph.create_node(4, "TT");
  graph.create_node(5, "AAAA");
  add_path_edges(graph, chr1);
  add_path_edges(graph, chr2);
  graph.remove_duplicate_edges();

  const std::filesystem::path path = std::filesystem::temp_directory_path() / "pggaf_test_reference_intervals.gbz";
  {
    gbwtgraph::GBZ gbz(index, graph);
    std::ofstream out(path, std::ios::binary);
    gbz.simple_sds_serialize(out);
  }
  return path;
}

void assert_interval(const std::optional<pggaf::GraphIndex::RefInterval>& interval,
                     const std::string& chrom,
                     std::uint64_t start,
                     std::uint64_t end) {
  assert(interval.has_value());
  assert(interval->chrom == chrom);
  assert(interval->start == start);
  assert(interval->end == end);
}

}  // namespace

int main() {
  static_assert(sizeof(pggaf::OrientedNode) == sizeof(std::uint64_t));

  {
    const auto nodes = pggaf::parse_target_walk(">2>3<4");
    const pggaf::OrientedNode first{2, false};
    const pggaf::OrientedNode second{3, false};
    const pggaf::OrientedNode third{4, true};
    assert(nodes.size() == 3);
    assert(nodes[0] == first);
    assert(nodes[1] == second);
    assert(nodes[2] == third);
  }

  {
    const auto parsed = pggaf::parse_gaf_line("read1\t6\t0\t6\t+\t>2>3>4\t12\t2\t8\t6\t6\t60\tcs:Z::6");
    const pggaf::OrientedNode first{2, false};
    const pggaf::OrientedNode last{4, false};
    assert(parsed.has_value());
    assert(parsed->qname == "read1");
    assert(parsed->query_length == 6);
    assert(parsed->nodes.size() == 3);
    assert(parsed->nodes[0] == first);
    assert(parsed->nodes[2] == last);
  }

  {
    bool threw = false;
    try {
      (void) pggaf::parse_target_walk(">2>");
    } catch (const pggaf::Error&) {
      threw = true;
    }
    assert(threw);
  }

  {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "pggaf_test_sidecar.pgs";
    {
      pggaf::SidecarWriter writer(path.string());
      writer.write_header(pggaf::SidecarHeader{1, "fingerprint", true});
      writer.write_set(pggaf::SidecarSetRecord{7, {1, 2, 3}});
    }

    const pggaf::LoadedSidecar loaded = pggaf::read_sidecar(path.string());
    assert(loaded.header.version == 1);
    assert(loaded.header.fingerprint == "fingerprint");
    assert(loaded.header.used_r_index);
    assert(loaded.sets.at(7) == std::vector<std::uint64_t>({1, 2, 3}));
    std::filesystem::remove(path);
  }

  {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "pggaf_test_sidecar_duplicate.pgs";
    {
      std::ofstream out(path, std::ios::binary);
      const char magic[] = {'P', 'G', 'S', '1'};
      const std::uint32_t version = 1;
      const std::uint8_t used_r_index = 0;
      const std::uint32_t fingerprint_size = 2;
      const char fingerprint[] = {'o', 'k'};
      const std::uint32_t set_id = 4;
      const std::uint32_t count = 1;
      const std::uint64_t thread = 9;
      out.write(magic, sizeof(magic));
      out.write(reinterpret_cast<const char*>(&version), sizeof(version));
      out.write(reinterpret_cast<const char*>(&used_r_index), sizeof(used_r_index));
      out.write(reinterpret_cast<const char*>(&fingerprint_size), sizeof(fingerprint_size));
      out.write(fingerprint, sizeof(fingerprint));
      out.write(reinterpret_cast<const char*>(&set_id), sizeof(set_id));
      out.write(reinterpret_cast<const char*>(&count), sizeof(count));
      out.write(reinterpret_cast<const char*>(&thread), sizeof(thread));
      out.write(reinterpret_cast<const char*>(&set_id), sizeof(set_id));
      out.write(reinterpret_cast<const char*>(&count), sizeof(count));
      out.write(reinterpret_cast<const char*>(&thread), sizeof(thread));
    }

    bool threw = false;
    try {
      (void) pggaf::read_sidecar(path.string());
    } catch (const pggaf::Error&) {
      threw = true;
    }
    assert(threw);
    std::filesystem::remove(path);
  }

  {
    const std::filesystem::path path = write_reference_test_gbz();
    const std::unique_ptr<pggaf::GraphIndex> graph = pggaf::make_graph_index(pggaf::GraphIndex::Config{
      path.string(),
      "",
      true,
      "REF",
    });

    assert_interval(graph->walk_ref_interval({{1, false}, {2, false}, {3, false}}, 1, 6), "chr1", 1, 6);
    assert_interval(graph->walk_ref_interval({{3, true}, {2, true}, {1, true}}, 1, 6), "chr1", 1, 6);
    assert_interval(graph->walk_ref_interval({{2, false}, {5, false}}, 0, 7), "chr2", 2, 9);
    assert(!graph->walk_ref_interval({{2, false}}, 0, 3).has_value());

    const std::filesystem::path gaf_path = std::filesystem::temp_directory_path() / "pggaf_test_reference_intervals.gaf";
    const std::filesystem::path out_gaf_path = std::filesystem::temp_directory_path() / "pggaf_test_reference_intervals.out.gaf";
    const std::filesystem::path sets_path = std::filesystem::temp_directory_path() / "pggaf_test_reference_intervals.pgs";
    {
      std::ofstream gaf(gaf_path);
      gaf << "read1\t7\t0\t7\t+\t>1>2>3\t7\t1\t6\t5\t5\t60\n";
    }

    pggaf::AnnotateGafOptions options;
    options.gaf_path  = gaf_path.string();
    options.gbz_path  = path.string();
    options.ref_sample = "REF";
    options.out_gaf_path = out_gaf_path.string();
    options.out_sets_path = sets_path.string();
    assert(pggaf::run_annotate_gaf(options) == 0);

    {
      std::ifstream annotated(out_gaf_path);
      std::string line;
      std::getline(annotated, line);
      assert(line.find("\trc:Z:chr1") != std::string::npos);
      assert(line.find("\trb:i:1") != std::string::npos);
      assert(line.find("\tre:i:6") != std::string::npos);
    }

    std::filesystem::remove(gaf_path);
    std::filesystem::remove(out_gaf_path);
    std::filesystem::remove(sets_path);
    std::filesystem::remove(path);
  }

  {
    std::vector<std::string> args = {
      "pggaf",
      "annotate-gaf",
      "--gaf", "in.gaf",
      "--gbz", "graph.gbz",
      "--out-gaf", "out.gaf",
      "--out-sets", "out.pgs",
      "--ref-sample", "CHM13",
      "--primary-only",
    };
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }

    const pggaf::CommandLine command = pggaf::parse_command_line(static_cast<int>(argv.size()), argv.data());
    assert(command.command == pggaf::CommandLine::Command::AnnotateGaf);
    assert(command.annotate_gaf.gbz_path == "graph.gbz");
    assert(command.annotate_gaf.out_gaf_path == "out.gaf");
    assert(command.annotate_gaf.ref_sample == "CHM13");
    assert(command.annotate_gaf.primary_only);
  }

  {
    // missing --out-gaf must throw
    std::vector<std::string> args = {
      "pggaf",
      "annotate-gaf",
      "--gaf", "in.gaf",
      "--gbz", "graph.gbz",
      "--out-sets", "out.pgs",
    };
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }

    bool threw = false;
    try {
      (void) pggaf::parse_command_line(static_cast<int>(argv.size()), argv.data());
    } catch (const pggaf::Error&) {
      threw = true;
    }
    assert(threw);
  }

  {
    // unknown option must throw
    std::vector<std::string> args = {
      "pggaf",
      "annotate-gaf",
      "--gaf", "in.gaf",
      "--gbz", "graph.gbz",
      "--out-gaf", "out.gaf",
      "--out-sets", "out.pgs",
      "--invalid-option",
    };
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }

    bool threw = false;
    try {
      (void) pggaf::parse_command_line(static_cast<int>(argv.size()), argv.data());
    } catch (const pggaf::Error&) {
      threw = true;
    }
    assert(threw);
  }

  // qname_less: basic ordering
  {
    assert(pggaf::qname_less("read/1/ccs", "read/2/ccs"));
    assert(pggaf::qname_less("read/9/ccs", "read/10/ccs"));
    assert(!pggaf::qname_less("read/10/ccs", "read/9/ccs"));
    assert(!pggaf::qname_less("read/1/ccs", "read/1/ccs"));
    assert(pggaf::qname_less("a", "b"));
    assert(!pggaf::qname_less("b", "a"));
  }

  // qname_less: the exact digit-length collision from HG002 HiFi data.
  {
    const std::string short_zmw = "m84031_231217_034919_s2/10027098/ccs";
    const std::string long_zmw_a = "m84031_231217_034919_s2/100270982/ccs";
    const std::string long_zmw_b = "m84031_231217_034919_s2/100270986/ccs";
    const std::string long_zmw_c = "m84031_231217_034919_s2/100270992/ccs";

    assert(pggaf::qname_less(short_zmw, long_zmw_a));
    assert(pggaf::qname_less(short_zmw, long_zmw_b));
    assert(pggaf::qname_less(short_zmw, long_zmw_c));
    assert(!pggaf::qname_less(long_zmw_a, short_zmw));
    assert(!pggaf::qname_less(long_zmw_b, short_zmw));
    assert(!pggaf::qname_less(long_zmw_c, short_zmw));
    assert(pggaf::qname_less(long_zmw_a, long_zmw_b));
    assert(pggaf::qname_less(long_zmw_b, long_zmw_c));
  }

  return 0;
}
