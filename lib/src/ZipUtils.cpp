// license:GPLv3+

#include "core/stdafx.h"
#include "ZipUtils.h"

#include <zip.h>
#include <fstream>

static bool IsExcludedPath(const string& path)
{
   return path.rfind("__MACOSX", 0) == 0 || path.find("/__MACOSX") != string::npos;
}

static int CountEntriesInDirectory(const std::filesystem::path& dirPath)
{
   int count = 0;
   std::error_code ec;
   for (auto it = std::filesystem::recursive_directory_iterator(dirPath, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      const string relativePath = std::filesystem::relative(it->path(), dirPath, ec).string();
      if (!IsExcludedPath(relativePath))
         count++;
   }
   return count;
}

struct ZipProgressContext {
   ZipUtils::ProgressCallback callback;
   int totalEntries;
};

static void ZipProgressCallback(zip_t* archive, double progress, void* userdata)
{
   auto* ctx = static_cast<ZipProgressContext*>(userdata);
   if (ctx && ctx->callback)
      ctx->callback(static_cast<int>(progress * ctx->totalEntries), ctx->totalEntries, "");
}

bool ZipUtils::Zip(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, ProgressCallback callback)
{
   std::error_code ec;
   if (!std::filesystem::is_directory(sourcePath, ec))
      return false;

   int error = 0;
   zip_t* archive = zip_open(destPath.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
   if (!archive)
      return false;

   const int totalEntries = CountEntriesInDirectory(sourcePath);
   int currentEntry = 0;

   for (auto it = std::filesystem::recursive_directory_iterator(sourcePath, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      const auto& entry = *it;
      const string relativePath = std::filesystem::relative(entry.path(), sourcePath, ec).string();

      if (IsExcludedPath(relativePath))
         continue;

      if (entry.is_directory(ec)) {
         const string dirPath = relativePath + "/";
         zip_dir_add(archive, dirPath.c_str(), ZIP_FL_ENC_UTF_8);
      }
      else if (entry.is_regular_file(ec)) {
         zip_source_t* fileSource = zip_source_file(archive, entry.path().string().c_str(), 0, -1);
         if (fileSource) {
            const zip_int64_t index = zip_file_add(archive, relativePath.c_str(), fileSource, ZIP_FL_ENC_UTF_8);
            if (index < 0) {
               zip_source_free(fileSource);
               PLOGE.printf("Failed to add file to zip: %s", relativePath.c_str());
            }
         }
      }

      currentEntry++;
   }

   ZipProgressContext progressCtx { callback, totalEntries };
   if (callback)
      zip_register_progress_callback_with_state(archive, 0.01, ZipProgressCallback, nullptr, &progressCtx);

   if (zip_close(archive) < 0)
      return false;

   return true;
}

bool ZipUtils::Unzip(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, ProgressCallback callback)
{
   int error = 0;
   zip_t* archive = zip_open(sourcePath.string().c_str(), ZIP_RDONLY, &error);
   if (!archive) {
      PLOGE.printf("Unable to unzip file: source=%s", sourcePath.string().c_str());
      return false;
   }

   const zip_int64_t totalEntries = zip_get_num_entries(archive, 0);

   for (zip_uint64_t i = 0; i < (zip_uint64_t)totalEntries; ++i) {
      zip_stat_t fileStat;
      if (zip_stat_index(archive, i, ZIP_STAT_NAME, &fileStat) != 0)
         continue;

      const string filename = fileStat.name;

      if (IsExcludedPath(filename))
         continue;

      const std::filesystem::path destFilePath = destPath / filename;

      std::error_code ec;
      if (filename.back() == '/') {
         std::filesystem::create_directories(destFilePath, ec);
         if (ec) {
            PLOGE.printf("Unable to create directory: %s, error=%s", destFilePath.string().c_str(), ec.message().c_str());
            continue;
         }
      }
      else {
         std::filesystem::create_directories(destFilePath.parent_path(), ec);
         if (ec) {
            PLOGE.printf("Unable to create directory: %s, error=%s", destFilePath.parent_path().string().c_str(), ec.message().c_str());
            continue;
         }

         zip_file_t* zipFile = zip_fopen_index(archive, i, 0);
         if (!zipFile) {
            PLOGE.printf("Unable to extract file: %s", destFilePath.string().c_str());
            continue;
         }

         std::ofstream ofs(destFilePath, std::ios::binary);
         char buf[4096];
         zip_int64_t len;
         while ((len = zip_fread(zipFile, buf, sizeof(buf))) > 0)
            ofs.write(buf, len);
         zip_fclose(zipFile);
      }

      if (callback)
         callback((int)(i + 1), (int)totalEntries, filename.c_str());
   }

   zip_close(archive);
   return true;
}

#ifdef VPX_ARCHIVE_SUPPORT
#include <archive.h>
#include <archive_entry.h>

// RAR (including RAR5) and 7z archives, read with libarchive
static bool ExtractWithLibarchive(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, const ZipUtils::ProgressCallback& callback)
{
   archive* const reader = archive_read_new();
   archive_read_support_format_rar(reader);
   archive_read_support_format_rar5(reader);
   archive_read_support_format_7zip(reader);
#ifdef _WIN32
   const int opened = archive_read_open_filename_w(reader, sourcePath.wstring().c_str(), 64 * 1024);
#else
   const int opened = archive_read_open_filename(reader, sourcePath.string().c_str(), 64 * 1024);
#endif
   if (opened != ARCHIVE_OK) {
      PLOGE.printf("Unable to open archive: source=%s, error=%s", sourcePath.string().c_str(), archive_error_string(reader));
      archive_read_free(reader);
      return false;
   }

   std::error_code ec;
   const int totalKB = static_cast<int>(std::filesystem::file_size(sourcePath, ec) / 1024);
   bool success = true;
   archive_entry* entry;
   int result;
   while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK || result == ARCHIVE_WARN) {
      const char* const utf8Name = archive_entry_pathname_utf8(entry);
#ifdef _WIN32
      const wchar_t* const wideName = archive_entry_pathname_w(entry); // Windows paths are UTF-16
      const std::filesystem::path name = wideName ? std::filesystem::path(wideName) : std::filesystem::path();
#else
      const std::filesystem::path name = utf8Name ? std::filesystem::path(reinterpret_cast<const char8_t*>(utf8Name)) : std::filesystem::path();
#endif
      const string displayName = utf8Name ? utf8Name : name.string();

      // Never write outside of the destination folder
      const std::filesystem::path relativePath = name.lexically_normal();
      if (relativePath.empty() || relativePath.is_absolute() || relativePath.has_root_name() || *relativePath.begin() == ".." || IsExcludedPath(relativePath.generic_string())) {
         if (!relativePath.empty() && !IsExcludedPath(relativePath.generic_string()))
            PLOGW.printf("Skipping archive entry outside of the destination: %s", displayName.c_str());
         archive_read_data_skip(reader);
         continue;
      }

      const std::filesystem::path destFilePath = destPath / relativePath;
      if (archive_entry_filetype(entry) == AE_IFDIR) {
         std::filesystem::create_directories(destFilePath, ec);
      }
      else if (archive_entry_filetype(entry) == AE_IFREG) {
         std::filesystem::create_directories(destFilePath.parent_path(), ec);
         std::ofstream ofs(destFilePath, std::ios::binary | std::ios::trunc);
         if (!ofs) {
            PLOGE.printf("Unable to create file: %s", destFilePath.string().c_str());
            archive_read_data_skip(reader);
            continue;
         }
         const void* block;
         size_t size;
         la_int64_t offset;
         int dataResult;
         while ((dataResult = archive_read_data_block(reader, &block, &size, &offset)) == ARCHIVE_OK)
         {
            ofs.seekp(offset); // Sparse entries give their offset
            ofs.write(static_cast<const char*>(block), static_cast<std::streamsize>(size));
         }
         if (dataResult != ARCHIVE_EOF) {
            PLOGE.printf("Unable to extract file: %s, error=%s", displayName.c_str(), archive_error_string(reader));
            success = false;
         }
      }
      else {
         archive_read_data_skip(reader); // Links and special files are not extracted
      }

      if (callback)
         callback(static_cast<int>(archive_filter_bytes(reader, -1) / 1024), totalKB, displayName.c_str());
   }
   if (result != ARCHIVE_EOF) {
      PLOGE.printf("Unable to read archive: source=%s, error=%s", sourcePath.string().c_str(), archive_error_string(reader));
      success = false;
   }
   archive_read_free(reader);
   return success;
}
#endif

const std::vector<string>& ZipUtils::GetExtractableExtensions()
{
#ifdef VPX_ARCHIVE_SUPPORT
   static const std::vector<string> extensions { "zip"s, "vpxz"s, "rar"s, "7z"s };
#else
   static const std::vector<string> extensions { "zip"s, "vpxz"s };
#endif
   return extensions;
}

bool ZipUtils::IsExtractable(const std::filesystem::path& path)
{
   const string ext = path.has_extension() ? lowerCase(path.extension().string().substr(1)) : string();
   const std::vector<string>& extensions = GetExtractableExtensions();
   return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

bool ZipUtils::Extract(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, ProgressCallback callback)
{
   const string ext = sourcePath.has_extension() ? lowerCase(sourcePath.extension().string()) : string();
#ifdef VPX_ARCHIVE_SUPPORT
   if (ext == ".rar" || ext == ".7z")
      return ExtractWithLibarchive(sourcePath, destPath, callback);
#endif
   if (ext == ".zip" || ext == ".vpxz")
      return Unzip(sourcePath, destPath, callback);
   PLOGE.printf("Unsupported archive: %s", sourcePath.string().c_str());
   return false;
}
