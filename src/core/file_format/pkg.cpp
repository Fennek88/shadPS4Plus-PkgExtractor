// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <zlib.h>
#include "common/io_file.h"
#include "core/file_format/pkg.h"
#include "core/file_format/pkg_type.h"

static bool IsRangeWithin(u64 offset, u64 size, u64 limit)
{
    return offset <= limit && size <= limit - offset;
}

static bool IsValidTitleId(std::span<const char> titleId)
{
    return std::all_of(titleId.begin(), titleId.end(), [](unsigned char character)
    {
        return (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
    });
}

static bool IsValidUtf8(std::string_view value)
{
    for (size_t i = 0; i < value.size();)
    {
        const auto first = static_cast<u8>(value[i]);
        size_t sequenceLength = 0;
        if (first <= 0x7F)
            sequenceLength = 1;
        else if (first >= 0xC2 && first <= 0xDF)
            sequenceLength = 2;
        else if (first >= 0xE0 && first <= 0xEF)
            sequenceLength = 3;
        else if (first >= 0xF0 && first <= 0xF4)
            sequenceLength = 4;
        else
            return false;

        if (sequenceLength > value.size() - i)
            return false;
        for (size_t j = 1; j < sequenceLength; j++)
        {
            if ((static_cast<u8>(value[i + j]) & 0xC0) != 0x80)
                return false;
        }

        const auto second = sequenceLength > 1 ? static_cast<u8>(value[i + 1]) : 0;
        if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0) ||
            (first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90))
        {
            return false;
        }
        i += sequenceLength;
    }
    return true;
}

static bool ReadAt(Common::FS::IOFile& file, u64 offset, void* data, size_t size)
{
    if (size == 0)
        return true;
    return offset <= static_cast<u64>(std::numeric_limits<s64>::max()) &&
        file.Seek(static_cast<s64>(offset)) && file.ReadRaw<u8>(data, size) == size;
}

static bool CopyRangeToFile(Common::FS::IOFile& input, u64 offset, u64 size,
                            const std::filesystem::path& outputPath)
{
    Common::FS::IOFile output(outputPath, Common::FS::FileAccessMode::Write);
    if (!output.IsOpen())
        return false;

    std::array<u8, 64 * 1024> buffer{};
    while (size > 0)
    {
        const size_t chunkSize = static_cast<size_t>(std::min<u64>(size, buffer.size()));
        if (!ReadAt(input, offset, buffer.data(), chunkSize) ||
            output.WriteRaw<u8>(buffer.data(), chunkSize) != chunkSize)
        {
            return false;
        }
        offset += chunkSize;
        size -= chunkSize;
    }
    return true;
}

// Inflate one compressed PFSC block into the fixed-size PFS sector buffer.
static bool DecompressPFSC(std::span<char> compressed_data, std::span<char> decompressed_data)
{
    if (compressed_data.size() > std::numeric_limits<uInt>::max() ||
        decompressed_data.size() > std::numeric_limits<uInt>::max())
    {
        return false;
    }

    z_stream decompressStream{};
    if (inflateInit(&decompressStream) != Z_OK)
    {
        return false;
    }

    decompressStream.avail_in = static_cast<uInt>(compressed_data.size());
    decompressStream.next_in = reinterpret_cast<Bytef*>(compressed_data.data());
    decompressStream.avail_out = static_cast<uInt>(decompressed_data.size());
    decompressStream.next_out = reinterpret_cast<Bytef*>(decompressed_data.data());

    const int inflateResult = inflate(&decompressStream, Z_FINISH);
    const bool success = inflateResult == Z_STREAM_END &&
        decompressStream.total_out == decompressed_data.size();
    const int endResult = inflateEnd(&decompressStream);
    return success && endResult == Z_OK;
}

