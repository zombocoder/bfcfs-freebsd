# BFCFS - FreeBSD Kernel Module

[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)
[![FreeBSD](https://img.shields.io/badge/FreeBSD-14.3+-red.svg)](https://www.freebsd.org/)
[![Build Status](https://img.shields.io/github/actions/workflow/status/zombocoder/bfcfs-freebsd/freebsd-build.yml?branch=main)](https://github.com/zombocoder/bfcfs-freebsd/actions)
[![Status](https://img.shields.io/badge/Status-Stable-brightgreen.svg)](#features)

FreeBSD kernel module for mounting BFC (Binary File Container) filesystem images as read-only filesystems.

## Features

- Read-only mounting of BFC container files
- Full support for files, directories, and symlinks
- Hardware-accelerated CRC32C verification (optional)
- Zero-copy reads from backing file

## Requirements

- FreeBSD 14.3 or later
- Kernel sources installed at `/usr/src/sys`
- C compiler (clang)

## Building

```sh
make
```

This will produce `bfcfs.ko` kernel module.

### Prerequisites

Before building, ensure you have:

1. **Kernel sources** installed at `/usr/src/sys`:

   ```sh
   # If kernel sources are not installed, fetch them:
   fetch https://download.freebsd.org/ftp/releases/amd64/14.3-RELEASE/src.txz
   doas tar -C / -xzf src.txz
   ```

2. **Clang compiler** (usually installed by default):
   ```sh
   cc --version
   ```

## Installation

### Load the module temporarily:

```sh
doas kldload ./bfcfs.ko
```

### Load at boot time:

Copy the module to the system modules directory:

```sh
doas cp bfcfs.ko /boot/modules/
```

Add to `/boot/loader.conf`:

```
bfcfs_load="YES"
```

## Usage

### Basic mount:

```sh
doas mount -t bfcfs -o ro /path/to/container.bfc /mnt/bfc
```

### Mount options:

- `ro` - Read-only (required)
- `noverify` - Disable CRC32C verification (faster, less safe)

### Examples:

```sh
# Mount with CRC verification
doas mount -t bfcfs -o ro /home/user/mysite.bfc /mnt/bfc

# Mount without CRC verification (faster)
doas mount -t bfcfs -o ro,noverify /home/user/mysite.bfc /mnt/bfc

# View mounted files
ls -la /mnt/bfc
cat /mnt/bfc/index.html

# Unmount
doas umount /mnt/bfc
```

## Limitations

- **Read-only**: No write support (containers are immutable by design)
- **No compression**: ZSTD-compressed files not yet supported
- **No encryption**: ChaCha20-Poly1305 encrypted files not yet supported
- **Full file reads only**: Partial reads of compressed/encrypted files not supported

## Architecture

The module consists of four main components:

### bfcfs_vfs.c

VFS operations (mount, unmount, root vnode, statfs)

### bfcfs_vnops.c

Vnode operations (lookup, open, read, readdir, getattr, etc.)

### bfc_format.c

BFC format parsing (header, footer, index management)

### bfc_io.c

Low-level I/O routines (pread, object reading, CRC verification)

## File Layout

```
bfcfs-freebsd/
├── .github/
│   └── workflows/
│       └── freebsd-build.yml  # GitHub Actions CI with tests
├── .gitignore                  # Git ignore rules
├── LICENSE                     # BSD 3-Clause License
├── Makefile                    # Build configuration
├── README.md                   # This file
├── bfcfs.h                     # Main header with data structures
├── bfcfs_vfs.c                 # VFS operations
├── bfcfs_vnops.c               # Vnode operations
├── bfc_format.c                # BFC format parsing
├── bfc_io.c                    # I/O routines
└── tests/                      # ATF test suite
    ├── Kyuafile                # Test configuration
    ├── h_funcs.subr            # Test helper functions
    ├── mount_test              # Mount/unmount tests
    ├── read_test               # Read operation tests
    ├── readdir_test            # Directory listing tests
    ├── symlink_test            # Symlink tests
    └── fixtures/               # Test BFC containers
        └── README.md           # Fixture documentation
```

## Debugging

Enable debug output by building with the `DEBUG` flag:

```sh
# Build with debug output enabled
make DEBUG=1

# Build without debug output (default)
make
```

View kernel messages:

```sh
doas dmesg | grep bfcfs
```

## Performance

- Uses FreeBSD's hardware-accelerated CRC32C (SSE 4.2 on x86_64)
- Direct VOP_READ from backing file (zero-copy when possible)
- Read statistics available via mount structure

## Troubleshooting

### Module fails to load

```
kldload: can't load ./bfcfs.ko: Operation not permitted
```

**Solution**: Use `doas` or `su root` - kernel modules require root privileges.

### Mount fails with "invalid magic"

```
bfcfs: invalid magic: expected 'BFCFv1', got '...'
```

**Solution**: Ensure the file is a valid BFC container. Check with:

```sh
hexdump -C container.bfc | head -1
# Should show: 00000000  42 46 43 46 76 31 00 00  ...
```

### CRC mismatch errors

```
bfcfs: CRC mismatch for file.txt: expected 0x..., got 0x...
```

**Solution**: Container may be corrupted. Try mounting with `noverify` option to bypass CRC checks, or regenerate the container.

### Kernel panic on unmount

**Solution**: This should not happen in the current version. If it does:

1. Check `doas dmesg` for backtrace
2. Ensure you're using the latest module version
3. Report the issue with the crash dump from `/var/crash/`

## Development

### Rebuilding after changes:

```sh
make clean
make
doas kldunload bfcfs
doas kldload ./bfcfs.ko
```

### Testing:

The module includes an ATF-based test suite using Kyua:

```sh
# Install test dependencies
doas pkg install kyua

# Load the module
doas kldload ./bfcfs.ko

# Run the test suite
cd tests
doas kyua test

# View test results
kyua report

# Detailed report
kyua report --verbose
```

#### Manual Testing:

```sh
# Create a test mount point
doas mkdir -p /mnt/bfc

# Mount test container
doas mount -t bfcfs -o ro /path/to/test.bfc /mnt/bfc

# Run manual tests
ls -la /mnt/bfc
cat /mnt/bfc/somefile.txt
find /mnt/bfc -type f

# Unmount
doas umount /mnt/bfc
```

### Test Suite Structure:

```
tests/
├── Kyuafile              # Test configuration
├── h_funcs.subr          # Helper functions
├── mount_test            # Mount/unmount tests
├── read_test             # Read operations tests
├── readdir_test          # Directory listing tests
├── symlink_test          # Symlink support tests
└── fixtures/
    ├── README.md         # Fixture documentation
    └── test.bfc          # Test BFC container
```

## License

BSD 3-Clause License - See [LICENSE](LICENSE) file for details

## What is BFC?

BFC (Binary File Container) is a modern container format designed for embedding and serving static assets. Key features include:

- **Immutable**: Read-only by design, perfect for distributing static content
- **Efficient**: Zero-copy reads, hardware-accelerated checksums
- **Flexible**: Optional compression (ZSTD) and encryption (ChaCha20-Poly1305)
- **Cross-platform**: Containers work identically on Linux and FreeBSD
- **Simple**: Single-file containers with embedded index

### Use Cases

- **Static Website Serving**: Embed entire websites in a single `.bfc` file
- **Application Assets**: Bundle application resources (images, configs, data)
- **Software Distribution**: Immutable, verifiable software packages
- **Embedded Systems**: Compact, read-only filesystem for embedded devices

## BFC Format Overview

A BFC container consists of:

1. **Header** (4096 bytes): Magic signature, features, UUID, encryption salt
2. **Objects**: File and directory data with individual headers
3. **Index**: Fast lookup table mapping paths to object offsets
4. **Footer** (56 bytes): Index location and container checksums

Each object includes:

- Object type (file/directory/symlink)
- Compression and encryption metadata
- POSIX mode bits and timestamps
- CRC32C checksum for integrity
- Variable-length name and content

## Creating BFC Containers

While this module is for mounting containers, you can create them using:

### BFC CLI Tool (Recommended)

```sh
# Install bfc tool (example)
pkg install bfc-container  # or build from source

# Create uncompressed container
bfc create mysite.bfc /var/www/html/

# Create with compression
bfc create -c zstd mysite.bfc /var/www/html/

# Create with encryption
bfc create -e chacha20-poly1305 -k mykey secure.bfc /data/
```

### Example: Serving a Static Website

```sh
# Create BFC container from website
bfc create website.bfc /var/www/mysite/

# Copy to server
scp website.bfc server:/var/containers/

# On server: load module
doas kldload bfcfs

# Mount and serve
doas mount -t bfcfs -o ro /var/containers/website.bfc /var/www/html

# Configure web server to serve from /var/www/html
# (nginx, apache, etc. will serve files from the mounted BFC)
```

## Comparison with Other Filesystems

| Feature        | BFCFS | SquashFS | ISO9660 | FUSE   |
| -------------- | ----- | -------- | ------- | ------ |
| Read-only      | [x]   | [x]      | [x]     | [-]    |
| Kernel module  | [x]   | [x]      | [x]     | [-]    |
| Compression    | [ ]   | [x]      | [-]     | Varies |
| Encryption     | [ ]   | [-]      | [-]     | Varies |
| CRC integrity  | [x]   | [x]      | [-]     | Varies |
| Cross-platform | [x]   | [x]      | [x]     | [x]    |
| Performance    | High  | High     | High    | Medium |

## Project Status

**Current Version**: 1.0.0 (Stable)

### Implemented Features

- [x] Mount/unmount operations
- [x] Directory listing and navigation
- [x] File reading (uncompressed)
- [x] Symlink support
- [x] CRC32C verification
- [x] Hardware-accelerated checksums

### Planned Features (Roadmap)

- [ ] ZSTD decompression support
- [ ] ChaCha20-Poly1305 decryption support
- [ ] Extended attributes
- [ ] Advanced mount options
- [ ] Performance optimizations

## Contributing

Contributions are welcome! Please:

1. **Fork the repository** and create a feature branch
2. **Follow FreeBSD style(9)** coding standards
3. **Test thoroughly** on FreeBSD 14.3+
4. **Update documentation** as needed
5. **Submit a pull request** with clear description

### Development Guidelines

- Follow [FreeBSD style(9)](https://man.freebsd.org/cgi/man.cgi?query=style&sektion=9)
- Use tabs for indentation (not spaces)
- Keep lines under 80 characters
- Add comments for complex logic
- Test with different container types

### Reporting Issues

Please report bugs and feature requests via [GitHub Issues](https://github.com/zombocoder/bfcfs-freebsd/issues).

Include:

- FreeBSD version (`freebsd-version`)
- Kernel version (`uname -a`)
- Steps to reproduce
- Error messages from `dmesg`
- Sample BFC container (if applicable)

## Acknowledgments

- FreeBSD kernel developers for excellent VFS documentation
- BFC format specification contributors
- Early testers and adopters

## Support

- **Documentation**: This README and inline code comments
- **Issues**: [GitHub Issues](https://github.com/zombocoder/bfcfs-freebsd/issues)
- **Discussions**: [GitHub Discussions](https://github.com/zombocoder/bfcfs-freebsd/discussions)

## Authors

- **Taras Havryliak** (zombocoder) - Initial implementation
- Contributors - See [CONTRIBUTORS](CONTRIBUTORS) file
