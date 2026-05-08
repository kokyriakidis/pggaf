#include "pggaf/cli.hpp"

#include <charconv>
#include <limits>
#include <string>
#include <string_view>

#include "pggaf/error.hpp"

namespace pggaf {
namespace {

std::string require_value(int& index, int argc, char** argv, std::string_view option) {
  if (index + 1 >= argc) {
    throw Error("missing value for option " + std::string(option));
  }
  ++index;
  return argv[index];
}

void validate_annotate_gaf(const AnnotateGafOptions& options) {
  if (options.gaf_path.empty()) {
    throw Error("--gaf is required");
  }
  if (options.out_gaf_path.empty()) {
    throw Error("--out-gaf is required");
  }
  if (options.out_sets_path.empty()) {
    throw Error("--out-sets is required");
  }
  if (options.gbz_path.empty()) {
    throw Error("--gbz is required");
  }
}

void validate_decode(const DecodeOptions& options) {
  if (options.sets_path.empty()) {
    throw Error("--sets is required");
  }
  if (options.out_path.empty()) {
    throw Error("--out is required");
  }
  if (options.gbz_path.empty()) {
    throw Error("--gbz is required");
  }
}

void validate_index_gaf(const IndexGafOptions& options) {
  if (options.in_path.empty()) {
    throw Error("--in is required");
  }
  if (options.out_path.empty()) {
    throw Error("--out is required");
  }
}

}  // namespace

CommandLine parse_command_line(int argc, char** argv) {
  if (argc < 2) {
    throw Error("expected a subcommand: annotate-gaf, decode, or index-gaf");
  }

  CommandLine result;
  const std::string command = argv[1];

  if (command == "annotate-gaf") {
    result.command = CommandLine::Command::AnnotateGaf;
    for (int index = 2; index < argc; ++index) {
      const std::string_view option = argv[index];
      if (option == "--gaf") {
        result.annotate_gaf.gaf_path = require_value(index, argc, argv, option);
      } else if (option == "--gbz") {
        result.annotate_gaf.gbz_path = require_value(index, argc, argv, option);
      } else if (option == "--r-index") {
        result.annotate_gaf.r_index_path = require_value(index, argc, argv, option);
      } else if (option == "--ref-sample") {
        result.annotate_gaf.ref_sample = require_value(index, argc, argv, option);
      } else if (option == "--out-gaf") {
        result.annotate_gaf.out_gaf_path = require_value(index, argc, argv, option);
      } else if (option == "--out-sets") {
        result.annotate_gaf.out_sets_path = require_value(index, argc, argv, option);
      } else if (option == "--primary-only") {
        result.annotate_gaf.primary_only = true;
      } else {
        throw Error("unknown annotate-gaf option: " + std::string(option));
      }
    }
    validate_annotate_gaf(result.annotate_gaf);
    return result;
  }

  if (command == "decode") {
    result.command = CommandLine::Command::Decode;
    for (int index = 2; index < argc; ++index) {
      const std::string_view option = argv[index];
      if (option == "--sets") {
        result.decode.sets_path = require_value(index, argc, argv, option);
      } else if (option == "--gbz") {
        result.decode.gbz_path = require_value(index, argc, argv, option);
      } else if (option == "--out") {
        result.decode.out_path = require_value(index, argc, argv, option);
      } else {
        throw Error("unknown decode option: " + std::string(option));
      }
    }
    validate_decode(result.decode);
    return result;
  }

  if (command == "index-gaf") {
    result.command = CommandLine::Command::IndexGaf;
    for (int index = 2; index < argc; ++index) {
      const std::string_view option = argv[index];
      if (option == "--in") {
        result.index_gaf.in_path = require_value(index, argc, argv, option);
      } else if (option == "--out") {
        result.index_gaf.out_path = require_value(index, argc, argv, option);
      } else {
        throw Error("unknown index-gaf option: " + std::string(option));
      }
    }
    validate_index_gaf(result.index_gaf);
    return result;
  }

  throw Error("unknown subcommand: " + command);
}

}  // namespace pggaf