static bool ReadDirent(std::span<const char> block, size_t offset, Dirent& dirent,
                       size_t& recordSize)
{
    constexpr size_t headerSize = offsetof(Dirent, name);
    if (offset > block.size() || headerSize > block.size() - offset)
        return false;

    std::memset(&dirent, 0, sizeof(dirent));
    std::memcpy(&dirent, block.data() + offset, headerSize);
    if (dirent.ino == 0)
    {
        recordSize = 0;
        return true;
    }
    if (dirent.namelen <= 0 || static_cast<size_t>(dirent.namelen) > sizeof(dirent.name) ||
        dirent.entsize < static_cast<s32>(headerSize + static_cast<size_t>(dirent.namelen)) ||
        static_cast<size_t>(dirent.entsize) > block.size() - offset)
    {
        return false;
    }

    std::memcpy(dirent.name, block.data() + offset + headerSize,
                static_cast<size_t>(dirent.namelen));
    recordSize = static_cast<size_t>(dirent.entsize);
    return true;
}

// Locate the PFSC header within the decrypted PFS image by scanning its aligned offsets.
std::optional<size_t> GetPFSCOffset(std::span<const u8> pfs_image)
{
    static constexpr u32 PfscMagic = 0x43534650;
    if (pfs_image.size() < 0x20000 + sizeof(u32))
        return std::nullopt;
    u32 value;
    for (size_t i = 0x20000; i <= pfs_image.size() - sizeof(value); i += 0x10000)
    {
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PfscMagic)
            return i;
    }
    return std::nullopt;
}

PKG::PKG() = default;

PKG::~PKG() = default;

// Read and validate the PKG header, then load the SFO metadata needed to identify the package.
bool PKG::Open(const std::filesystem::path& filepath, std::string& failreason)
{
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen())
    {
        failreason = "Could not open the PKG file";
        return false;
    }
    pkgSize = file.GetSize();

    if (pkgSize < sizeof(pkgheader) || !ReadAt(file, 0, &pkgheader, sizeof(pkgheader)))
    {
        failreason = "PKG header is truncated";
        return false;
    }
    if (pkgheader.magic != 0x7F434E54)
    {
        failreason = "Invalid PKG magic number";
        return false;
    }

    const u64 declaredPkgSize = pkgheader.pkg_size;
    const u64 tableOffset = pkgheader.pkg_table_entry_offset;
    const u64 tableSize = static_cast<u64>(pkgheader.pkg_table_entry_count) * sizeof(PKGEntry);
    if (declaredPkgSize < sizeof(pkgheader) || declaredPkgSize > pkgSize ||
        !IsRangeWithin(tableOffset, tableSize, declaredPkgSize))
    {
        failreason = "PKG table or declared file size is invalid";
        return false;
    }
    if (!IsRangeWithin(pkgheader.pkg_content_offset, pkgheader.pkg_content_size, declaredPkgSize))
    {
        failreason = "PKG content range exceeds the package size";
        return false;
    }

    pkgFlags.clear();
    for (const auto& flag : flagNames)
    {
        if (isFlagSet(pkgheader.pkg_content_flags, flag.first))
        {
            if (!pkgFlags.empty())
                pkgFlags += (", ");
            pkgFlags += (flag.second);
        }
    }

    // The title ID starts seven bytes into the fixed-width content ID field.
    if (!ReadAt(file, 0x47, pkgTitleID, sizeof(pkgTitleID)))
    {
        failreason = "PKG content ID is truncated";
        return false;
    }
    if (!IsValidTitleId(pkgTitleID))
    {
        failreason = "PKG title ID contains invalid path characters";
        return false;
    }

    sfo.clear();
    for (u64 i = 0; i < pkgheader.pkg_table_entry_count; i++)
    {
        PKGEntry entry{};
        const u64 entryOffset = tableOffset + i * sizeof(PKGEntry);
        if (!ReadAt(file, entryOffset, &entry, sizeof(entry)))
        {
            failreason = "PKG table entry is truncated";
            return false;
        }
        if (!IsRangeWithin(entry.offset, entry.size, declaredPkgSize))
        {
            failreason = "PKG entry points outside the package";
            return false;
        }

        // Try to figure out the name
        const auto name = GetEntryNameByType(entry.id);
        if (name == "param.sfo")
        {
            constexpr u32 MaxSfoSize = 16 * 1024 * 1024;
            if (entry.size > MaxSfoSize)
            {
                failreason = "PKG SFO metadata is too large";
                return false;
            }
            sfo.resize(entry.size);
            if (!ReadAt(file, entry.offset, sfo.data(), sfo.size()))
            {
                failreason = "PKG SFO metadata is truncated";
                return false;
            }
        }
    }

    if (sfo.empty())
    {
        failreason = "PKG does not contain param.sfo metadata";
        return false;
    }

    return true;
}

