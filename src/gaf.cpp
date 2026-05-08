#include "pggaf/gaf.hpp"

#include <charconv>
#include <string_view>

#include "pggaf/error.hpp"

namespace pggaf {

bool qname_less(std::string_view a, std::string_view b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit(static_cast<unsigned char>(a[i])) &&
        std::isdigit(static_cast<unsigned char>(b[j]))) {
      while (i < a.size() && a[i] == '0') ++i;
      while (j < b.size() && b[j] == '0') ++j;
      std::size_t ia = i, jb = j;
      while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) ++ia;
      while (jb < b.size() && std::isdigit(static_cast<unsigned char>(b[jb]))) ++jb;
      std::size_t alen = ia - i, blen = jb - j;
      if (alen != blen) return alen < blen;
      while (i < ia) {
        if (a[i] != b[j]) return a[i] < b[j];
        ++i; ++j;
      }
      i = ia; j = jb;
    } else {
      if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
      ++i; ++j;
    }
  }
  return i == a.size() && j < b.size();
}

namespace {

std::uint64_t parse_u64(std::string_view value, std::string_view field_name) {
  std::uint64_t result = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto parse_result = std::from_chars(begin, end, result);
  if (parse_result.ec != std::errc{} || parse_result.ptr != end) {
    throw Error("cannot parse GAF field " + std::string(field_name) + " from value '" + std::string(value) + "'");
  }
  return result;
}

std::vector<std::string_view> split_tab_fields(const std::string& line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.emplace_back(line.data() + start, line.size() - start);
      break;
    }
    fields.emplace_back(line.data() + start, tab - start);
    start = tab + 1;
  }
  return fields;
}

}  // namespace

std::vector<OrientedNode> parse_target_walk(std::string_view walk) {
  std::vector<OrientedNode> result;
  std::size_t index = 0;
  while (index < walk.size()) {
    const char orientation = walk[index];
    if (orientation != '>' && orientation != '<') {
      throw Error("invalid target walk orientation in '" + std::string(walk) + "'");
    }
    ++index;
    const std::size_t start = index;
    while (index < walk.size() && walk[index] != '>' && walk[index] != '<') {
      ++index;
    }
    if (start == index) {
      throw Error("missing node id in target walk '" + std::string(walk) + "'");
    }
    const std::string_view token(walk.data() + start, index - start);
    result.push_back(OrientedNode{parse_u64(token, "target_walk_node"), orientation == '<'});
  }
  return result;
}

std::optional<GafRecord> parse_gaf_line(const std::string& line) {
  if (line.empty() || line.front() == '#' || line.front() == '@') {
    return std::nullopt;
  }

  const std::vector<std::string_view> fields = split_tab_fields(line);
  if (fields.size() < 12) {
    throw Error("expected at least 12 fields in GAF line: " + line);
  }

  GafRecord record;
  record.qname = std::string(fields[0]);
  record.query_length = parse_u64(fields[1], "query_length");
  record.query_start = parse_u64(fields[2], "query_start");
  record.query_end = parse_u64(fields[3], "query_end");
  if (fields[4].size() == 1 && fields[4][0] == '*') {
    return std::nullopt;
  }
  if (fields[4].size() != 1 || (fields[4][0] != '+' && fields[4][0] != '-')) {
    throw Error("invalid GAF strand field in line: " + line);
  }
  record.strand = fields[4][0];
  record.nodes = parse_target_walk(fields[5]);
  record.target_length = parse_u64(fields[6], "target_length");
  record.target_start = parse_u64(fields[7], "target_start");
  record.target_end = parse_u64(fields[8], "target_end");
  record.residue_matches = parse_u64(fields[9], "residue_matches");
  record.block_length = parse_u64(fields[10], "block_length");
  record.mapq = static_cast<std::uint32_t>(parse_u64(fields[11], "mapq"));

  return record;
}

std::vector<GafRecord> read_gaf_records(std::istream& in) {
  std::vector<GafRecord> records;
  std::string line;
  while (std::getline(in, line)) {
    if (auto record = parse_gaf_line(line)) {
      records.push_back(std::move(*record));
    }
  }
  return records;
}

}  // namespace pggaf
