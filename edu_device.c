/*
 * EDU PCIe Device Implementation using libvfio-user
 *
 * This implements the QEMU EDU educational PCI device as a standalone
 * vfio-user server process. It can be connected to QEMU via:
 *   -device vfio-user-pci,socket=/tmp/edu.sock
 *
 * The EDU device provides:
 *   - Device identification register (RO)
 *   - Liveness check register (value inversion)
 *   - Factorial computation (async, with interrupt)
 *   - IRQ controller (INTx + MSI)
 *   - DMA controller (read/write with 4KB internal buffer)
 *
 * Register map (BAR0, 1MB):
 *   0x00  (RO)  Identification: 0x010000edu
 *   0x04  (RW)  Liveness check: ~value
 *   0x08  (RW)  Factorial computation
 *   0x20  (RW)  Status register
 *   0x24  (RO)  Interrupt status
 *   0x60  (WO)  Interrupt raise
 *   0x64  (WO)  Interrupt acknowledge
 *   0x80  (RW)  DMA source address (64-bit)
 *   0x88  (RW)  DMA destination address (64-bit)
 *   0x90  (RW)  DMA transfer count (64-bit)
 *   0x98  (RW)  DMA command register (64-bit)
 *   0x40000-0x40FFF  DMA buffer (4KB)
 *
 * Copyright (c) 2026. Educational use. BSD-3-Clause.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/param.h>
#include <sys/mman.h>

#include "libvfio-user.h"
#include "pci_defs.h"

/* ================================================================
 * EDU Device Constants
 * ================================================================ */

#define EDU_VENDOR_ID       0x1234
#define EDU_DEVICE_ID       0x11e8
#define EDU_REVISION        0x10
#define EDU_SUBSYS_VENDOR   0x0000
#define EDU_SUBSYS_ID       0x0000

#define EDU_BAR0_SIZE       (1U << 20)   /* 1 MB */

/* Register offsets */
#define EDU_REG_ID          0x00    /* Identification (RO) */
#define EDU_REG_ALIVE       0x04    /* Liveness check (RW) */
#define EDU_REG_FACTORIAL   0x08    /* Factorial (RW) */
#define EDU_REG_STATUS      0x20    /* Status (RW) */
#define EDU_REG_IRQ_STATUS  0x24    /* Interrupt status (RO) */
#define EDU_REG_IRQ_RAISE   0x60    /* Interrupt raise (WO) */
#define EDU_REG_IRQ_ACK     0x64    /* Interrupt acknowledge (WO) */
#define EDU_REG_DMA_SRC     0x80    /* DMA source address */
#define EDU_REG_DMA_DST     0x88    /* DMA destination address */
#define EDU_REG_DMA_CNT     0x90    /* DMA transfer count */
#define EDU_REG_DMA_CMD     0x98    /* DMA command register */

/* Status bits */
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80

/* DMA command bits */
#define EDU_DMA_RUN         0x01
#define EDU_DMA_DIR_MASK    0x02
#define EDU_DMA_FROM_PCI    0       /* RAM -> device */
#define EDU_DMA_TO_PCI      1       /* device -> RAM */
#define EDU_DMA_IRQ         0x04

#define EDU_DMA_DIR(cmd)    (((cmd) & EDU_DMA_DIR_MASK) >> 1)

/* DMA buffer */
#define DMA_BUF_OFFSET      0x40000
#define DMA_BUF_SIZE        4096

/* IRQ value for DMA completion */
#define DMA_IRQ_VALUE       0x00000100

/* Factorial completion IRQ value */
#define FACT_IRQ_VALUE      0x00000001

/* Identification value: major=1, minor=0 */
#define EDU_ID_VALUE        0x010000edu

/* DMA address mask (28 bits by default) */
#define EDU_DMA_MASK_DEFAULT  ((1ULL << 28) - 1)

/* ================================================================
 * EDU Device State
 * ================================================================ */

