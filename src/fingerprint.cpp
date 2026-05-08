#include "pggaf/fingerprint.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <openssl/evp.h>

#include "pggaf/error.hpp"

namespace pggaf {

std::string sha256_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error("cannot open file for hashing: " + path);
  }

  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw Error("failed to allocate OpenSSL digest context");
  }

  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(context);
    throw Error("failed to initialize SHA-256 digest");
  }

  std::vector<char> buffer(1 << 16);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() > 0) {
      if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(input.gcount())) != 1) {
        EVP_MD_CTX_free(context);
        throw Error("failed to update SHA-256 digest");
      }
    }
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1) {
    EVP_MD_CTX_free(context);
    throw Error("failed to finalize SHA-256 digest");
  }
  EVP_MD_CTX_free(context);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < digest_length; ++index) {
    const unsigned char byte = digest[index];
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

}  // namespace pggaf
