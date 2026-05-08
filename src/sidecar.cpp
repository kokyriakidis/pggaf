#include "pggaf/sidecar.hpp"

#include <fstream>
#include <memory>

#include "pggaf/error.hpp"

namespace pggaf {
namespace {

constexpr char kMagic[] = {'P', 'G', 'S', '1'};

template <typename T>
void write_binary(std::ostream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void read_binary(std::istream& in, T& value) {
  in.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!in) {
    throw Error("unexpected end of sidecar file");
  }
}

void write_string(std::ostream& out, const std::string& value) {
  const std::uint32_t size = static_cast<std::uint32_t>(value.size());
  write_binary(out, size);
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_string(std::istream& in) {
  std::uint32_t size = 0;
  read_binary(in, size);
  std::string value(size, '\0');
  in.read(value.data(), static_cast<std::streamsize>(value.size()));
  if (!in) {
    throw Error("unexpected end of sidecar string");
  }
  return value;
}

}  // namespace

SidecarWriter::SidecarWriter(const std::string& path) : path_(path), out_(std::make_unique<std::ofstream>(path, std::ios::binary)) {
  if (!(*out_)) {
    throw Error("cannot open sidecar for writing: " + path);
  }
}

SidecarWriter::~SidecarWriter() = default;

void SidecarWriter::write_header(const SidecarHeader& header) {
  out_->write(kMagic, sizeof(kMagic));
  write_binary(*out_, header.version);
  const std::uint8_t used_r_index = header.used_r_index ? 1 : 0;
  write_binary(*out_, used_r_index);
  write_string(*out_, header.fingerprint);
}

void SidecarWriter::write_set(const SidecarSetRecord& record) {
  write_binary(*out_, record.set_id);
  const std::uint32_t count = static_cast<std::uint32_t>(record.thread_ids.size());
  write_binary(*out_, count);
  for (std::uint64_t thread_id : record.thread_ids) {
    write_binary(*out_, thread_id);
  }
}

LoadedSidecar read_sidecar(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw Error("cannot open sidecar for reading: " + path);
  }

  char magic[sizeof(kMagic)] = {};
  in.read(magic, sizeof(magic));
  if (!in || std::string_view(magic, sizeof(kMagic)) != std::string_view(kMagic, sizeof(kMagic))) {
    throw Error("invalid sidecar magic in " + path);
  }

  LoadedSidecar loaded;
  read_binary(in, loaded.header.version);
  if (loaded.header.version != 1) {
    throw Error("unsupported sidecar version in " + path);
  }
  std::uint8_t used_r_index = 0;
  read_binary(in, used_r_index);
  loaded.header.used_r_index = (used_r_index != 0);
  loaded.header.fingerprint = read_string(in);

  while (in.peek() != std::char_traits<char>::eof()) {
    SidecarSetRecord record;
    read_binary(in, record.set_id);
    std::uint32_t count = 0;
    read_binary(in, count);
    record.thread_ids.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      read_binary(in, record.thread_ids[index]);
    }
    const auto inserted = loaded.sets.emplace(record.set_id, std::move(record.thread_ids));
    if (!inserted.second) {
      throw Error("duplicate set id in sidecar: " + std::to_string(record.set_id));
    }
  }

  return loaded;
}

}  // namespace pggaf