typedef struct {
    vfu_ctx_t *vfu_ctx;

    /* Registers */
    uint32_t alive;             /* 0x04: liveness check */
    uint32_t factorial;         /* 0x08: factorial value */
    uint32_t status;            /* 0x20: status register */
    uint32_t irq_status;        /* 0x24: interrupt status */

    /* DMA state */
    uint64_t dma_src;           /* 0x80: DMA source */
    uint64_t dma_dst;           /* 0x88: DMA destination */
    uint64_t dma_cnt;           /* 0x90: DMA count */
    uint64_t dma_cmd;           /* 0x98: DMA command */
    uint8_t  dma_buf[DMA_BUF_SIZE]; /* Internal DMA buffer */
    uint64_t dma_mask;          /* DMA address mask */

    /* Factorial thread */
    pthread_t fact_thread;
    pthread_mutex_t fact_mutex;
    pthread_cond_t fact_cond;
    bool fact_pending;          /* New factorial request */
    bool stopping;              /* Shutdown signal */
} edu_device_t;

static volatile bool g_running = true;

/* ================================================================
 * Interrupt Handling
 * ================================================================ */

static void edu_raise_irq(edu_device_t *edu, uint32_t val)
{
    edu->irq_status |= val;
    if (edu->irq_status) {
        int ret = vfu_irq_trigger(edu->vfu_ctx, 0);
        if (ret != 0) {
            fprintf(stderr, "edu: failed to trigger IRQ: %s\n",
                    strerror(errno));
        }
    }
}

static void edu_lower_irq(edu_device_t *edu, uint32_t val)
{
    edu->irq_status &= ~val;
    /* INTx de-assertion would happen here if needed;
     * with MSI, each trigger is edge-triggered */
}

/* ================================================================
 * Factorial Computation Thread
 * ================================================================ */

static uint32_t compute_factorial(uint32_t n)
{
    uint32_t result = 1;
    for (uint32_t i = 2; i <= n && i <= 12; i++) {
        result *= i;
    }
    return result;
}

static void *edu_fact_thread(void *arg)
{
    edu_device_t *edu = (edu_device_t *)arg;

    pthread_mutex_lock(&edu->fact_mutex);
    while (!edu->stopping) {
        while (!edu->fact_pending && !edu->stopping) {
            pthread_cond_wait(&edu->fact_cond, &edu->fact_mutex);
        }
        if (edu->stopping) {
            break;
        }

        edu->fact_pending = false;
        uint32_t n = edu->factorial;
        pthread_mutex_unlock(&edu->fact_mutex);

        /* Simulate computation delay */
        usleep(10000); /* 10ms */

        uint32_t result = compute_factorial(n);

        pthread_mutex_lock(&edu->fact_mutex);
        edu->factorial = result;
        __atomic_and_fetch(&edu->status, ~EDU_STATUS_COMPUTING,
                           __ATOMIC_SEQ_CST);

        /* Raise IRQ if requested */
        if (__atomic_load_n(&edu->status, __ATOMIC_SEQ_CST)
            & EDU_STATUS_IRQFACT) {
            edu_raise_irq(edu, FACT_IRQ_VALUE);
        }

        printf("edu: factorial(%u) = %u\n", n, result);
    }
    pthread_mutex_unlock(&edu->fact_mutex);

    return NULL;
}

/* ================================================================
 * DMA Engine
 * ================================================================ */

static uint64_t edu_clamp_addr(edu_device_t *edu, uint64_t addr)
{
    return addr & edu->dma_mask;
}

static bool edu_check_dma_range(uint64_t addr, uint64_t cnt)
{
    /* Check that the device-side address falls within the DMA buffer */
    if (addr < DMA_BUF_OFFSET) {
        return false;
    }
    if (addr + cnt > DMA_BUF_OFFSET + DMA_BUF_SIZE) {
        return false;
    }
    return true;
}

