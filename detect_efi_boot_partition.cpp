/*
 * detect_efi_boot_partition
 *  A command line tool to find EFI boot partition and print its device name
 * 
 * Copyright (c) 2023 Tomoatsu Shimada
 */

#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <string.h>

#include <iostream>
#include <optional>
#include <filesystem>

#include <blkid/blkid.h>

#include <argparse/argparse.hpp>

static std::optional<std::filesystem::path> search_partition(const std::string& key, const std::string& value)
{
    blkid_cache _cache;
    if (blkid_get_cache(&_cache, "/dev/null") < 0) throw std::runtime_error("blkid_get_cache() failed");
    auto cache = std::shared_ptr<blkid_struct_cache>(_cache, blkid_put_cache);
    if (blkid_probe_all(cache.get()) < 0) throw std::runtime_error("blkid_probe_all() failed");
    std::shared_ptr<blkid_struct_dev_iterate> dev_iter(blkid_dev_iterate_begin(cache.get()),blkid_dev_iterate_end);
    if (!dev_iter)  throw std::runtime_error("blkid_dev_iterate_begin() failed");

    if (blkid_dev_set_search(dev_iter.get(), key.c_str(), value.c_str()) < 0) 
        throw std::runtime_error("blkid_dev_set_search() failed");
    blkid_dev dev = NULL;
    while (blkid_dev_next(dev_iter.get(), &dev) == 0) {
        dev = blkid_verify(cache.get(), dev);
        if (dev) return blkid_dev_devname(dev);
    }
    //else
    return {}; // not found
}

template <typename T> T read(int fd)
{
    T buf;
    auto r = read(fd, &buf, sizeof(buf));
    if (r < (ssize_t)sizeof(buf)) throw std::runtime_error("Boundary exceeded(EFI bug?)");
    return buf;
}

inline uint16_t read_le16(int fd) { return le16toh(read<uint16_t>(fd)); }
inline uint32_t read_le32(int fd) { return le32toh(read<uint32_t>(fd)); }

static std::filesystem::path detect_efi_boot_partition()
{
    int bc_fd = open("/sys/firmware/efi/efivars/BootCurrent-8be4df61-93ca-11d2-aa0d-00e098032b8c", O_RDONLY);
    if (bc_fd < 0) throw std::runtime_error("Cannot access EFI vars(No efivarfs mounted?)"); // no efi firmware?
    uint16_t boot_current = 0;
    try {
        read<uint32_t>(bc_fd); // skip 4 bytes
        boot_current = read_le16(bc_fd); // current boot #
    }
    catch (...) {
        close(bc_fd);
        throw;
    }
    close(bc_fd);

    char bootvarpath[80];
    if (sprintf(bootvarpath, "/sys/firmware/efi/efivars/Boot%04X-8be4df61-93ca-11d2-aa0d-00e098032b8c", boot_current) < 0) {
        throw std::runtime_error("sprintf()");
    }
    //else
    int fd = open(bootvarpath, O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot access EFI boot option");
    //else
    std::optional<std::string> partuuid;
    try {
        read<uint32_t>(fd); // skip 4 bytes
        read_le32(fd); // some flags
        read_le16(fd); // length of path list
        while (read_le16(fd) != 0x0000) { ; } // description

        while(!partuuid) {
            uint8_t type, subtype;
            type = read<uint8_t>(fd);
            subtype = read<uint8_t>(fd);
            if (type == 0x7f && subtype == 0xff) break; // end of device path
            // else
            auto struct_len = read_le16(fd);
            if (struct_len < 4) throw std::runtime_error("Invalid structure(length must not be less than 4)");
            if (type != 0x04/*MEDIA_DEVICE_PATH*/ || subtype != 0x01/*MEDIA_HARDDRIVE_DP*/) {
                ssize_t skip_len = struct_len - 4; 
                uint8_t buf[skip_len];
                if (read(fd, buf, skip_len) != skip_len)
                    throw std::runtime_error("Boundary exceeded(EFI bug?)");
                //else
                continue;
            }
            //else
            auto partition_number = read_le32(fd);
            read<uint64_t>(fd); // partition_start
            read<uint64_t>(fd); // partition_size
            uint8_t signature[16];
            for (size_t i = 0; i < sizeof(signature); i++) {
                signature[i] = read<uint8_t>(fd);
            }
            read<uint8_t>(fd); // mbrtype
            auto signaturetype = read<uint8_t>(fd);
            if (signaturetype == 1/*mbr*/) {
                uint32_t u32lebuf =
                    ((uint32_t)signature[0]) | ((uint32_t)signature[1] << 8) 
                    | ((uint32_t)signature[2] << 16) | ((uint32_t)signature[3] << 24);
                char buf[16];
                if (sprintf(buf, "%08x-%02d", u32lebuf, (int)partition_number) < 0) {
                    throw std::runtime_error("sprintf()");
                }
                //else
                partuuid = buf;
            } else if (signaturetype == 2/*gpt*/) {
                uint32_t u32lebuf =
                    ((uint32_t)signature[0]) | ((uint32_t)signature[1] << 8) 
                    | ((uint32_t)signature[2] << 16) | ((uint32_t)signature[3] << 24);
                uint16_t u16lebuf1 =
                    ((uint16_t)signature[4]) | ((uint16_t)signature[5] << 8);
                uint16_t u16lebuf2 =
                    ((uint16_t)signature[6]) | ((uint16_t)signature[7] << 8);
                uint16_t u16bebuf1 =
                    ((uint16_t)signature[8] << 8) | ((uint16_t)signature[9]);
                uint16_t u16bebuf2 =
                    ((uint16_t)signature[10] << 8) | ((uint16_t)signature[11]);
                uint32_t u32bebuf =
                    ((uint32_t)signature[12] << 24) | ((uint32_t)signature[13] << 16) 
                    | ((uint32_t)signature[14] << 8) | ((uint32_t)signature[15]);
                char buf[40];
                if (sprintf(buf, "%08x-%04x-%04x-%04x-%04x%08x", 
                    u32lebuf, u16lebuf1, u16lebuf2, u16bebuf1, u16bebuf2, u32bebuf) < 0) {
                    throw std::runtime_error("sprintf()");
                }
                //else
                partuuid = buf;
            }
        }
    }
    catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    if (!partuuid) throw std::runtime_error("Partition not found in device path");
    //else
    auto partition = search_partition("PARTUUID", *partuuid);
    if (!partition) throw std::runtime_error("Partition not found(PARTUUID=" + (*partuuid) + ")");
    return *partition;
}

int main(int argc, char* argv[])
{
    argparse::ArgumentParser program(argv[0]);
    program.add_argument("-q", "--quiet").default_value(false).implicit_value(true)
        .help("Don't show error message");
    try {
        program.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << program;
        return -1;
    }

    bool quiet = program.get<bool>("--quiet");

    if (!std::filesystem::is_directory("/sys/firmware/efi/efivars")) {
        if (!quiet) std::cerr << "No EFI variables available" << std::endl;
        return 1;
    }
    //else
    try {
        std::cout << detect_efi_boot_partition().string() << std::endl;
    }
    catch (const std::runtime_error& e) {
        if (!quiet) std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