// Decrypt package metadata, build the PFS inode/path tables, and reject entries that escape the output root.
bool PKG::Extract(const std::filesystem::path& filepath, const std::filesystem::path& extract,
                  std::string& failreason)
{
    failreason.clear();
    fsTable.clear();
    iNodeBuf.clear();
    sectorMap.clear();
    extractPaths.clear();
    pkgFlags.clear();
    sfo.clear();

    extract_path = extract;
    current_dir = extract_path;
    pkgpath = filepath;
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen())
    {
        failreason = "Could not open the PKG file";
        return false;
    }
    pkgSize = file.GetSize();
    if (pkgSize < sizeof(PKGHeader) || !ReadAt(file, 0, &pkgheader, sizeof(PKGHeader)))
    {
        failreason = "PKG header is truncated";
        return false;
    }

    if (pkgheader.magic != 0x7F434E54)
    {
        failreason = "Invalid PKG magic number";
        return false;
    }

    const u64 declaredPkgSize = pkgheader.pkg_size;
    const u64 tableOffset = pkgheader.pkg_table_entry_offset;
    const u64 tableSize = static_cast<u64>(pkgheader.pkg_table_entry_count) * sizeof(PKGEntry);
    if (declaredPkgSize < sizeof(PKGHeader) || declaredPkgSize > pkgSize ||
        !IsRangeWithin(tableOffset, tableSize, declaredPkgSize))
    {
        failreason = "PKG table or declared file size is invalid";
        return false;
    }
    if (!IsRangeWithin(pkgheader.pkg_content_offset, pkgheader.pkg_content_size, declaredPkgSize))
    {
        failreason = "PKG content range exceeds the package size";
        return false;
    }
    if (!ReadAt(file, 0x47, pkgTitleID, sizeof(pkgTitleID)) || !IsValidTitleId(pkgTitleID))
    {
        failreason = "PKG title ID is invalid";
        return false;
    }

    std::array<u8, 64> concatenated_ivkey_dk3{};
    std::array<u8, 32> seed_digest{};
    std::array<std::array<u8, 32>, 7> digest1{};
    std::array<std::array<u8, 256>, 7> key1{};
    std::array<u8, 256> imgkeydata{};

    for (u64 i = 0; i < pkgheader.pkg_table_entry_count; i++)
    {
        PKGEntry entry{};
        const u64 entryOffset = tableOffset + i * sizeof(PKGEntry);
        if (!ReadAt(file, entryOffset, &entry, sizeof(entry)))
        {
            failreason = "PKG table entry is truncated";
            return false;
        }
        if (!IsRangeWithin(entry.offset, entry.size, declaredPkgSize))
        {
            failreason = "PKG entry points outside the package";
            return false;
        }

        // Try to figure out the name
        const auto name = GetEntryNameByType(entry.id);
        const auto filepath = extract_path / "sce_sys" / name;
        std::error_code fsError;
        std::filesystem::create_directories(filepath.parent_path(), fsError);
        if (fsError)
        {
            failreason = "Could not create package metadata directory: " + fsError.message();
            return false;
        }

        if (name.empty())
        {
            // Just print with id
            const auto outputPath = extract_path / "sce_sys" / std::to_string(entry.id);
            if (!CopyRangeToFile(file, entry.offset, entry.size, outputPath))
            {
                failreason = "Failed to copy PKG entry data";
                return false;
            }
            continue;
        }

        if (entry.id == 0x1)
        {
            // DIGESTS, seek;
            // file.Seek(entry.offset, fsSeekSet);
        }
        else if (entry.id == 0x10)
        {
            constexpr size_t EntryKeyDataSize = 32 + 7 * 32 + 7 * 256;
            if (entry.size < EntryKeyDataSize ||
                !ReadAt(file, entry.offset, seed_digest.data(), seed_digest.size()))
            {
                failreason = "PKG entry key data is truncated";
                return false;
            }
            u64 keyOffset = entry.offset + seed_digest.size();
            for (int i = 0; i < 7; i++)
            {
                if (!ReadAt(file, keyOffset, digest1[i].data(), digest1[i].size()))
                {
                    failreason = "PKG entry key data is truncated";
                    return false;
                }
                keyOffset += digest1[i].size();
            }
            for (int i = 0; i < 7; i++)
            {
                if (!ReadAt(file, keyOffset, key1[i].data(), key1[i].size()))
                {
                    failreason = "PKG entry key data is truncated";
                    return false;
                }
                keyOffset += key1[i].size();
            }

            PKG::crypto.RSA2048Decrypt(dk3_, key1[3], true); // decrypt DK3
        }
        else if (entry.id == 0x20)
        {
            if (entry.size < imgkeydata.size() ||
                !ReadAt(file, entry.offset, imgkeydata.data(), imgkeydata.size()))
            {
                failreason = "PKG image key data is truncated";
                return false;
            }

            // The Concatenated iv + dk3 imagekey for HASH256
            std::memcpy(concatenated_ivkey_dk3.data(), &entry, sizeof(entry));
            std::memcpy(concatenated_ivkey_dk3.data() + sizeof(entry), dk3_.data(), sizeof(dk3_));

            PKG::crypto.ivKeyHASH256(concatenated_ivkey_dk3, ivKey); // ivkey_
            // imgkey_ to use for last step to get ekpfs
            PKG::crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
            // ekpfs key to get data and tweak keys.
            PKG::crypto.RSA2048Decrypt(ekpfsKey, imgKey, false);
        }
        else if (entry.id == 0x80)
        {
            // GENERAL_DIGESTS, seek;
            // file.Seek(entry.offset, fsSeekSet);
        }

        const auto outputPath = extract_path / "sce_sys" / name;
        if (!CopyRangeToFile(file, entry.offset, entry.size, outputPath))
        {
            failreason = "Failed to copy PKG entry data";
            return false;
        }

        // Decrypt Np stuff and overwrite.
        if (entry.id == 0x400 || entry.id == 0x401 || entry.id == 0x402 ||
            entry.id == 0x403)
        {
            // somehow 0x401 is not decrypting
            constexpr u32 MaxEncryptedEntrySize = 64 * 1024 * 1024;
            if (entry.size > MaxEncryptedEntrySize)
            {
                failreason = "Encrypted PKG metadata entry is too large";
                return false;
            }
            const size_t encryptedSize = entry.size;
            const size_t paddedSize = (encryptedSize + 15) & ~size_t{15};
            std::vector<u8> cipherNp(paddedSize);
            decNp.resize(paddedSize);
            if (!ReadAt(file, entry.offset, cipherNp.data(), encryptedSize))
            {
                failreason = "Encrypted PKG metadata entry is truncated";
                return false;
            }
            std::array<u8, 64> concatenated_ivkey_dk3_;
            std::memcpy(concatenated_ivkey_dk3_.data(), &entry, sizeof(entry));
            std::memcpy(concatenated_ivkey_dk3_.data() + sizeof(entry), dk3_.data(), sizeof(dk3_));
            PKG::crypto.ivKeyHASH256(concatenated_ivkey_dk3_, ivKey);
            PKG::crypto.aesCbcCfb128DecryptEntry(ivKey, cipherNp, decNp);

            Common::FS::IOFile out(extract_path / "sce_sys" / name,
                                   Common::FS::FileAccessMode::Write);
            if (!out.IsOpen() || out.WriteRaw<u8>(decNp.data(), encryptedSize) != encryptedSize)
            {
                failreason = "Failed to write decrypted PKG metadata";
                return false;
            }
            out.Close();
        }
    }

    // Read the seed and encrypted PFS metadata only after validating their package ranges.
    std::array<u8, 16> seed{};
    const u64 pfsImageOffset = pkgheader.pfs_image_offset;
    const u64 pfsImageSize = pkgheader.pfs_image_size;
    // Small packages (e.g. DLC) can declare a cache larger than the PFS image itself.
    const u64 pfsCacheBytes =
        std::min<u64>(static_cast<u64>(pkgheader.pfs_cache_size) * 2, pfsImageSize);
    constexpr u64 MaxPfsCacheBytes = 256 * 1024 * 1024;
    if (pfsImageSize == 0 || !IsRangeWithin(pfsImageOffset, pfsImageSize, declaredPkgSize) ||
        !IsRangeWithin(pfsImageOffset, 0x370 + seed.size(), declaredPkgSize) ||
        !ReadAt(file, pfsImageOffset + 0x370, seed.data(), seed.size()))
    {
        failreason = "PFS seed is outside the package or truncated";
        return false;
    }
    if (pfsCacheBytes == 0 || pfsCacheBytes > MaxPfsCacheBytes ||
        pfsCacheBytes % 0x1000 != 0 ||
        !IsRangeWithin(pfsImageOffset, pfsCacheBytes, declaredPkgSize))
    {
        failreason = "PFS cache range or size is invalid";
        return false;
    }

    // Get data and tweak keys.
    PKG::crypto.PfsGenCryptoKey(ekpfsKey, seed, dataKey, tweakKey);
    const size_t pfsSize = static_cast<size_t>(pfsCacheBytes);
    std::vector<u8> pfs_encrypted(pfsSize);
    if (!ReadAt(file, pfsImageOffset, pfs_encrypted.data(), pfs_encrypted.size()))
    {
        failreason = "PFS cache data is truncated";
        return false;
    }
    std::vector<u8> pfs_decrypted(pfsSize);
    PKG::crypto.decryptPFS(dataKey, tweakKey, pfs_encrypted, pfs_decrypted, 0);

    const auto pfscOffset = GetPFSCOffset(pfs_decrypted);
    if (!pfscOffset || *pfscOffset > pfs_decrypted.size() ||
        sizeof(PFSCHdr) > pfs_decrypted.size() - *pfscOffset)
    {
        failreason = "PFS cache does not contain a valid PFSC header";
        return false;
    }
    pfsc_offset = *pfscOffset;
    std::vector<u8> pfsc(pfs_decrypted.begin() + pfsc_offset, pfs_decrypted.end());

    PFSCHdr pfsChdr{};
    std::memcpy(&pfsChdr, pfsc.data(), sizeof(pfsChdr));
    if (pfsChdr.block_sz2 <= 0 || pfsChdr.data_length <= 0 ||
        pfsChdr.data_length % pfsChdr.block_sz2 != 0 || pfsChdr.block_offsets < 0)
    {
        failreason = "PFSC block table parameters are invalid";
        return false;
    }
    const u64 blockCount = static_cast<u64>(pfsChdr.data_length / pfsChdr.block_sz2);
    const u64 blockOffsets = static_cast<u64>(pfsChdr.block_offsets);
    if (blockCount == 0 || blockCount > std::numeric_limits<size_t>::max() - 1 ||
        blockOffsets > pfsc.size() || blockCount + 1 > (pfsc.size() - blockOffsets) / sizeof(u64))
    {
        failreason = "PFSC block table exceeds the PFS cache";
        return false;
    }
    sectorMap.resize(static_cast<size_t>(blockCount + 1));
    for (size_t i = 0; i < sectorMap.size(); i++)
    {
        std::memcpy(&sectorMap[i], pfsc.data() + blockOffsets + i * sizeof(u64), sizeof(u64));
        if (sectorMap[i] > pfsImageSize - pfsc_offset ||
            (i > 0 && sectorMap[i] < sectorMap[i - 1]))
        {
            failreason = "PFSC sector offset exceeds the PFS image";
            return false;
        }
        if (i > 0 && (sectorMap[i] == sectorMap[i - 1] ||
            sectorMap[i] - sectorMap[i - 1] > 0x10000))
        {
            failreason = "PFSC sector size is invalid";
            return false;
        }
    }
    const size_t num_blocks = static_cast<size_t>(blockCount);

    u32 ndinode = 0;
    int ndinode_counter = 0;
    bool dinode_reached = false;
    bool uroot_reached = false;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(0x10000);

    // Decompress PFSC sectors to collect inodes and directory entries that describe the PFS tree.
    for (size_t i = 0; i < num_blocks; i++)
    {
        const u64 sectorOffset = sectorMap[i];
        const u64 sectorSize = sectorMap[i + 1] - sectorOffset;
        if (sectorSize == 0 || sectorSize > 0x10000 ||
            !IsRangeWithin(sectorOffset, sectorSize, pfsc.size()))
        {
            failreason = "PFSC metadata sector is outside the PFS cache";
            return false;
        }

        compressedData.resize(static_cast<size_t>(sectorSize));
        std::memcpy(compressedData.data(), pfsc.data() + sectorOffset, sectorSize);

        if (sectorSize == 0x10000) // Uncompressed data
            std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
        else if (!DecompressPFSC(compressedData, decompressedData))
        {
            failreason = "Could not decompress PFSC metadata sector";
            return false;
        }

        if (i == 0)
        {
            std::memcpy(&ndinode, decompressedData.data() + 0x30, 4); // number of folders and files
            constexpr u32 MaxInodeCount = 1'000'000;
            if (ndinode == 0 || ndinode > MaxInodeCount)
            {
                failreason = "PFS inode count is invalid";
                return false;
            }
        }

        const u64 inodeBytes = static_cast<u64>(ndinode) * 0xA8;
        const size_t occupied_blocks = static_cast<size_t>((inodeBytes + 0xFFFF) / 0x10000);
        if (occupied_blocks >= num_blocks)
        {
            failreason = "PFS inode table exceeds the metadata sectors";
            return false;
        }

        if (i >= 1 && i <= occupied_blocks)
        {
            // Get all iNodes, gives type, file size and location.
            for (size_t p = 0; p + 0xA8 <= decompressedData.size() && iNodeBuf.size() < ndinode;
                 p += 0xA8)
            {
                Inode node{};
                std::memcpy(&node, &decompressedData[p], sizeof(node));
                if (node.Mode == 0)
                {
                    break;
                }
                iNodeBuf.push_back(node);
            }
        }

        // let's deal with the root/uroot entries here.
        // Sometimes it's more than 2 entries (Tomb Raider Remastered)
        const std::string_view flat_path_table(&decompressedData[0x10], 15);
        if (flat_path_table == "flat_path_table")
        {
            uroot_reached = true;
        }

        if (uroot_reached)
        {
            for (size_t pos = 0; pos < decompressedData.size();)
            {
                Dirent dirent{};
                size_t recordSize = 0;
                if (!ReadDirent(decompressedData, pos, dirent, recordSize))
                {
                    failreason = "PFS root directory entry is malformed";
                    return false;
                }
                if (dirent.ino != 0)
                {
                    ndinode_counter++;
                    pos += recordSize;
                }
                else
                {
                    // Set the the folder according to the current inode.
                    // Can be 2 or more (rarely)
                    auto parent_path = extract_path.parent_path();
                    auto title_id = GetTitleID();

                    if (parent_path.filename() != title_id &&
                        !extract_path.string().ends_with("-patch"))
                    {
                        extractPaths[ndinode_counter] = parent_path / title_id;
                    }
                    else
                    {
                        // DLCs path has different structure
                        extractPaths[ndinode_counter] = extract_path;
                    }
                    uroot_reached = false;
                    break;
                }
            }
        }

        const char dot = decompressedData[0x10];
        const std::string_view dotdot(&decompressedData[0x28], 2);
        if (dot == '.' && dotdot == "..")
        {
            dinode_reached = true;
        }

        // Get folder and file names.
        bool end_reached = false;
        if (dinode_reached)
        {
            for (size_t pos = 0; pos < decompressedData.size();)
            {
                Dirent dirent{};
                size_t recordSize = 0;
                if (!ReadDirent(decompressedData, pos, dirent, recordSize))
                {
                    failreason = "PFS directory entry is malformed";
                    return false;
                }

                // Stop here and continue the main loop
                if (dirent.ino == 0)
                {
                    break;
                }

                auto& table = fsTable.emplace_back();
                table.name = std::string(dirent.name, dirent.namelen);
                table.inode = static_cast<u32>(dirent.ino);
                table.type = dirent.type;
                pos += recordSize;

                if (table.inode >= iNodeBuf.size() ||
                    (table.type != PFS_FILE && table.type != PFS_DIR &&
                        table.type != PFS_CURRENT_DIR && table.type != PFS_PARENT_DIR))
                {
                    failreason = "PFS directory entry references an invalid inode or type";
                    return false;
                }

                // Restrict each serialized name to one safe path component on every platform.
                if (!IsValidUtf8(table.name) || table.name.find_first_of("/\\:") != std::string::npos ||
                    table.name.find('\0') != std::string::npos)
                {
                    failreason = "PKG contains an invalid PFS path component";
                    return false;
                }

                std::u8string utf8Name;
                utf8Name.reserve(table.name.size());
                for (unsigned char character : table.name)
                    utf8Name.push_back(static_cast<char8_t>(character));
                const std::filesystem::path entryPath(utf8Name);
                const bool isParentEntry = table.type == PFS_PARENT_DIR && entryPath == "..";
                const bool isCurrentEntry = table.type == PFS_CURRENT_DIR && entryPath == ".";
                if (entryPath.empty() || entryPath.is_absolute() || entryPath.has_root_name() ||
                    (table.type == PFS_PARENT_DIR && !isParentEntry) ||
                    (table.type == PFS_CURRENT_DIR && !isCurrentEntry) ||
                    ((table.type == PFS_FILE || table.type == PFS_DIR) &&
                        (entryPath == "." || entryPath == "..")))
                {
                    failreason = "PKG contains a path that escapes the extraction directory";
                    return false;
                }

                if (table.type == PFS_CURRENT_DIR)
                {
                    const auto current = extractPaths.find(table.inode);
                    if (current == extractPaths.end())
                    {
                        failreason = "PFS current-directory entry references an unknown path";
                        return false;
                    }
                    current_dir = current->second;
                }
                const auto resolvedPath = (current_dir / entryPath).lexically_normal();
                if (table.type == PFS_FILE || table.type == PFS_DIR)
                {
                    const auto relativePath = resolvedPath.lexically_relative(extract_path.lexically_normal());
                    if (relativePath.empty() || relativePath.is_absolute() ||
                        *relativePath.begin() == "..")
                    {
                        failreason = "PKG contains a path that escapes the extraction directory";
                        return false;
                    }
                }
                extractPaths[table.inode] = resolvedPath;

                if (table.type == PFS_FILE || table.type == PFS_DIR)
                {
                    if (table.type == PFS_DIR)
                    {
                        // Create dirs.
                        std::error_code directoryError;
                        std::filesystem::create_directories(extractPaths[table.inode], directoryError);
                        if (directoryError)
                        {
                            failreason = "Could not create extracted directory: " + directoryError.message();
                            return false;
                        }
                    }
                    ndinode_counter++;
                    if ((ndinode_counter + 1) == ndinode) // 1 for the image itself (root).
                        end_reached = true;
                }
            }
            if (end_reached)
            {
                break;
            }
        }
    }

    if (iNodeBuf.size() != ndinode)
    {
        failreason = "PFS inode table is truncated";
        return false;
    }
    return true;
}

