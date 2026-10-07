#include "outfit/NpcRecordReader.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Tailor::Records
{
    namespace
    {
        constexpr std::uint32_t kHeaderSize = 24;          // records and groups alike
        constexpr std::uint32_t kCompressed = 0x00040000;  // record flag
        constexpr std::uint32_t kMaxRecord = 64u << 20;    // refuse absurd sizes from a damaged file
        constexpr std::uint16_t kUseInventory = 0x0100;    // ACBS template flag

        struct NpcEntry
        {
            std::uint64_t offset = 0;
            std::uint32_t id = 0, size = 0, flags = 0;
        };
        struct PluginIndex
        {
            std::string name;  // owns every ID whose index is past the master list
            std::vector<std::string> masters;
            std::unordered_multimap<std::uint32_t, NpcEntry> npcs;  // by lower 24 bits
        };

        std::mutex cacheLock;
        std::unordered_map<std::string, PluginIndex> cache;  // plugins do not change while the game runs

        std::uint32_t U32(const char* at) { std::uint32_t value; std::memcpy(&value, at, 4); return value; }
        std::uint16_t U16(const char* at) { std::uint16_t value; std::memcpy(&value, at, 2); return value; }
        bool Is(const char* at, const char* type) { return std::memcmp(at, type, 4) == 0; }
        bool SameName(std::string_view left, std::string_view right)
        {
            return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        }

        // Walks a record's fields. XXXX carries the size of a field too large for 16 bits.
        template <class Visit>
        bool Fields(const std::vector<char>& data, Visit visit)
        {
            std::size_t at = 0;
            std::uint32_t oversized = 0;
            while (at + 6 <= data.size()) {
                const char* type = data.data() + at;
                std::uint32_t size = U16(type + 4);
                at += 6;
                if (Is(type, "XXXX")) {
                    if (size != 4 || at + 4 > data.size()) return false;
                    oversized = U32(data.data() + at);
                    at += 4;
                    continue;
                }
                if (oversized) { size = oversized; oversized = 0; }
                if (size > data.size() - at) return false;
                visit(type, data.data() + at, size);
                at += size;
            }
            return true;
        }

        std::optional<PluginIndex> BuildIndex(const std::filesystem::path& file)
        {
            std::error_code error;
            const std::uint64_t fileSize = std::filesystem::file_size(file, error);
            std::ifstream in(file, std::ios::binary);
            char header[kHeaderSize];
            if (error || !in.read(header, kHeaderSize) || !Is(header, "TES4") || U32(header + 4) > kMaxRecord) return std::nullopt;

            PluginIndex index;
            index.name = file.filename().string();
            std::vector<char> data(U32(header + 4));
            if (!in.read(data.data(), static_cast<std::streamsize>(data.size()))) return std::nullopt;
            if (!Fields(data, [&](const char* type, const char* at, std::uint32_t size) {
                    if (Is(type, "MAST")) index.masters.emplace_back(at, strnlen(at, size));
                })) return std::nullopt;

            // Below the header a plugin is a run of top-level groups, one per record type.
            for (std::uint64_t at = kHeaderSize + data.size(); at + kHeaderSize <= fileSize;) {
                in.seekg(static_cast<std::streamoff>(at));
                if (!in.read(header, kHeaderSize) || !Is(header, "GRUP")) return std::nullopt;
                const std::uint64_t groupSize = U32(header + 4);
                if (groupSize < kHeaderSize || groupSize > fileSize - at) return std::nullopt;
                if (Is(header + 8, "NPC_") && U32(header + 12) == 0) {
                    const std::uint64_t end = at + groupSize;
                    std::uint64_t inner = at + kHeaderSize;
                    while (inner + kHeaderSize <= end) {
                        in.seekg(static_cast<std::streamoff>(inner));
                        if (!in.read(header, kHeaderSize)) return std::nullopt;
                        const std::uint64_t size = U32(header + 4);
                        if (Is(header, "GRUP")) {
                            if (size < kHeaderSize || size > end - inner) return std::nullopt;
                            inner += size;
                            continue;
                        }
                        if (size > end - inner - kHeaderSize) return std::nullopt;
                        const auto id = U32(header + 12);
                        if (Is(header, "NPC_")) index.npcs.emplace(id & 0xFFFFFF, NpcEntry{inner, id, static_cast<std::uint32_t>(size), U32(header + 8)});
                        inner += kHeaderSize + size;
                    }
                    if (inner != end) return std::nullopt;
                }
                at += groupSize;
            }
            return index;
        }

        bool ReadRecord(const std::filesystem::path& file, const NpcEntry& entry, std::vector<char>& data)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in || entry.size > kMaxRecord) return false;
            std::vector<char> raw(entry.size);
            in.seekg(static_cast<std::streamoff>(entry.offset + kHeaderSize));
            if (!in.read(raw.data(), static_cast<std::streamsize>(raw.size()))) return false;
            if (!(entry.flags & kCompressed)) { data = std::move(raw); return true; }
            if (raw.size() < 4 || U32(raw.data()) > kMaxRecord) return false;
            data.resize(U32(raw.data()));
            uLongf written = static_cast<uLongf>(data.size());
            return uncompress(reinterpret_cast<Bytef*>(data.data()), &written, reinterpret_cast<const Bytef*>(raw.data()) + 4,
                       static_cast<uLong>(raw.size() - 4)) == Z_OK && written == data.size();
        }
    }

    std::optional<NpcOutfits> ReadNpcOutfits(const std::filesystem::path& file, std::string_view owner, std::uint32_t localId)
    {
        std::scoped_lock lock(cacheLock);
        auto cached = cache.find(file.string());
        if (cached == cache.end()) {
            auto index = BuildIndex(file);
            if (!index) return std::nullopt;
            cached = cache.emplace(file.string(), std::move(*index)).first;
        }
        const auto& index = cached->second;
        // An ID's top byte picks a master; past the master list it is the file itself.
        const auto ownerOf = [&](std::uint32_t id) -> const std::string& {
            const auto master = id >> 24;
            return master < index.masters.size() ? index.masters[master] : index.name;
        };
        const auto link = [&](const char* at) -> std::optional<FormRef> {
            const auto id = U32(at);
            if (!id) return std::nullopt;
            return FormRef{ownerOf(id), id & 0xFFFFFF};
        };

        for (auto [entry, last] = index.npcs.equal_range(localId & 0xFFFFFF); entry != last; ++entry) {
            if (!SameName(ownerOf(entry->second.id), owner)) continue;
            std::vector<char> data;
            if (!ReadRecord(file, entry->second, data)) return std::nullopt;
            NpcOutfits outfits;
            std::uint16_t templateFlags = 0;
            std::uint32_t templateId = 0;
            if (!Fields(data, [&](const char* type, const char* at, std::uint32_t size) {
                    if (Is(type, "ACBS") && size >= 20) templateFlags = U16(at + 18);
                    else if (Is(type, "TPLT") && size == 4) templateId = U32(at);
                    else if (Is(type, "DOFT") && size == 4) outfits.defaultOutfit = link(at);
                    else if (Is(type, "SOFT") && size == 4) outfits.sleepOutfit = link(at);
                })) return std::nullopt;
            outfits.usesTemplateInventory = templateId && (templateFlags & kUseInventory);
            return outfits;
        }
        return std::nullopt;
    }
}
