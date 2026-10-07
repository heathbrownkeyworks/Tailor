#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

// Writing Tailor's JSON files, and the id counter their loaders share. Engine-free, so TailorJsonFileTests runs it as
// the game does. Every store serializes its JSON before it calls WriteJsonFile (d7459298b): a name JSON can't hold
// throws there, before any file is touched.
namespace Tailor::Persistence
{
    namespace detail
    {
        // Text mode, as every save has written its file (CRLF on Windows). The stream is checked after the flush and
        // after the close, so a full disk or a failed write counts as a failure.
        inline bool WriteText(const std::filesystem::path& path, const std::string& contents)
        {
            std::ofstream file(path, std::ios::out | std::ios::trunc);
            if (!file.is_open()) return false;
            file << contents;
            file.flush();
            const bool written = file.good();
            file.close();
            return written && !file.fail();
        }

        inline bool ReadsBack(const std::filesystem::path& path, const std::string& contents)
        {
            std::ifstream file(path);
            if (!file.is_open()) return false;
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            return text == contents;
        }

        // The file's bytes, or none when it can't be read in full. A read that stops early looks like the end of the
        // file to the stream (a range another program has locked does that), so the count is checked against the size.
        inline std::optional<std::string> ReadBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) return std::nullopt;
            std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || bytes.size() != size) return std::nullopt;
            return bytes;
        }

        // A symbolic link, or a file with more than one name (a hard link).
        inline bool IsLinked(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (std::filesystem::is_symlink(status)) return true;
            if (!std::filesystem::is_regular_file(status)) return false;
            const auto names = std::filesystem::hard_link_count(path, error);
            return !error && names > 1;
        }
    }

    // Saves `contents` as `path`, never leaving it half written:
    //   1. writes <name>.tmp beside it, then renames that over the file;
    //   2. where the rename is refused (another program has the file open, an antivirus, MO2's virtual file system),
    //      copies the .tmp over the file instead, checks the copy and removes the .tmp, as settings.json does
    //      (38a5efa51). A file that is there but can't be read isn't copied over;
    //   3. a symbolic link, or a hard link with another name, is written in place through the link, as before, so a
    //      library shared through links stays shared. That write is not protected by a .tmp: as before this helper,
    //      a write that fails part way can leave a linked file short.
    // A <name>.tmp that is a file as the save starts is the full copy an earlier failed save kept (below). A save that
    // fails never removes it, and puts its own full contents in its place where it can, so the newest
    // full copy wins. While it is there, the save takes the same route
    // through <name>.next.tmp instead. A save that succeeds, on any route, removes the kept .tmp too.
    // Never throws. Returns false, with the reason in `error`, only when every route failed. The file is then as it
    // was and no .tmp is left, except:
    //   - a linked file, written in place, can be left short;
    //   - after a copy that failed and left the file changed or gone (or found no file to read first), or a copy that
    //     doesn't read back, the full contents are kept in <name>.tmp, and `error` says so;
    //   - with a kept <name>.tmp, a failed copy can still leave the file changed, and the kept .tmp always stays. A
    //     <name>.next.tmp that couldn't be written in full is removed and the kept .tmp stays as it was. Once it is
    //     written in full, every later failure renames it over the kept .tmp, so <name>.tmp holds the newest full
    //     contents. Where that rename is refused, both stay, and `error` names both, <name>.next.tmp as the newest.
    inline bool WriteJsonFile(const std::filesystem::path& path, const std::string& contents, std::string& error) noexcept
    {
        namespace fs = std::filesystem;
        try {
            std::error_code status;
            if (const auto folder = path.parent_path(); !folder.empty()) {
                fs::create_directories(folder, status);
                if (status) {
                    error = "could not create the folder: " + status.message();
                    return false;
                }
            }
            auto kept = path;
            kept += ".tmp";
            const auto removeFile = [](const fs::path& file) {
                // Only a file: a folder in its place is someone else's, and stays.
                std::error_code ignored;
                if (fs::is_regular_file(fs::symlink_status(file, ignored))) fs::remove(file, ignored);
            };
            if (detail::IsLinked(path)) {
                if (detail::WriteText(path, contents)) {
                    // The file holds the newer full contents, so a kept .tmp goes, as on every route.
                    removeFile(kept);
                    return true;
                }
                error = "could not write the file through its link";
                return false;
            }

            // A kept .tmp may be the only full copy, since the file itself can be damaged, so while it is there this
            // save is staged in <name>.next.tmp (a leftover one is an interrupted save's, never a kept copy). A status
            // error counts as kept, since Tailor can't tell; a folder is someone else's, not a kept copy. Below, "the
            // .tmp" is this save's own.
            std::error_code checked;
            const auto found = fs::symlink_status(kept, checked);
            const bool keeping = fs::is_regular_file(found) || (checked && found.type() != fs::file_type::not_found);
            auto temporary = path;
            temporary += keeping ? ".next.tmp" : ".tmp";
            const auto removeTemporary = [&] { removeFile(temporary); };
            // A failure once this save's .next.tmp is written in full: its contents are newer than the kept .tmp's, so
            // it takes the kept one's place, and <name>.tmp always holds the newest full contents.
            // Where that rename is refused, both stay. Returns where they are, for `error`.
            const auto keepNewest = [&] {
                std::error_code replaced;
                fs::rename(temporary, kept, replaced);
                if (!replaced) return "; the full contents are kept in " + kept.filename().string();
                return "; the full contents are kept in " + temporary.filename().string() + ", and an earlier save's in " +
                    kept.filename().string();
            };
            if (!detail::WriteText(temporary, contents)) {
                // Not written in full: the kept .tmp stays as it was.
                removeTemporary();
                error = "could not write " + temporary.filename().string() +
                    (keeping ? "; an earlier save's full contents are kept in " + kept.filename().string() : std::string{});
                return false;
            }

            fs::rename(temporary, path, status);
            if (!status) {
                // The file holds the newer full contents, so a kept .tmp goes too. One that can't be removed just
                // counts as kept next time.
                if (keeping) removeFile(kept);
                return true;
            }
            const auto refused = status.message();

            // The rename was refused: copy over the file instead. Its bytes are read first, because a copy can fail
            // part way and leave the file cut short or gone.
            std::error_code unknown;
            const bool exists = fs::exists(path, unknown);
            const auto before = detail::ReadBytes(path);
            if (exists && !before) {
                // A file that is there but can't be read couldn't be checked after a failed copy, so it isn't copied
                // over: it stays as it was, and the .tmp goes (or, with a kept .tmp, takes the kept one's place).
                std::string where;
                if (keeping) where = keepNewest();
                else removeTemporary();
                error = "the rename was refused (" + refused + ") and the file can't be read, so it wasn't copied over" + where;
                return false;
            }
            fs::copy_file(temporary, path, fs::copy_options::overwrite_existing, status);
            if (status) {
                // The .tmp goes only when the file still holds exactly what it held. Otherwise, or when there was no
                // file to read (none, or one Tailor couldn't check), the .tmp is the only full copy left and stays, as
                // settings.json's fallback leaves it (38a5efa51). With a kept .tmp, it takes the kept one's place
                // either way.
                const bool untouched = before && detail::ReadBytes(path) == before;
                std::string where;
                if (keeping) where = keepNewest();
                else if (untouched) removeTemporary();
                else where = "; the full contents are kept in " + temporary.filename().string();
                error = "the rename was refused (" + refused + ") and so was the copy (" + status.message() + ")" + where;
                return false;
            }
            if (!detail::ReadsBack(path, contents)) {
                error = "the copy over the file doesn't read back" +
                    (keeping ? keepNewest() : "; the full contents are kept in " + temporary.filename().string());
                return false;
            }
            removeTemporary();
            if (keeping) removeFile(kept);
            return true;
        } catch (...) {
            error = "an unexpected error while writing";
            return false;
        }
    }

    // Exports: a new file only, never over one, and removed again when it couldn't be written in full. Moved here
    // unchanged from OutfitTransfer.cpp.
    inline void WriteNew(const std::filesystem::path& path, const std::string& content)
    {
        // Exclusive creation also protects against repeated/racing export requests.
        std::ofstream file(path, std::ios::binary | std::ios::out | std::ios::noreplace);
        if (!file.is_open()) throw std::runtime_error("Cannot create the file. Use a new name and check folder permissions.");
        file << content;
        file.flush();
        const bool written = file.good();
        file.close();
        if (!written || file.fail()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw std::runtime_error("Could not finish writing the file. Check available disk space and folder permissions.");
        }
    }

    // The next id to hand out: past every id the file holds and never behind the file's own counter, so an older or
    // hand-edited file can't make a new outfit or category take an id already in use. None when no id is left:
    // Tailor never hands out INT_MAX, as Import doesn't.
    [[nodiscard]] inline std::optional<int> ReconcileNextId(std::int64_t stored, int maxSeen)
    {
        const auto next = (std::max)(stored, std::int64_t{maxSeen} + 1);
        if (next >= (std::numeric_limits<int>::max)()) return std::nullopt;
        return static_cast<int>(next);
    }
}
