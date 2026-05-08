#pragma once

#include "pggaf/options.hpp"

namespace pggaf {

int run_annotate_gaf(const AnnotateGafOptions& options);
int run_decode(const DecodeOptions& options);
int run_index_gaf(const IndexGafOptions& options);

}  // namespace pggaf
