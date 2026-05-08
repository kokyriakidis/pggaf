#pragma once

#include <cstddef>
#include <string>

namespace pggaf {

struct AnnotateGafOptions {
  std::string gaf_path;
  std::string gbz_path;
  std::string r_index_path;
  std::string ref_sample;
  std::string out_gaf_path;
  std::string out_sets_path;
  bool primary_only = false;

  bool use_r_index() const { return !r_index_path.empty(); }
};

struct DecodeOptions {
  std::string sets_path;
  std::string gbz_path;
  std::string out_path;
};

struct IndexGafOptions {
  std::string in_path;
  std::string out_path;
};

struct CommandLine {
  enum class Command { AnnotateGaf, Decode, IndexGaf };

  Command command = Command::AnnotateGaf;
  AnnotateGafOptions annotate_gaf;
  DecodeOptions decode;
  IndexGafOptions index_gaf;
};

}  // namespace pggaf
