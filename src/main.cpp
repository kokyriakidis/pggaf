#include <exception>
#include <iostream>

#include "pggaf/cli.hpp"
#include "pggaf/pggaf.hpp"

int main(int argc, char** argv) {
  try {
    const pggaf::CommandLine command_line = pggaf::parse_command_line(argc, argv);
    switch (command_line.command) {
      case pggaf::CommandLine::Command::AnnotateGaf:
        return pggaf::run_annotate_gaf(command_line.annotate_gaf);
      case pggaf::CommandLine::Command::Decode:
        return pggaf::run_decode(command_line.decode);
      case pggaf::CommandLine::Command::IndexGaf:
        return pggaf::run_index_gaf(command_line.index_gaf);
    }
  } catch (const std::exception& error) {
    std::cerr << "pggaf: " << error.what() << '\n';
  }

  return 1;
}
