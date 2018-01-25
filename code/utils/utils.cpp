// Very messy hack to get the defines for different bio operations. Should be
// changed to something more palatable if possible.

#include <endian.h>

#include <cassert>
#include <cstdint>
#include <cstring>

#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "utils.h"
#include "utils_c.h"

namespace fs_testing {
namespace utils {
using std::endl;
using std::ifstream;
using std::ios;
using std::ios_base;
using std::memcpy;
using std::mt19937;
using std::ofstream;
using std::ostream;
using std::pair;
using std::shared_ptr;
using std::tie;
using std::uniform_int_distribution;
using std::vector;

namespace {
  const unsigned int kSerializeBufSize = 4096;
}

bool disk_write::is_async_write() {
  return c_is_async_write(&metadata);
}

bool disk_write::is_barrier() {
  return c_is_barrier(&metadata);
}

bool disk_write::is_meta() {
  return c_is_meta(&metadata);
}

bool disk_write::is_checkpoint() {
  return c_is_checkpoint(&metadata);
}

disk_write::disk_write() {
  // Apparently contained structs aren't set to 0 on default initialization
  // unless their constructors are defined too. But, we have a struct that is
  // shared between C and C++ so we can't define an constructor easily.
  metadata.bi_flags = 0;
  metadata.bi_rw = 0;
  metadata.write_sector = 0;
  metadata.size = 0;
  metadata.time_ns = 0;
  data.reset();
}

disk_write::disk_write(const struct disk_write_op_meta& m,
    const char *d) {
  metadata = m;
  if (metadata.size > 0 && d != NULL) {
    data.reset(new char[metadata.size], [](char* c) {delete[] c;});
    memcpy(data.get(), d, metadata.size);
  }
}

bool operator==(const disk_write& a, const disk_write& b) {
  if (tie(a.metadata.bi_flags, a.metadata.bi_rw, a.metadata.write_sector,
        a.metadata.size) ==
      tie(b.metadata.bi_flags, b.metadata.bi_rw, b.metadata.write_sector,
        b.metadata.size)) {
    if ((a.data.get() == NULL && b.data.get() != NULL) ||
        (a.data.get() != NULL && b.data.get() == NULL)) {
      return false;
    } else if (a.data.get() == NULL && b.data.get() == NULL) {
      return true;
    }
    if (memcmp(a.data.get(), b.data.get(), a.metadata.size) == 0) {
      return true;
    }
  }
  return false;
}

bool operator!=(const disk_write& a, const disk_write& b) {
  return !(a == b);
}

// Assumes binary file stream provided.
void disk_write::serialize(std::ofstream& fs, const disk_write& dw) {
  char buffer[kSerializeBufSize];
  memset(buffer, 0, kSerializeBufSize);
  // Working all in big endian...

  // Write out the metadata for this log entry.
  unsigned int buf_offset = 0;
  const uint64_t write_flags = htobe64(dw.metadata.bi_flags);
  const uint64_t write_rw = htobe64(dw.metadata.bi_rw);
  const uint64_t write_write_sector = htobe64(dw.metadata.write_sector);
  const uint64_t write_size = htobe64(dw.metadata.size);
  const uint64_t time_ns = htobe64(dw.metadata.time_ns);
  memcpy(buffer + buf_offset, &write_flags, sizeof(const uint64_t));
  buf_offset += sizeof(const uint64_t);
  memcpy(buffer + buf_offset, &write_rw, sizeof(const uint64_t));
  buf_offset += sizeof(const uint64_t);
  memcpy(buffer + buf_offset, &write_write_sector, sizeof(const uint64_t));
  buf_offset += sizeof(const uint64_t);
  memcpy(buffer + buf_offset, &write_size, sizeof(const uint64_t));
  buf_offset += sizeof(const uint64_t);
  memcpy(buffer + buf_offset, &time_ns, sizeof(const uint64_t));
  buf_offset += sizeof(const uint64_t);

  // Write out the 4K buffer containing metadata for this log entry
  fs.write(buffer, kSerializeBufSize);
  if (!fs.good()) {
    std::cerr << "some error writing to file" << std::endl;
    return;
  }

  // Write out the actual data for this log entry. Data could be larger than
  // buf_size so loop through this.
  const char *data = (char *) dw.data.get();
  for (unsigned int i = 0; i < dw.metadata.size; i += kSerializeBufSize) {
    const unsigned int copy_amount =
      ((i + kSerializeBufSize) > dw.metadata.size)
        ? (dw.metadata.size - i)
        : kSerializeBufSize;
    // Not strictly needed, but it makes it easier.
    memset(buffer, 0, kSerializeBufSize);
    memcpy(buffer, data + i, copy_amount);
    fs.write(buffer, kSerializeBufSize);
    if (!fs.good()) {
      std::cerr << "some error writing to file" << std::endl;
      return;
    }
  }
}

// Assumes binary file stream provided.
disk_write disk_write::deserialize(ifstream& is) {
  char buffer[kSerializeBufSize];
  memset(buffer, 0, kSerializeBufSize);

  // TODO(ashmrtn): Make this read only the size of the required data. This
  // means we should probably slightly restructure some of the structs we use so
  // we can read from the buffer into struct fields.
  is.read(buffer, kSerializeBufSize);
  // check if read was successful
  assert(is);

  unsigned int buf_offset = 0;
  uint64_t write_flags, write_rw, write_write_sector, write_size, time_ns;
  memcpy(&write_flags, buffer + buf_offset, sizeof(uint64_t));
  buf_offset += sizeof(uint64_t);
  memcpy(&write_rw, buffer + buf_offset, sizeof(uint64_t));
  buf_offset += sizeof(uint64_t);
  memcpy(&write_write_sector, buffer + buf_offset, sizeof(uint64_t));
  buf_offset += sizeof(uint64_t);
  memcpy(&write_size, buffer + buf_offset, sizeof(uint64_t));
  buf_offset += sizeof(uint64_t);
  memcpy(&time_ns, buffer + buf_offset, sizeof(uint64_t));
  buf_offset += sizeof(uint64_t);

  disk_write_op_meta meta;

  meta.bi_flags = be64toh(write_flags);
  meta.bi_rw = be64toh(write_rw);
  meta.write_sector = be64toh(write_write_sector);
  meta.size = be64toh(write_size);
  meta.time_ns = be64toh(time_ns);

  char *data = new char[meta.size];
  for (unsigned int i = 0; i < meta.size; i += kSerializeBufSize) {
    const unsigned int read_amount =
      ((i + kSerializeBufSize) > meta.size)
        ? (meta.size - i)
        : kSerializeBufSize;
    is.read(buffer, kSerializeBufSize);
    // check if read was successful
    assert(is);
    memcpy(data + i, buffer, read_amount);
  }

  disk_write res(meta, data);
  delete[] data;
  return res;
}

std::string disk_write::flags_to_string(long long flags) {
  const unsigned int flag_buf_size = 4096;
  char *flag_buf = new char[flag_buf_size];
  flag_buf[0] = '\0';
  c_flags_to_string(flags, flag_buf, flag_buf_size);
  std::string res(flag_buf);
  delete[] flag_buf;
  return res;
}

ostream& operator<<(ostream& os, const disk_write& dw) {
  os << std::dec << std::setw(18) << std::fixed <<
    ((double) dw.metadata.time_ns) / 100000000 <<
    " " << std::setw(18) << std::hex << std::showbase <<
      dw.metadata.write_sector <<
    " " << std::setw(18) << dw.metadata.size << std::endl <<
    '\t' << "flags " << std::setw(18) << dw.metadata.bi_rw << std::noshowbase
      << std::dec << ": " << disk_write::flags_to_string(dw.metadata.bi_rw) <<
      endl;
  return os;
}

bool disk_write::has_write_flag() {
  return c_has_write_flag(&metadata);
}

bool disk_write::has_flush_flag() {
  return c_has_flush_flag(&metadata);
}

bool disk_write::has_flush_seq_flag() {
  return c_has_flush_seq_flag(&metadata);
}

bool disk_write::has_FUA_flag() {
  return c_has_FUA_flag(&metadata);
}

void disk_write::set_flush_flag() {
  c_set_flush_flag(&metadata);
}

void disk_write::set_flush_seq_flag() {
  c_set_flush_seq_flag(&metadata);
}

void disk_write::clear_flush_flag() {
  c_clear_flush_flag(&metadata);
}

void disk_write::clear_flush_seq_flag() {
  c_clear_flush_seq_flag(&metadata);
}

shared_ptr<char> disk_write::set_data(const char *d) {
  if (metadata.size > 0 && d != NULL) {
    data.reset(new char[metadata.size], [](char* c) {delete[] c;});
    memcpy(data.get(), d, metadata.size);
  }
  return data;
}

shared_ptr<char> disk_write::get_data() {
  return data;
}

void disk_write::clear_data() {
  data.reset();
}

}  // namespace utils
}  // namespace fs_testing
