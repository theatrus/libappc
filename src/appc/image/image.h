// Copyright 2015 Charles D. Aylward
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// A (possibly updated) copy of of this software is available at
// https://github.com/cdaylward/libappc

#pragma once

#include <archive.h>
#include <archive_entry.h>

#include "3rdparty/cdaylward/pathname.h"
#include "appc/util/status.h"
#include "appc/util/try.h"


namespace appc {
namespace image {


using FileList = std::vector<std::string>;

const std::string manifest_filename{"manifest"};
const std::string rootfs_filename{"rootfs"};


// Work in progress, experimental, no API here yet.


static int copy_data(struct archive* in, struct archive* out) {
  const void* buff;
  size_t size;
  off_t offset;

  for (;;) {
    int r = archive_read_data_block(in, &buff, &size, &offset);
    if (r == ARCHIVE_EOF) return ARCHIVE_OK;
    if (r < ARCHIVE_OK) return r;
    r = archive_write_data_block(out, buff, size, offset);
    if (r < ARCHIVE_OK) return r;
  }
}


static Try<std::string> read_data_into_string(struct archive* in) {
  const void* buff;
  size_t size;
  off_t offset;

  std::string result{};
  for (;;) {
    int r = archive_read_data_block(in, &buff, &size, &offset);
    if (r == ARCHIVE_EOF) break;
    if (r < ARCHIVE_OK) return Failure<std::string>(archive_error_string(in));
    result.append(static_cast<const char*>(buff), size);
  }
  return Result(result);
}


class Image {
private:
  static std::string trim_dot_slash(const std::string& path) {
    return path.length() > 2 && path.compare(0, 2, "./") == 0 ? path.substr(2) : path;
  }

public:
  const std::string filename;

  explicit Image(const std::string& filename)
  : filename(filename) {}

  // List files in the rootfs
  Try<FileList> file_list() {
    std::unique_ptr<struct archive, decltype(&archive_read_free)> archive{
        archive_read_new(), archive_read_free};
    // TODO restrict this to ACI spec
    archive_read_support_filter_all(archive.get());
    archive_read_support_format_all(archive.get());

    if (archive_read_open_filename(archive.get(), filename.c_str(), 10240) != ARCHIVE_OK) {
      return Failure<FileList>(archive_error_string(archive.get()));
    }

    FileList file_list{};
    {
      struct archive_entry* entry;
      while (archive_read_next_header(archive.get(), &entry) == ARCHIVE_OK) {
        const std::string path = trim_dot_slash(archive_entry_pathname(entry));
        if (path.length() > rootfs_filename.length() &&
            path.compare(0, rootfs_filename.length(), rootfs_filename) == 0) {
          file_list.push_back(path.substr(rootfs_filename.length()));
        }
        archive_read_data_skip(archive.get());
      }
    }

    return Result(file_list);
  }

  // Check for valid ACI structure
  Status validate_structure() {
    std::unique_ptr<struct archive, decltype(&archive_read_free)> archive{
        archive_read_new(), archive_read_free};
    // TODO restrict
    archive_read_support_filter_all(archive.get());
    archive_read_support_format_all(archive.get());

    if (archive_read_open_filename(archive.get(), filename.c_str(), 10240) != ARCHIVE_OK) {
      return Invalid(archive_error_string(archive.get()));
    }
    // TODO requires at least one rootfs entry?
    {
      unsigned int manifest_count = 0;
      struct archive_entry* entry;
      while (archive_read_next_header(archive.get(), &entry) == ARCHIVE_OK) {
        const std::string path = trim_dot_slash(archive_entry_pathname(entry));
        const mode_t entry_mode = archive_entry_filetype(entry);
        // TODO fixup
        if (path == manifest_filename) {
          manifest_count++;
          if (manifest_count > 1) return Invalid("Multiple manifest dentries present.");
          if (!(entry_mode & AE_IFREG)) return Invalid("manifest is not a regular file");
        }
        else if (path == rootfs_filename) {
          if (!(entry_mode & AE_IFDIR)) return Invalid("rootfs is not a directory");
        }
        else if (path.length() <= rootfs_filename.length() ||
                 path.compare(0, rootfs_filename.length(), rootfs_filename) != 0) {
          return Invalid(path + " is not under rootfs.");
        }
        // TODO check for foul beasts like ..
        archive_read_data_skip(archive.get());
      }
    }

    return Valid();
  }

