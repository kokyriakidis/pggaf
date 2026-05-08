#include "pggaf/pggaf.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <htslib/bgzf.h>
#include <htslib/tbx.h>

#include "pggaf/error.hpp"

namespace pggaf {
namespace {

// Extract rc/rb/re from optional tag fields after the 12 mandatory GAF columns.
// rb/re are 0-based half-open; returns false if any tag is absent.
bool extract_ref_coords(const std::string& line,
                        std::string& rc, std::int64_t& rb, std::int64_t& re) {
  const char* p = line.c_str();
  int tabs = 0;
  while (*p && tabs < 12) {
    if (*p++ == '\t') ++tabs;
  }
  if (tabs < 12) return false;

  bool has_rc = false, has_rb = false, has_re = false;
  while (*p) {
    const char* s = p;
    while (*p && *p != '\t') ++p;
    const std::size_t len = static_cast<std::size_t>(p - s);

    if (len > 5 && s[2] == ':' && s[4] == ':') {
      if (s[0] == 'r' && s[1] == 'c' && s[3] == 'Z') {
        rc.assign(s + 5, len - 5);
        has_rc = true;
      } else if (s[0] == 'r' && s[1] == 'b' && s[3] == 'i') {
        rb = std::strtoll(s + 5, nullptr, 10);
        has_rb = true;
      } else if (s[0] == 'r' && s[1] == 'e' && s[3] == 'i') {
        re = std::strtoll(s + 5, nullptr, 10);
        has_re = true;
      }
    }

    if (has_rc && has_rb && has_re) break;
    if (*p == '\t') ++p;
  }

  return has_rc && has_rb && has_re;
}

void wait_child(pid_t pid, const char* label) {
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    throw Error(std::string(label) + " exited with code " + std::to_string(code));
  }
}

}  // namespace

// Pipeline: main thread writes decorated lines → sort (fork/exec) → bgzf thread (htslib)
// Then: tbx_index_build (htslib).
//
// Each output line is prepended with three 1-based coordinate columns so tabix
// can index them: chrom\t1based_start\t1based_end\t<original GAF line>
// Records without rc/rb/re are skipped.
int run_index_gaf(const IndexGafOptions& options) {
  int pre_sort[2], sort_out[2];
  if (pipe(pre_sort) < 0 || pipe(sort_out) < 0)
    throw Error("pipe() failed in index-gaf");

  const pid_t sort_pid = fork();
  if (sort_pid < 0) {
    close(pre_sort[0]); close(pre_sort[1]);
    close(sort_out[0]); close(sort_out[1]);
    throw Error("fork failed for sort in index-gaf");
  }
  if (sort_pid == 0) {
    dup2(pre_sort[0], STDIN_FILENO);
    dup2(sort_out[1], STDOUT_FILENO);
    close(pre_sort[0]); close(pre_sort[1]);
    close(sort_out[0]); close(sort_out[1]);
    setenv("LC_ALL", "C", 1);
    execlp("sort", "sort", "-k1,1", "-k2,2n", "-k3,3n",
           static_cast<char*>(nullptr));
    _exit(127);
  }

  // Parent keeps only the write end of the input pipe and read end of the output pipe.
  close(pre_sort[0]);
  close(sort_out[1]);

  // BGZF writer thread: reads sort's output and writes it to the bgzip file.
  // Runs concurrently with the main thread feeding sort, preventing pipe-buffer
  // deadlock when sort's output grows large.
  std::string bgzf_error;
  std::thread bgzf_thread([&]() {
    BGZF* fp = bgzf_open(options.out_path.c_str(), "w");
    if (!fp) {
      bgzf_error = "cannot open output file for index-gaf: " + options.out_path;
      close(sort_out[0]);
      return;
    }
    char buf[65536];
    bool write_error = false;
    ssize_t n;
    while ((n = read(sort_out[0], buf, sizeof(buf))) > 0) {
      if (!write_error && bgzf_write(fp, buf, static_cast<std::size_t>(n)) < 0)
        write_error = true;
    }
    close(sort_out[0]);
    if (bgzf_close(fp) < 0 || write_error)
      bgzf_error = "failed to write bgzip output in index-gaf";
  });

  // Main thread: stream the input GAF, prepend coordinate columns, feed sort.
  {
    std::ifstream in(options.in_path);
    if (!in) {
      close(pre_sort[1]);
      bgzf_thread.join();
      waitpid(sort_pid, nullptr, 0);
      throw Error("cannot open input GAF: " + options.in_path);
    }

    FILE* to_sort = fdopen(pre_sort[1], "w");
    if (!to_sort) {
      close(pre_sort[1]);
      bgzf_thread.join();
      waitpid(sort_pid, nullptr, 0);
      throw Error("fdopen failed in index-gaf");
    }

    std::string line, rc;
    std::int64_t rb = 0, re = 0;
    while (std::getline(in, line)) {
      if (line.empty() || line.front() == '#' || line.front() == '@') continue;
      if (!extract_ref_coords(line, rc, rb, re)) continue;
      // Convert 0-based half-open [rb, re) → 1-based closed [rb+1, re] for tabix
      std::fprintf(to_sort, "%s\t%" PRId64 "\t%" PRId64 "\t%s\n",
                   rc.c_str(), rb + 1, re, line.c_str());
    }
    std::fclose(to_sort);  // signals EOF to sort
  }

  bgzf_thread.join();
  wait_child(sort_pid, "index-gaf: sort");

  if (!bgzf_error.empty()) throw Error(bgzf_error);

  static const tbx_conf_t conf = {TBX_GENERIC, 1, 2, 3, '#', 0};
  if (tbx_index_build(options.out_path.c_str(), 0, &conf) != 0)
    throw Error("tabix indexing failed for: " + options.out_path);

  return 0;
}

}  // namespace pggaf