static void edu_dma_execute(edu_device_t *edu)
{
    if (!(edu->dma_cmd & EDU_DMA_RUN)) {
        return;
    }

    if (EDU_DMA_DIR(edu->dma_cmd) == EDU_DMA_FROM_PCI) {
        /* Direction: host RAM -> device DMA buffer */
        uint64_t dst = edu->dma_dst;
        if (!edu_check_dma_range(dst, edu->dma_cnt)) {
            fprintf(stderr, "edu: DMA FROM_PCI: dst 0x%lx cnt %lu out of range\n",
                    (unsigned long)dst, (unsigned long)edu->dma_cnt);
            goto done;
        }
        dst -= DMA_BUF_OFFSET;

        /* Read from guest memory via vfu_sgl API */
        dma_sg_t *sg = calloc(1, dma_sg_size());
        if (!sg) {
            goto done;
        }
        uint64_t src = edu_clamp_addr(edu, edu->dma_src);
        int ret = vfu_addr_to_sgl(edu->vfu_ctx,
                                  (void *)(uintptr_t)src,
                                  edu->dma_cnt, sg, 1, PROT_READ);
        if (ret < 0) {
            fprintf(stderr, "edu: DMA read: addr_to_sgl failed: %s\n",
                    strerror(errno));
            free(sg);
            goto done;
        }
        ret = vfu_sgl_read(edu->vfu_ctx, sg, 1,
                           edu->dma_buf + dst);
        if (ret != 0) {
            fprintf(stderr, "edu: DMA read: sgl_read failed: %s\n",
                    strerror(errno));
        } else {
            printf("edu: DMA FROM_PCI: 0x%lx -> buf+0x%lx, %lu bytes\n",
                   (unsigned long)src, (unsigned long)dst,
                   (unsigned long)edu->dma_cnt);
        }
        free(sg);
    } else {
        /* Direction: device DMA buffer -> host RAM */
        uint64_t src = edu->dma_src;
        if (!edu_check_dma_range(src, edu->dma_cnt)) {
            fprintf(stderr, "edu: DMA TO_PCI: src 0x%lx cnt %lu out of range\n",
                    (unsigned long)src, (unsigned long)edu->dma_cnt);
            goto done;
        }
        src -= DMA_BUF_OFFSET;

        dma_sg_t *sg = calloc(1, dma_sg_size());
        if (!sg) {
            goto done;
        }
        uint64_t dst = edu_clamp_addr(edu, edu->dma_dst);
        int ret = vfu_addr_to_sgl(edu->vfu_ctx,
                                  (void *)(uintptr_t)dst,
                                  edu->dma_cnt, sg, 1, PROT_WRITE);
        if (ret < 0) {
            fprintf(stderr, "edu: DMA write: addr_to_sgl failed: %s\n",
                    strerror(errno));
            free(sg);
            goto done;
        }
        ret = vfu_sgl_write(edu->vfu_ctx, sg, 1,
                            edu->dma_buf + src);
        if (ret != 0) {
            fprintf(stderr, "edu: DMA write: sgl_write failed: %s\n",
                    strerror(errno));
        } else {
            printf("edu: DMA TO_PCI: buf+0x%lx -> 0x%lx, %lu bytes\n",
                   (unsigned long)src, (unsigned long)dst,
                   (unsigned long)edu->dma_cnt);
        }
        free(sg);
    }

done:
    edu->dma_cmd &= ~EDU_DMA_RUN;

    if (edu->dma_cmd & EDU_DMA_IRQ) {
        edu_raise_irq(edu, DMA_IRQ_VALUE);
    }
}

/* ================================================================
 * BAR0 MMIO Access Callback
 * ================================================================ */

