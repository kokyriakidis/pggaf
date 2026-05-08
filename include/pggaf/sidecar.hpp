#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pggaf {

struct SidecarHeader {
  std::uint32_t version = 1;
  std::string fingerprint;
  bool used_r_index = false;
};

struct SidecarSetRecord {
  std::uint32_t set_id = 0;
  std::vector<std::uint64_t> thread_ids;
};

class SidecarWriter {
public:
  explicit SidecarWriter(const std::string& path);
  ~SidecarWriter();

  void write_header(const SidecarHeader& header);
  void write_set(const SidecarSetRecord& record);

private:
  std::string path_;
  std::unique_ptr<std::ostream> out_;
};

struct LoadedSidecar {
  SidecarHeader header;
  std::unordered_map<std::uint32_t, std::vector<std::uint64_t>> sets;
};

LoadedSidecar read_sidecar(const std::string& path);

}  // namespace pggaf
