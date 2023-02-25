all: detect_efi_boot_partition

detect_efi_boot_partition: detect_efi_boot_partition.cpp
	g++ -std=c++17 -Wall -o $@ $^ -lblkid

clean:
	rm -f detect_efi_boot_partition