static ssize_t edu_bar0_access(vfu_ctx_t *vfu_ctx, char *buf,
                               size_t count, loff_t offset,
                               bool is_write)
{
    edu_device_t *edu = vfu_get_private(vfu_ctx);
    uint32_t val32 = 0;
    uint64_t val64 = 0;

    /*
     * Access to DMA buffer region (0x40000 - 0x40FFF)
     */
    if (offset >= DMA_BUF_OFFSET &&
        offset + count <= DMA_BUF_OFFSET + DMA_BUF_SIZE) {
        if (is_write) {
            memcpy(edu->dma_buf + (offset - DMA_BUF_OFFSET), buf, count);
        } else {
            memcpy(buf, edu->dma_buf + (offset - DMA_BUF_OFFSET), count);
        }
        return count;
    }

    /*
     * Register access: offsets < 0x80 require size==4,
     * offsets >= 0x80 allow size==4 or size==8.
     */
    if (offset < 0x80 && count != 4) {
        errno = EINVAL;
        return -1;
    }
    if (offset >= 0x80 && count != 4 && count != 8) {
        errno = EINVAL;
        return -1;
    }

    if (is_write) {
        if (count == 4) {
            memcpy(&val32, buf, 4);
        } else {
            memcpy(&val64, buf, 8);
        }

        switch (offset) {
        case EDU_REG_ALIVE:
            edu->alive = ~val32;
            break;

        case EDU_REG_FACTORIAL:
            pthread_mutex_lock(&edu->fact_mutex);
            if (!(__atomic_load_n(&edu->status, __ATOMIC_SEQ_CST)
                  & EDU_STATUS_COMPUTING)) {
                edu->factorial = val32;
                __atomic_or_fetch(&edu->status, EDU_STATUS_COMPUTING,
                                  __ATOMIC_SEQ_CST);
                edu->fact_pending = true;
                pthread_cond_signal(&edu->fact_cond);
            }
            pthread_mutex_unlock(&edu->fact_mutex);
            break;

        case EDU_REG_STATUS:
            if (val32 & EDU_STATUS_IRQFACT) {
                __atomic_or_fetch(&edu->status, EDU_STATUS_IRQFACT,
                                  __ATOMIC_SEQ_CST);
            } else {
                __atomic_and_fetch(&edu->status, ~EDU_STATUS_IRQFACT,
                                   __ATOMIC_SEQ_CST);
            }
            break;

        case EDU_REG_IRQ_RAISE:
            edu_raise_irq(edu, val32);
            break;

        case EDU_REG_IRQ_ACK:
            edu_lower_irq(edu, val32);
            break;

        case EDU_REG_DMA_SRC:
            if (count == 8) edu->dma_src = val64;
            else edu->dma_src = (edu->dma_src & 0xFFFFFFFF00000000ULL) | val32;
            break;
        case EDU_REG_DMA_SRC + 4:
            edu->dma_src = (edu->dma_src & 0x00000000FFFFFFFFULL)
                           | ((uint64_t)val32 << 32);
            break;

        case EDU_REG_DMA_DST:
            if (count == 8) edu->dma_dst = val64;
            else edu->dma_dst = (edu->dma_dst & 0xFFFFFFFF00000000ULL) | val32;
            break;
        case EDU_REG_DMA_DST + 4:
            edu->dma_dst = (edu->dma_dst & 0x00000000FFFFFFFFULL)
                           | ((uint64_t)val32 << 32);
            break;

        case EDU_REG_DMA_CNT:
            if (count == 8) edu->dma_cnt = val64;
            else edu->dma_cnt = (edu->dma_cnt & 0xFFFFFFFF00000000ULL) | val32;
            break;
        case EDU_REG_DMA_CNT + 4:
            edu->dma_cnt = (edu->dma_cnt & 0x00000000FFFFFFFFULL)
                           | ((uint64_t)val32 << 32);
            break;

        case EDU_REG_DMA_CMD:
            if (count == 8) edu->dma_cmd = val64;
            else edu->dma_cmd = val32;
            if (edu->dma_cmd & EDU_DMA_RUN) {
                edu_dma_execute(edu);
            }
            break;
        case EDU_REG_DMA_CMD + 4:
            edu->dma_cmd = (edu->dma_cmd & 0x00000000FFFFFFFFULL)
                           | ((uint64_t)val32 << 32);
            break;

        default:
            /* Ignore writes to unknown registers */
            break;
        }
    } else {
        /* Read */
        switch (offset) {
        case EDU_REG_ID:
            val32 = EDU_ID_VALUE;
            break;
        case EDU_REG_ALIVE:
            val32 = edu->alive;
            break;
        case EDU_REG_FACTORIAL:
            pthread_mutex_lock(&edu->fact_mutex);
            val32 = edu->factorial;
            pthread_mutex_unlock(&edu->fact_mutex);
            break;
        case EDU_REG_STATUS:
            val32 = __atomic_load_n(&edu->status, __ATOMIC_SEQ_CST);
            break;
        case EDU_REG_IRQ_STATUS:
            val32 = edu->irq_status;
            break;

        case EDU_REG_DMA_SRC:
            if (count == 8) { val64 = edu->dma_src; goto ret64; }
            val32 = (uint32_t)edu->dma_src;
            break;
        case EDU_REG_DMA_SRC + 4:
            val32 = (uint32_t)(edu->dma_src >> 32);
            break;

        case EDU_REG_DMA_DST:
            if (count == 8) { val64 = edu->dma_dst; goto ret64; }
            val32 = (uint32_t)edu->dma_dst;
            break;
        case EDU_REG_DMA_DST + 4:
            val32 = (uint32_t)(edu->dma_dst >> 32);
            break;

        case EDU_REG_DMA_CNT:
            if (count == 8) { val64 = edu->dma_cnt; goto ret64; }
            val32 = (uint32_t)edu->dma_cnt;
            break;
        case EDU_REG_DMA_CNT + 4:
            val32 = (uint32_t)(edu->dma_cnt >> 32);
            break;

        case EDU_REG_DMA_CMD:
            if (count == 8) { val64 = edu->dma_cmd; goto ret64; }
            val32 = (uint32_t)edu->dma_cmd;
            break;
        case EDU_REG_DMA_CMD + 4:
            val32 = (uint32_t)(edu->dma_cmd >> 32);
            break;

        default:
            val32 = 0;
            break;
        }

        if (count == 4) {
            memcpy(buf, &val32, 4);
        } else {
            memcpy(buf, &val32, 4);
            memset(buf + 4, 0, count - 4);
        }
    }

    return count;

ret64:
    memcpy(buf, &val64, 8);
    return count;
}

