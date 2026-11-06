// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// tt-chd-read: reads byte ranges of the disc inside a CHD image (MAME chdman createdvd), for
// project/tools/disc_image.py, which checks and extracts the two files the build needs.
//
//   tt-chd-read IMAGE.chd
//
// Writes the uncompressed disc size (8 bytes, little endian) to stdout, then answers requests
// read from stdin: each request is an offset and a length (8 bytes each, little endian) and is
// answered with exactly that many bytes. Ends at the end of stdin. Errors go to stderr with a
// non-zero exit. The image is only read.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>

static int read_u64(FILE* in, uint64_t* value) {
  unsigned char bytes[8];
  if (fread(bytes, 1, 8, in) != 8) return 0;
  *value = 0;
  for (int i = 7; i >= 0; --i) *value = (*value << 8) | bytes[i];
  return 1;
}

static void write_u64(FILE* out, uint64_t value) {
  unsigned char bytes[8];
  for (int i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
  fwrite(bytes, 1, 8, out);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: tt-chd-read IMAGE.chd\n");
    return 2;
  }
  chd_file* chd = NULL;
  chd_error error = chd_open(argv[1], CHD_OPEN_READ, NULL, &chd);
  if (error != CHDERR_NONE) {
    fprintf(stderr, "Could not open CHD image: %s\n", chd_error_string(error));
    return 1;
  }
  const chd_header* header = chd_get_header(chd);
  const uint32_t hunk_bytes = header->hunkbytes;
  const uint64_t size = header->logicalbytes;
  unsigned char* hunk_data = malloc(hunk_bytes ? hunk_bytes : 1);
  if (!hunk_bytes || !hunk_data) {
    fprintf(stderr, "Invalid CHD image\n");
    chd_close(chd);
    return 1;
  }
  int64_t cached_hunk = -1;

  write_u64(stdout, size);
  fflush(stdout);
  uint64_t offset, length;
  while (read_u64(stdin, &offset)) {
    if (!read_u64(stdin, &length) || offset > size || length > size - offset) {
      fprintf(stderr, "Disc entry is outside the CHD image\n");
      chd_close(chd);
      return 1;
    }
    while (length) {
      const uint32_t hunk = (uint32_t)(offset / hunk_bytes);
      const uint32_t inside = (uint32_t)(offset % hunk_bytes);
      uint64_t chunk = hunk_bytes - inside;
      if (chunk > length) chunk = length;
      if (cached_hunk != (int64_t)hunk) {
        error = chd_read(chd, hunk, hunk_data);
        if (error != CHDERR_NONE) {
          fprintf(stderr, "CHD read failed: %s\n", chd_error_string(error));
          chd_close(chd);
          return 1;
        }
        cached_hunk = hunk;
      }
      fwrite(hunk_data + inside, 1, (size_t)chunk, stdout);
      offset += chunk;
      length -= chunk;
    }
    fflush(stdout);
  }
  free(hunk_data);
  chd_close(chd);
  return 0;
}
