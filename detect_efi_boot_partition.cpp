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

static std::optional<std::filesystem::path>
    search_partition(const std::string& key, const std::string& value)
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

typedef std::shared_ptr<int> auto_fd;

static auto_fd open(const std::filesystem::path& path, int flags = O_RDONLY)
{
    auto_fd fd(
        new int(::open(path.c_str(), flags)),
        [](int* fd) {if (fd) { if (*fd >= 0) close(*fd); delete fd; }}
    );
    //else
    return (fd && *fd >= 0)? fd : auto_fd(nullptr);
}

inline void read(auto_fd fd, void* buf, size_t size)
{
    if (!fd) throw std::runtime_error("File descriptor invalid");
    auto r = ::read(*fd, buf, size);
    if (r < (ssize_t)size) throw std::runtime_error("Boundary exceeded(EFI bug?)");
}

template <typename T> T read(auto_fd fd)
{
    T buf;
    read(fd, &buf, sizeof(buf));
    return buf;
}

inline uint16_t read_le16(auto_fd fd) { return le16toh(read<uint16_t>(fd)); }
inline uint32_t read_le32(auto_fd fd) { return le32toh(read<uint32_t>(fd)); }
inline uint64_t read_le64(auto_fd fd) { return le64toh(read<uint64_t>(fd)); }

static std::optional<std::string> get_partuuid_from_harddrive_device_path(auto_fd fd)
{
    auto partition_number = read_le32(fd);
    read_le64(fd); // partition_start
    read_le64(fd); // partition_size

    struct mbr_signature_t {
        uint32_t u32le;
        uint8_t padding[12];
    };

    struct gpt_signature_t {
        uint32_t u32le;
        uint16_t u16le1, u16le2, u16be1, u16be2;
        uint32_t u32be;
    };

    union signature_t {
        mbr_signature_t mbr;
        gpt_signature_t gpt;
    };

    signature_t signature;
    static_assert(sizeof(signature) == 16);

    read(fd, &signature, sizeof(signature));
    read<uint8_t>(fd); // mbrtype
    auto signaturetype = read<uint8_t>(fd);
    if (signaturetype == 1/*mbr*/) {
        char buf[16];
        if (sprintf(buf, "%08x-%02d", le32toh(signature.mbr.u32le), (int)partition_number) < 0) {
            throw std::runtime_error("sprintf() failed");
        }
        //else
        return buf;
    }
    //else
    if (signaturetype == 2/*gpt*/) {
        char buf[40];
        if (sprintf(buf, "%08x-%04x-%04x-%04x-%04x%08x", 
            le32toh(signature.gpt.u32le), le16toh(signature.gpt.u16le1), le16toh(signature.gpt.u16le2),
            be16toh(signature.gpt.u16be1), be16toh(signature.gpt.u16be2), be32toh(signature.gpt.u32be)) < 0) {
            throw std::runtime_error("sprintf() failed");
        }
        //else
        return buf;
    }
    //else
    return {};
}

static std::filesystem::path detect_efi_boot_partition(
    const std::filesystem::path& efivars_dir = "/sys/firmware/efi/efivars")
{
    uint16_t boot_current = [&efivars_dir]() {
        auto_fd fd = open(efivars_dir / "BootCurrent-8be4df61-93ca-11d2-aa0d-00e098032b8c");
        if (!fd) throw std::runtime_error("Cannot access EFI vars(No efivarfs mounted?)"); // no efi firmware?
        read_le32(fd); // variable attributes
        return read_le16(fd); // current boot #
    }();

    char bootvar[80];
    if (sprintf(bootvar, "Boot%04X-8be4df61-93ca-11d2-aa0d-00e098032b8c", boot_current) < 0) {
        throw std::runtime_error("sprintf() failed(how come this could happen?)");
    }
    //else
    auto_fd fd = open(efivars_dir / bootvar);
    if (!fd) throw std::runtime_error("Cannot access EFI boot option " + std::to_string(boot_current));

    read_le32(fd); // variable attributes
    read_le32(fd); // some flags
    read_le16(fd); // length of path list
    while (read_le16(fd) != 0x0000) { ; } // description

    std::optional<std::string> partuuid;
    while(!partuuid) {  // parse device tree until what we're looking for found
        uint8_t type, subtype;
        type = read<uint8_t>(fd);
        subtype = read<uint8_t>(fd);
        if (type == 0x7f/*END_DEVICE_PATH_TYPE*/ && subtype == 0xff/*END_ENTIRE_DEVICE_PATH_SUBTYPE*/)
            break; // reached to the end of device path
        // else
        auto struct_len = read_le16(fd);
        if (struct_len < 4) throw std::runtime_error("Invalid structure(length must not be less than 4)");
        if (type != 0x04/*MEDIA_DEVICE_PATH*/ || subtype != 0x01/*MEDIA_HARDDRIVE_DP*/) {
            ssize_t skip_len = struct_len - 4; 
            uint8_t buf[skip_len];
            read(fd, buf, skip_len); // skip this part
            continue;
        }
        //else
        partuuid = get_partuuid_from_harddrive_device_path(fd);
    }
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