/* ================================================================
 * DMA region callbacks (guest memory map/unmap notifications)
 * ================================================================ */

static void edu_dma_register(vfu_ctx_t *vfu_ctx __attribute__((unused)),
                             vfu_dma_info_t *info)
{
    fprintf(stderr, "edu: DMA region registered: iova=%p len=%zu\n",
            info->iova.iov_base, info->iova.iov_len);
}

static void edu_dma_unregister(vfu_ctx_t *vfu_ctx __attribute__((unused)),
                               vfu_dma_info_t *info)
{
    fprintf(stderr, "edu: DMA region unregistered: iova=%p len=%zu\n",
            info->iova.iov_base, info->iova.iov_len);
}

/* ================================================================
 * Device Reset Callback
 * ================================================================ */

static int edu_device_reset(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
    edu_device_t *edu = vfu_get_private(vfu_ctx);

    fprintf(stderr, "edu: device reset (type=%d)\n", type);

    edu->alive = 0;
    edu->factorial = 0;
    __atomic_store_n(&edu->status, 0, __ATOMIC_SEQ_CST);
    edu->irq_status = 0;
    edu->dma_src = 0;
    edu->dma_dst = 0;
    edu->dma_cnt = 0;
    edu->dma_cmd = 0;
    memset(edu->dma_buf, 0, DMA_BUF_SIZE);

    return 0;
}

/* ================================================================
 * Signal Handler
 * ================================================================ */

