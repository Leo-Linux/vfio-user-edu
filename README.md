# vfio-user EDU PCIe Device

A standalone implementation of the QEMU EDU educational PCI device using
**libvfio-user**. This runs as an independent user-space process and connects
to QEMU via the vfio-user protocol over a UNIX socket.

## Architecture

```
┌─────────────────────────────┐          UNIX Socket
│        QEMU (VMM)           │    ◄──── vfio-user ────►  ┌──────────────────────┐
│  ┌───────────────────────┐  │         protocol           │  edu_device (this)   │
│  │ vfio-user-pci driver  │  │                            │                      │
│  └───────────────────────┘  │                            │  BAR0 MMIO handler   │
│            ↕                │                            │  IRQ controller      │
│  ┌───────────────────────┐  │                            │  DMA engine          │
│  │     Guest VM          │  │                            │  Factorial thread    │
│  │  (Linux + edu driver) │  │                            │                      │
│  └───────────────────────┘  │                            └──────────────────────┘
└─────────────────────────────┘
```

## Features

| Feature | Description |
|---------|-------------|
| **Identification** | Register at 0x00 returns `0x010000edu` (v1.0) |
| **Liveness check** | Write to 0x04, read back bitwise-inverted value |
| **Factorial** | Write N to 0x08, result computed asynchronously |
| **Interrupts** | INTx and MSI support via raise/ack registers |
| **DMA** | Bidirectional DMA with 4KB internal buffer |
| **PCIe** | PCI Express endpoint with PM and MSI capabilities |

## Register Map (BAR0, 1MB)

| Offset | Size | Access | Description |
|--------|------|--------|-------------|
| 0x00 | 4 | RO | Device ID: `0x010000edu` |
| 0x04 | 4 | RW | Liveness: returns `~value` |
| 0x08 | 4 | RW | Factorial computation |
| 0x20 | 4 | RW | Status (bit0=computing, bit7=IRQ on completion) |
| 0x24 | 4 | RO | Interrupt status |
| 0x60 | 4 | WO | Interrupt raise |
| 0x64 | 4 | WO | Interrupt acknowledge |
| 0x80 | 8 | RW | DMA source address |
| 0x88 | 8 | RW | DMA destination address |
| 0x90 | 8 | RW | DMA transfer count |
| 0x98 | 8 | RW | DMA command register |
| 0x40000 | 4096 | RW | DMA buffer |

## PCI Configuration Space

| Offset | Capability | Description |
|--------|------------|-------------|
| 0x40 | MSI | 1 vector, 64-bit address |
| auto | PM | Power Management |
| auto | PCIe | PCI Express endpoint |

PCI ID: `1234:11e8` (vendor: 0x1234, device: 0x11e8, revision: 0x10)

## Project Structure

```
vfio-user-edu/
├── edu_device.c     # Main device implementation
├── Makefile         # GNU Make build
├── CMakeLists.txt   # CMake build
└── README.md        # This file
```

## Prerequisites

- Linux x86_64
- [libvfio-user](https://github.com/nutanix/libvfio-user) built or installed
- QEMU 10.1+ with vfio-user client support
- GCC, Make or CMake

### Building libvfio-user

```bash
sudo apt install libjson-c-dev libcmocka-dev meson ninja-build

git clone https://github.com/nutanix/libvfio-user.git
cd libvfio-user
meson build
ninja -C build

# Optional: install to system paths
sudo ninja -C build install
sudo ldconfig
```

## Building

### Using Make

**If libvfio-user is installed to system paths** (after `sudo ninja -C build install`):

```bash
make
```

**If using a local source tree** (recommended for development, no install needed):

```bash
make VFIO_USER_SRCDIR=/path/to/libvfio-user
```

For example:

```bash
make VFIO_USER_SRCDIR=/home/leo/leo/workspace/vfio-user/libvfio-user
```

`VFIO_USER_SRCDIR` automatically derives the include and lib paths. You can also specify them individually:

```bash
make VFIO_USER_INCDIR=/path/to/libvfio-user/include \
     VFIO_USER_LIBDIR=/path/to/libvfio-user/build/lib
```

**Verify paths are correct:**

```bash
make check-lib VFIO_USER_SRCDIR=/path/to/libvfio-user
```

### Using CMake

```bash
mkdir build && cd build
cmake -DVFIO_USER_PREFIX=/path/to/libvfio-user/install ..
make
```

## Running

### 1. Start the EDU device server

```bash
./edu_device -s /tmp/edu.sock
```

### 2. Start QEMU with the vfio-user device

> **Note**: The server must be started before QEMU. Guest memory must use a `share=on` file backend.

```bash
qemu-system-x86_64 \
    -machine accel=kvm,type=q35 \
    -cpu host -m 2G \
    -object memory-backend-file,id=mem,size=2G,mem-path=/dev/shm/qemu-mem,share=on \
    -numa node,memdev=mem \
    -drive if=virtio,format=qcow2,file=your-disk.img \
    -device '{"driver":"vfio-user-pci","socket":{"path":"/tmp/edu.sock","type":"unix"}}' \
    -nographic
```

### 3. Verify the device in the guest VM

```bash
# Should show device 1234:11e8
lspci -nn | grep 1234

# Check detailed info, should show MSI capability at [40]
lspci -vvs <BDF>
```

## Command-line Options

| Option | Default | Description |
|--------|---------|-------------|
| `-s, --socket PATH` | `/tmp/edu.sock` | UNIX socket path |
| `-d, --dma-mask N` | `28` | DMA address bits (default 256MB) |
| `-v, --verbose` | off | Verbose logging |
| `-h, --help` | | Show help |

## Testing with a Guest Driver

Basic tests from the guest using `/dev/mem` or a custom driver:

```bash
# Read device ID at BAR0 + 0x00
# Should return 0x010000ed

# Liveness check: write 0x12345678 to BAR0 + 0x04
# Read back from BAR0 + 0x04: should get 0xedcba987 (~0x12345678)

# Factorial: write 5 to BAR0 + 0x08
# Wait for status bit 0 to clear at BAR0 + 0x20
# Read BAR0 + 0x08: should get 120 (5!)
```

An open-source Linux kernel driver is available at:
[maxerenberg/qemu-edu-driver](https://github.com/maxerenberg/qemu-edu-driver)

## Comparison with QEMU Internal edu.c

| Aspect | QEMU internal edu.c | This vfio-user edu |
|--------|---------------------|-------------------|
| Process | Runs inside QEMU | Standalone process |
| MMIO | `MemoryRegionOps` callbacks | `vfu_setup_region()` callback |
| Interrupts | `pci_set_irq()` / `msi_notify()` | `vfu_irq_trigger()` |
| DMA | `pci_dma_read/write()` | `vfu_sgl_read/write()` |
| PCI config | QEMU PCI subsystem | `vfu_pci_init/set_id/...` |
| Event loop | QEMU main loop | `vfu_run_ctx()` loop |
| Object model | QOM type system | Plain C struct |

## License

BSD-3-Clause
