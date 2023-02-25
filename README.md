# detect_efi_boot_partition
A command line tool to find EFI boot partition and print its device name

## Prerequisites

### Runtime

- Linux kernel with CONFIG_EFI_VARS (runtime)
- System have booted via EFI(not BIOS)
- [util-linux](https://github.com/util-linux/util-linux)

### Build time

- [argparse](https://github.com/p-ranav/argparse)
- gcc >= (probably)7.1

## How to build

```sh
make
```

## Usage

```
Usage: ./detect_efi_boot_partition [-h] [--quiet]

Optional arguments:
  -h, --help    shows help message and exits
  -v, --version prints version information and exits
  -q, --quiet   Don't show error message
```

## Example

```
# ./detect_efi_boot_partition
/dev/nvme0n1p1
```