// Decrypt and decompress the requested file's PFS sectors into its resolved output path.
bool PKG::ExtractFiles(const int index, std::string& failreason,
                       const std::atomic_bool* cancelRequested,
                       const std::function<void(const std::string&, int, int)>& onProgress)
{
    failreason.clear();
    if (index < 0 || static_cast<size_t>(index) >= fsTable.size())
    {
        failreason = "PFS file index is out of range";
        return false;
    }

    const u32 inodeNumber = fsTable[index].inode;
    const int inodeType = fsTable[index].type;
    const std::string inodeName = fsTable[index].name;

    if (inodeType == PFS_FILE)
    {
        if (inodeNumber >= iNodeBuf.size())
        {
            failreason = "PFS file references an invalid inode";
            return false;
        }
        const Inode& inode = iNodeBuf[inodeNumber];
        if (inode.Size < 0)
        {
            failreason = "PFS file has an invalid size";
            return false;
        }
        const u64 fileSize = static_cast<u64>(inode.Size);
        const u64 blockCount = inode.Blocks;
        const u64 firstSector = inode.loc;
        if ((fileSize != 0 && blockCount == 0) || sectorMap.empty() ||
            blockCount > sectorMap.size() - 1 || firstSector > sectorMap.size() - 1 ||
            blockCount > sectorMap.size() - 1 - firstSector ||
            fileSize > blockCount * 0x10000 ||
            (fileSize != 0 && fileSize <= (blockCount - 1) * 0x10000))
        {
            failreason = "PFS file block range is invalid";
            return false;
        }

        Common::FS::IOFile inflated;
        if (const auto outputPath = extractPaths.find(inodeNumber);
            outputPath == extractPaths.end() ||
            inflated.Open(outputPath->second, Common::FS::FileAccessMode::Write) != 0 ||
            !inflated.IsOpen())
        {
            failreason = "Could not open the extracted PFS file for writing";
            return false;
        }

        Common::FS::IOFile pkgFile(pkgpath, Common::FS::FileAccessMode::Read);
        if (!pkgFile.IsOpen())
        {
            failreason = "Could not reopen the PKG file";
            return false;
        }

        std::vector<char> compressedData;
        std::vector<char> decompressedData(0x10000);

        if (onProgress)
        {
            onProgress(inodeName, 0, static_cast<int>(blockCount));
        }
        u64 bytesWritten = 0;
        for (u64 j = 0; j < blockCount; j++)
        {
            if (cancelRequested != nullptr &&
                cancelRequested->load(std::memory_order_relaxed))
            {
                return false;
            }

            const u64 sectorOffset = sectorMap[firstSector + j];
            const u64 sectorSize = sectorMap[firstSector + j + 1] - sectorOffset;
            if (sectorSize == 0 || sectorSize > 0x10000 ||
                sectorOffset > std::numeric_limits<u64>::max() - pfsc_offset)
            {
                failreason = "PFS file sector size or offset is invalid";
                return false;
            }
            const u64 absoluteSectorOffset = pfsc_offset + sectorOffset;
            const u64 previousData = absoluteSectorOffset & 0xFFF;
            const u64 alignedOffset = absoluteSectorOffset - previousData;
            const u64 bytesNeeded = previousData + sectorSize;
            const u64 encryptedSize = (bytesNeeded + 0xFFF) & ~u64{0xFFF};
            const u64 packageOffset = static_cast<u64>(pkgheader.pfs_image_offset) + alignedOffset;
            if (encryptedSize == 0 || encryptedSize > 0x11000 ||
                !IsRangeWithin(alignedOffset, encryptedSize,
                               static_cast<u64>(pkgheader.pfs_image_size)) ||
                !IsRangeWithin(packageOffset, encryptedSize, pkgSize))
            {
                failreason = "PFS file sector points outside the package";
                return false;
            }

            std::vector<u8> encrypted(static_cast<size_t>(encryptedSize));
            std::vector<u8> decrypted(static_cast<size_t>(encryptedSize));
            if (!ReadAt(pkgFile, packageOffset, encrypted.data(), encrypted.size()))
            {
                failreason = "PFS file sector is truncated";
                return false;
            }
            PKG::crypto.decryptPFS(dataKey, tweakKey, encrypted, decrypted,
                                   absoluteSectorOffset / 0x1000);

            compressedData.resize(static_cast<size_t>(sectorSize));
            std::memcpy(compressedData.data(), decrypted.data() + previousData,
                        static_cast<size_t>(sectorSize));

            if (sectorSize == 0x10000) // Uncompressed data
                std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
            else if (!DecompressPFSC(compressedData, decompressedData))
            {
                failreason = "Could not decompress PFS file sector";
                return false;
            }

            const size_t writeSize = static_cast<size_t>(std::min<u64>(0x10000, fileSize - bytesWritten));
            if (writeSize > 0 && inflated.WriteRaw<u8>(decompressedData.data(), writeSize) != writeSize)
            {
                failreason = "Failed to write extracted PFS file data";
                return false;
            }
            bytesWritten += writeSize;
            if (onProgress)
            {
                onProgress(inodeName, static_cast<int>(j + 1), static_cast<int>(blockCount));
            }
        }
        if (bytesWritten != fileSize)
        {
            failreason = "PFS file data is incomplete";
            return false;
        }
        inflated.Close();
    }
    return true;
}
