/*
 * hdgl-read.c — phi-bridge mailbox reader
 *
 * Reads the HDGL phi-bridge mailbox from physical memory at 0x50000.
 * Run AFTER booting the router OS via HDGL's 'boot' command.
 *
 * The mailbox persists in conventional memory because Linux/FreeBSD/OpenBSD
 * do not reclaim physical addresses below 640KB that are not in the E820
 * "available" set — and 0x50000 is within the range that is typically left
 * untouched after real-mode BIOS initialisation.
 *
 * Build:
 *   gcc -O2 -o hdgl-read hdgl-read.c
 *
 * Run:
 *   sudo ./hdgl-read
 *   sudo ./hdgl-read --watch       (poll every second)
 *   sudo ./hdgl-read --json        (machine-readable output)
 *
 * Works on: Linux, FreeBSD, OpenBSD, NetBSD
 * Requires: root / CAP_SYS_RAWIO
 *
 * Mailbox layout (104 bytes at physical 0x50000):
 *   +0   uint32  magic       0x48444C47 'HDLG'
 *   +4   uint32  version     1
 *   +8   uint64  phi_tick    address of live phi-tick counter (0x101010)
 *   +16  uint64  omega_base  Omega graph base (0x200000)
 *   +24  uint64  nic_table   NIC table base (0x104000)
 *   +32  uint64  nic_count   number of NICs detected at boot
 *   +40  uint64  uart_table  UART table base (0x104200)
 *   +48  uint64  uart_count  number of UARTs detected at boot
 *   +56  uint64  lat_base    phi-lattice base (0x101020)
 *   +64  uint64  consensus   1 = LOCK at time of chainload
 *   +72  uint64  wave_agg    Dn(r) wave aggregate at time of chainload
 *   +80  uint64  cpu_cpuid   CPUID leaf1 eax (processor ID)
 *   +88  uint64  mem_kb      usable RAM in KB (from E820)
 *   +96  uint32  hdgl_drive  BIOS drive HDGL booted from (0x80...)
 *   +100 uint32  reserved    0
 *
 * NOTE: The phi_tick, omega_base, lat_base fields are ADDRESSES pointing
 * into physical memory that HDGL used. To read the live phi-tick counter
 * you must mmap that address too (0x101010). This is valid as long as the
 * OS has not reclaimed or repurposed that memory region.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#define MAILBOX_PHYS    0x50000UL
#define MAILBOX_SIZE    104
#define MAGIC_HDLG      0x48444C47UL
#define PAGE_SIZE       4096UL
#define PAGE_MASK       (~(PAGE_SIZE - 1))

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t phi_tick_ptr;
    uint64_t omega_base;
    uint64_t nic_table;
    uint64_t nic_count;
    uint64_t uart_table;
    uint64_t uart_count;
    uint64_t lat_base;
    uint64_t consensus;
    uint64_t wave_agg;
    uint64_t cpu_cpuid;
    uint64_t mem_kb;
    uint32_t hdgl_drive;
    uint32_t reserved;
} __attribute__((packed)) hdgl_mailbox_t;

static void *mem_map(int fd, uint64_t phys, size_t len) {
    uint64_t page_base = phys & PAGE_MASK;
    size_t   page_off  = phys - page_base;
    size_t   map_len   = page_off + len;
    void    *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, (off_t)page_base);
    if (map == MAP_FAILED) return NULL;
    return (char *)map + page_off;
}

static void decode_cpuid(uint32_t eax, char *out) {
    int family  = ((eax >> 8)  & 0xF) + ((eax >> 20) & 0xFF);
    int model   = ((eax >> 4)  & 0xF) | (((eax >> 16) & 0xF) << 4);
    int step    =  (eax >> 0)  & 0xF;
    snprintf(out, 64, "family=%d model=%d step=%d (0x%08X)", family, model, step, eax);
}

static void print_mailbox(const hdgl_mailbox_t *mb, int json, int fd) {
    char cpuid_str[64];
    decode_cpuid((uint32_t)mb->cpu_cpuid, cpuid_str);

    /* Try to read live phi-tick if the memory is still accessible */
    uint64_t live_tick = 0;
    int has_live = 0;
    if (mb->phi_tick_ptr && fd >= 0) {
        void *tick_map = mem_map(fd, mb->phi_tick_ptr, 8);
        if (tick_map) {
            memcpy(&live_tick, tick_map, 8);
            munmap((char *)tick_map - (mb->phi_tick_ptr & (PAGE_SIZE-1)),
                   PAGE_SIZE);
            has_live = 1;
        }
    }

    if (json) {
        printf("{\n");
        printf("  \"magic\": \"0x%08X\",\n", mb->magic);
        printf("  \"version\": %u,\n", mb->version);
        printf("  \"consensus\": %s,\n", mb->consensus ? "true" : "false");
        printf("  \"wave_agg\": \"0x%016llX\",\n", (unsigned long long)mb->wave_agg);
        printf("  \"cpu_cpuid\": \"0x%08X\",\n", (uint32_t)mb->cpu_cpuid);
        printf("  \"mem_kb\": %llu,\n", (unsigned long long)mb->mem_kb);
        printf("  \"nic_count\": %llu,\n", (unsigned long long)mb->nic_count);
        printf("  \"uart_count\": %llu,\n", (unsigned long long)mb->uart_count);
        printf("  \"hdgl_drive\": \"0x%02X\",\n", mb->hdgl_drive);
        if (has_live)
            printf("  \"phi_tick_live\": %llu,\n", (unsigned long long)live_tick);
        printf("  \"phi_tick_ptr\": \"0x%llX\",\n", (unsigned long long)mb->phi_tick_ptr);
        printf("  \"omega_base\": \"0x%llX\",\n", (unsigned long long)mb->omega_base);
        printf("  \"lat_base\": \"0x%llX\"\n", (unsigned long long)mb->lat_base);
        printf("}\n");
    } else {
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  HDGL phi-bridge mailbox @ 0x%05lX\n", MAILBOX_PHYS);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  version    : %u\n", mb->version);
        printf("  hdgl_drive : 0x%02X  (HDGL booted from this drive)\n", mb->hdgl_drive);
        printf("  cpu        : %s\n", cpuid_str);
        printf("  memory     : %llu KB (%.1f GB)\n",
               (unsigned long long)mb->mem_kb,
               (double)mb->mem_kb / (1024.0 * 1024.0));
        printf("  nics       : %llu detected at HDGL boot\n",
               (unsigned long long)mb->nic_count);
        printf("  uarts      : %llu detected at HDGL boot\n",
               (unsigned long long)mb->uart_count);
        printf("  consensus  : %s\n", mb->consensus ? "LOCK" : "UNLOCK");
        printf("  wave_agg   : 0x%016llX\n", (unsigned long long)mb->wave_agg);
        if (has_live)
            printf("  phi_tick   : %llu (live)\n", (unsigned long long)live_tick);
        else
            printf("  phi_tick   : 0x%llX (ptr; map /dev/mem to read live)\n",
                   (unsigned long long)mb->phi_tick_ptr);
        printf("  omega_base : 0x%llX\n", (unsigned long long)mb->omega_base);
        printf("  lat_base   : 0x%llX\n", (unsigned long long)mb->lat_base);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    }
}