static void sigint_handler(int signum)
{
    (void)signum;
    g_running = false;
}

/* ================================================================
 * Usage
 * ================================================================ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -s, --socket PATH    Socket path (default: /tmp/edu.sock)\n"
        "  -d, --dma-mask BITS  DMA address bits (default: 28)\n"
        "  -v, --verbose        Verbose output\n"
        "  -h, --help           Show this help\n"
        "\n"
        "Example:\n"
        "  %s -s /tmp/edu.sock\n"
        "\n"
        "Then in QEMU:\n"
        "  qemu-system-x86_64 ... \\\n"
        "    -device vfio-user-pci,socket=/tmp/edu.sock\n",
        prog, prog);
}

/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char *argv[])
{
    const char *socket_path = "/tmp/edu.sock";
    int dma_bits = 28;
    bool verbose = false;
    (void)verbose;
    int ret;

    /* Parse arguments */
    static struct option long_opts[] = {
        { "socket",   required_argument, NULL, 's' },
        { "dma-mask", required_argument, NULL, 'd' },
        { "verbose",  no_argument,       NULL, 'v' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:d:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': socket_path = optarg; break;
        case 'd': dma_bits = atoi(optarg); break;
        case 'v': verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Set up signal handler */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Allocate device state */
    edu_device_t *edu = calloc(1, sizeof(edu_device_t));
    if (!edu) {
        fprintf(stderr, "edu: failed to allocate device state\n");
        return 1;
    }
    edu->dma_mask = (1ULL << dma_bits) - 1;

    printf("=== EDU PCIe Device (vfio-user) ===\n");
    printf("Socket:   %s\n", socket_path);
    printf("DMA mask: 0x%lx (%d bits)\n",
           (unsigned long)edu->dma_mask, dma_bits);

    /*
     * Step 1: Create vfio-user context
     */
    edu->vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, socket_path,
                                  LIBVFIO_USER_FLAG_ATTACH_NB,
                                  edu, VFU_DEV_TYPE_PCI);
    if (edu->vfu_ctx == NULL) {
        fprintf(stderr, "edu: failed to create vfu context: %s\n",
                strerror(errno));
        free(edu);
        return 1;
    }

    /*
     * Step 2: Initialize PCI configuration space
     */
    ret = vfu_pci_init(edu->vfu_ctx, VFU_PCI_TYPE_EXPRESS,
                       PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        fprintf(stderr, "edu: vfu_pci_init failed: %s\n", strerror(errno));
        goto cleanup;
    }

    vfu_pci_set_id(edu->vfu_ctx, EDU_VENDOR_ID, EDU_DEVICE_ID,
                   EDU_SUBSYS_VENDOR, EDU_SUBSYS_ID);
    vfu_pci_set_class(edu->vfu_ctx, 0x00, 0xFF, 0x00);

    /*
     * Step 3: Add PCI capabilities
     */

    /* MSI capability at offset 0x40 (matches QEMU internal EDU layout) */
    struct msicap msi = {
        .hdr.id = PCI_CAP_ID_MSI,
        .mc.msie = 0,       /* guest driver will enable */
        .mc.mmc = 0,        /* encoding 0 = 1 vector */
        .mc.c64 = 1,        /* support 64-bit address */
    };
    ret = vfu_pci_add_capability(edu->vfu_ctx, 0x40, 0, &msi);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to add MSI capability: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /* Power Management capability */
    struct pmcap pm = {
        .hdr.id = PCI_CAP_ID_PM,
        .pmcs.nsfrst = 0x1,
    };
    ret = vfu_pci_add_capability(edu->vfu_ctx, 0, 0, &pm);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to add PM capability: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /* PCI Express capability */
    struct pxcap px = {
        .hdr.id = PCI_CAP_ID_EXP,
        .pxcaps.ver = 0x2,
        .pxdcap = { .rer = 0x1, .flrc = 0x1 },
        .pxdcap2.ctds = 0x1,
    };
    ret = vfu_pci_add_capability(edu->vfu_ctx, 0, 0, &px);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to add PCIe capability: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /*
     * Step 4: Setup BAR0 — MMIO region (1MB)
     */
    ret = vfu_setup_region(edu->vfu_ctx,
                           VFU_PCI_DEV_BAR0_REGION_IDX,
                           EDU_BAR0_SIZE,
                           edu_bar0_access,
                           VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
                           NULL, 0, -1, 0);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to setup BAR0: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /*
     * Step 5: Setup interrupts (INTx)
     */
    ret = vfu_setup_device_nr_irqs(edu->vfu_ctx, VFU_DEV_INTX_IRQ, 1);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to setup INTx: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /* Also support MSI (1 vector) */
    ret = vfu_setup_device_nr_irqs(edu->vfu_ctx, VFU_DEV_MSI_IRQ, 1);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to setup MSI: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /*
     * Step 6: Setup DMA callbacks
     */
    ret = vfu_setup_device_dma(edu->vfu_ctx,
                               edu_dma_register,
                               edu_dma_unregister);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to setup DMA: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /*
     * Step 7: Setup device reset callback
     */
    ret = vfu_setup_device_reset_cb(edu->vfu_ctx, edu_device_reset);
    if (ret < 0) {
        fprintf(stderr, "edu: failed to setup reset callback: %s\n",
                strerror(errno));
        goto cleanup;
    }

    /*
     * Step 8: Realize the device
     */
    ret = vfu_realize_ctx(edu->vfu_ctx);
    if (ret < 0) {
        fprintf(stderr, "edu: vfu_realize_ctx failed: %s\n",
                strerror(errno));
        goto cleanup;
    }

    printf("edu: device realized, waiting for connection...\n");

    /*
     * Step 9: Initialize factorial thread
     */
    pthread_mutex_init(&edu->fact_mutex, NULL);
    pthread_cond_init(&edu->fact_cond, NULL);
    edu->stopping = false;
    edu->fact_pending = false;
    ret = pthread_create(&edu->fact_thread, NULL, edu_fact_thread, edu);
    if (ret != 0) {
        fprintf(stderr, "edu: failed to create factorial thread: %s\n",
                strerror(ret));
        goto cleanup;
    }

    /*
     * Step 10: Wait for client connection (non-blocking)
     */
    printf("edu: waiting for QEMU to connect on %s ...\n", socket_path);
    while (g_running) {
        ret = vfu_attach_ctx(edu->vfu_ctx);
        if (ret == 0) {
            printf("edu: client connected!\n");
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "edu: vfu_attach_ctx failed: %s\n",
                    strerror(errno));
            goto stop_thread;
        }
        usleep(100000); /* 100ms */
    }

    if (!g_running) {
        goto stop_thread;
    }

    /*
     * Step 11: Main event loop — process vfio-user messages
     */
    printf("edu: entering main loop\n");
    while (g_running) {
        ret = vfu_run_ctx(edu->vfu_ctx);
        if (ret < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            if (errno == ENOTCONN) {
                printf("edu: client disconnected\n");
                break;
            }
            fprintf(stderr, "edu: vfu_run_ctx error: %s\n",
                    strerror(errno));
            break;
        }
    }

    printf("edu: shutting down\n");

stop_thread:
    /* Stop the factorial thread */
    pthread_mutex_lock(&edu->fact_mutex);
    edu->stopping = true;
    pthread_cond_signal(&edu->fact_cond);
    pthread_mutex_unlock(&edu->fact_mutex);
    pthread_join(edu->fact_thread, NULL);
    pthread_mutex_destroy(&edu->fact_mutex);
    pthread_cond_destroy(&edu->fact_cond);

cleanup:
    vfu_destroy_ctx(edu->vfu_ctx);
    free(edu);

    printf("edu: done\n");
    return 0;
}