  // Return the manifest as a string
  Try<std::string> manifest() {
    std::unique_ptr<struct archive, decltype(&archive_read_free)> archive{
        archive_read_new(), archive_read_free};
    // TODO restrict
    archive_read_support_filter_all(archive.get());
    archive_read_support_format_all(archive.get());

    if (archive_read_open_filename(archive.get(), filename.c_str(), 10240) != ARCHIVE_OK) {
      return Failure<std::string>(archive_error_string(archive.get()));
    }

    {
      struct archive_entry* entry;
      while (archive_read_next_header(archive.get(), &entry) == ARCHIVE_OK) {
        const std::string path = trim_dot_slash(archive_entry_pathname(entry));
        const mode_t entry_mode = archive_entry_filetype(entry);
        if (path == manifest_filename) {
          if (!(entry_mode & AE_IFREG)) {
            return Failure<std::string>("manifest is not a regular file");
          }
          return read_data_into_string(archive.get());
        }
        archive_read_data_skip(archive.get());
      }
    }

    return Failure<std::string>("Archive did not contain a manifest");
  }

  // Extract contents of rootfs to base_path (removes rootfs/ base)
  Status extract_rootfs_to(const std::string& base_path) {
    std::unique_ptr<struct archive, decltype(&archive_read_free)> archive{
        archive_read_new(), archive_read_free};
    archive_read_support_filter_all(archive.get());
    archive_read_support_format_all(archive.get());

    std::unique_ptr<struct archive, decltype(&archive_write_free)> writer{
        archive_write_disk_new(), archive_write_free};
    const int flags = ARCHIVE_EXTRACT_TIME
                        | ARCHIVE_EXTRACT_PERM
                        | ARCHIVE_EXTRACT_ACL
                        | ARCHIVE_EXTRACT_FFLAGS;
    archive_write_disk_set_options(writer.get(), flags);
    archive_write_disk_set_standard_lookup(writer.get());

    if (archive_read_open_filename(archive.get(), filename.c_str(), 10240) != ARCHIVE_OK) {
      return Error(archive_error_string(archive.get()));
    }

    struct archive_entry* entry;
    for (int r = archive_read_next_header(archive.get(), &entry);
         r == ARCHIVE_OK;
         r = archive_read_next_header(archive.get(), &entry)) {
      if (r == ARCHIVE_EOF) break;
      if (r < ARCHIVE_OK) return Error(archive_error_string(archive.get()));

      const std::string entry_path { archive_entry_pathname(entry) };

      if (entry_path == "manifest") {
        archive_read_data_skip(archive.get());
        continue;
      }

      std::string write_path { pathname::join(base_path,
                                              entry_path.substr(rootfs_filename.length())) };
      archive_entry_set_pathname(entry, write_path.c_str());

      if (archive_write_header(writer.get(), entry) != ARCHIVE_OK) {
        return Error(archive_error_string(writer.get()));
      }

      if (archive_entry_size(entry) > 0) {
        if (copy_data(archive.get(), writer.get())) {
          return Error(archive_error_string(writer.get()));
        }
      }

      if (archive_write_finish_entry(writer.get()) != ARCHIVE_OK) {
        return Error(archive_error_string(writer.get()));
      }
    }

    // Free will call close so this is not necessary above but used here to
    // report errors when closing.
    if (archive_write_close(writer.get()) != ARCHIVE_OK) {
      return Error(archive_error_string(writer.get()));
    }

    return Success();
  }
};


} // namespace image
} // namespace appc