int main(int argc, char *argv[]) {
    int do_watch = 0, do_json = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0) do_watch = 1;
        if (strcmp(argv[i], "--json")  == 0) do_json  = 1;
        if (strcmp(argv[i], "--help")  == 0) {
            printf("Usage: %s [--watch] [--json]\n", argv[0]);
            printf("  Reads HDGL phi-bridge mailbox from /dev/mem at 0x%05lX\n",
                   MAILBOX_PHYS);
            return 0;
        }
    }

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "hdgl-read: cannot open /dev/mem: %s\n"
                        "  Try: sudo %s\n", strerror(errno), argv[0]);
        return 1;
    }

    do {
        hdgl_mailbox_t *mb = mem_map(fd, MAILBOX_PHYS, sizeof(hdgl_mailbox_t));
        if (!mb) {
            fprintf(stderr, "hdgl-read: mmap failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }

        if (mb->magic != MAGIC_HDLG) {
            fprintf(stderr,
                    "hdgl-read: no HDGL mailbox at 0x%05lX (found 0x%08X)\n"
                    "  Boot the router OS via: Router64> boot\n",
                    MAILBOX_PHYS, mb->magic);
            munmap((char *)mb - (MAILBOX_PHYS & (PAGE_SIZE-1)), PAGE_SIZE);
            close(fd);
            return 2;
        }

        print_mailbox(mb, do_json, fd);
        munmap((char *)mb - (MAILBOX_PHYS & (PAGE_SIZE-1)), PAGE_SIZE);

        if (do_watch) {
            sleep(1);
            if (!do_json) printf("\033[%dA", 13); /* move cursor up */
        }
    } while (do_watch);

    close(fd);
    return 0;
}
