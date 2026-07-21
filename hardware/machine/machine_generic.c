#include "generic.h"
#include "merced.h"
#include "vga_ibm.h"
#include "vgafont16.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include "gemu/util.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 640
#define FB_H 400
#define INSTR_PER_FRAME 500000
#define HALT_TRACE_LINES 32
#define HALT_CALL_LINES 32

#define KBD_ACTION_UP    GEMU_ACTION(0)
#define KBD_ACTION_DOWN  GEMU_ACTION(1)
#define KBD_ACTION_ENTER GEMU_ACTION(2)
#define KBD_FIFO_SIZE 16

static const GemuActionDef generic_kbd_actions[] = {
    { "Up",    KBD_ACTION_UP,    "Up"     },
    { "Down",  KBD_ACTION_DOWN,  "Down"   },
    { "Enter", KBD_ACTION_ENTER, "Return" },
};

struct Ia64GenericState {
    GemuMonitor *monitor;
    GemuDisplay *display;
    Merced      *cpu;

    uint8_t  *ram;
    uint64_t  ram_size;
    uint8_t   rom[GENERIC_ROM_SIZE];
    char      rom_file[512];
    bool      rom_loaded;

    bool         halted;
    MercedStatus halt_status;

    VgaIbm    vga;
    uint32_t  fb[FB_W * FB_H];

    uint8_t  kbd_fifo[KBD_FIFO_SIZE];
    int      kbd_head, kbd_tail;

    FILE    *cdrom;
    char     cdrom_file[512];
    uint64_t cdrom_size;
    uint32_t cdrom_lba;
    uint8_t  cdrom_result;
    uint8_t  cdrom_buf[GENERIC_CDROM_BUF_SIZE];
    char     cdrom_path_buf[GENERIC_CDROM_PATH_SIZE];

    /* El Torito boot image / embedded FAT12 volume, mounted lazily on the
     * first file-open command (see cdrom_open_file() below). */
    bool     fat12_valid;
    uint32_t fat12_vol_off;           /* byte offset of FAT12 sector 0 */
    uint32_t fat12_bytes_per_cluster;
    uint32_t fat12_fat_off;           /* byte offset of first FAT copy */
    uint32_t fat12_root_dir_off;
    uint32_t fat12_root_entries;
    uint32_t fat12_data_area_off;     /* byte offset where cluster 2 begins */

    /* Currently open file (opened by GENERIC_CDROM_CMD_OPEN). */
    bool     file_open;
    uint32_t file_size;
    uint32_t file_remaining;
    uint32_t file_cur_cluster;
    uint32_t chunk_size;              /* valid bytes in cdrom_buf after READ_NEXT */

    /* PE32+/IA-64 image loading (GENERIC_CDROM_CMD_LOAD_PE). */
    uint32_t pe_src, pe_dst;
    uint32_t pe_entry_rva;             /* valid after a successful CMD 4 */
    uint32_t cdrom_dma_dst;
    uint32_t cdrom_dma_size;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void kbd_push(Ia64GenericState *s, uint8_t code) {
    int next = (s->kbd_tail + 1) % KBD_FIFO_SIZE;
    if (next == s->kbd_head)
        return;   /* drop if full */
    s->kbd_fifo[s->kbd_tail] = code;
    s->kbd_tail = next;
}

static uint8_t kbd_pop(Ia64GenericState *s) {
    if (s->kbd_head == s->kbd_tail)
        return 0;
    uint8_t code = s->kbd_fifo[s->kbd_head];
    s->kbd_head = (s->kbd_head + 1) % KBD_FIFO_SIZE;
    return code;
}

static void cdrom_do_read(Ia64GenericState *s) {
    if (!s->cdrom) {
        s->cdrom_result = 1;
        return;
    }
    uint64_t off = (uint64_t)s->cdrom_lba * GENERIC_CDROM_SECTOR_SIZE;
    if (off + GENERIC_CDROM_SECTOR_SIZE > s->cdrom_size ||
        fseek(s->cdrom, (long)off, SEEK_SET) != 0 ||
        fread(s->cdrom_buf, 1, GENERIC_CDROM_SECTOR_SIZE, s->cdrom) !=
            GENERIC_CDROM_SECTOR_SIZE) {
        s->cdrom_result = 1;
        return;
    }
    s->cdrom_result = 0;
}

static void cdrom_do_read_dma(Ia64GenericState *s) {
    s->cdrom_result = 1;
    if (!s->cdrom || s->cdrom_dma_size == 0 ||
        s->cdrom_dma_size % GENERIC_CDROM_SECTOR_SIZE != 0 ||
        (uint64_t)s->cdrom_dma_dst + s->cdrom_dma_size > s->ram_size)
        return;
    uint64_t off = (uint64_t)s->cdrom_lba * GENERIC_CDROM_SECTOR_SIZE;
    if (off + s->cdrom_dma_size > s->cdrom_size ||
        fseek(s->cdrom, (long)off, SEEK_SET) != 0 ||
        fread(s->ram + s->cdrom_dma_dst, 1, s->cdrom_dma_size, s->cdrom) !=
            s->cdrom_dma_size)
        return;
    s->cdrom_result = 0;
}

/* ── El Torito / FAT12 (embedded EFI boot image) ─────────────────────────
 *
 * Microsoft's IA-64 EFI install media of this era package their EFI boot
 * loader inside an El Torito "no emulation" boot image that is itself a
 * plain 1.44 MiB FAT12 floppy image - real EFI firmware mounts it as a
 * filesystem and opens the loader by name, rather than executing it as
 * x86 boot-sector code (its FAT12 boot sector looks like one, but that's
 * just the on-disk format's provenance, not how this actually boots).
 * All of that lives here on the host side, in ordinary tested C - see the
 * comment on the CD-ROM controller in hardware/generic.h for why. */

static bool cdrom_find_eltorito_boot_lba(Ia64GenericState *s, uint32_t *out_lba) {
    uint8_t sec[2048];
    for (uint32_t lba = 16; lba < 16 + 32; lba++) {
        if (fseek(s->cdrom, (long)lba * 2048, SEEK_SET) != 0)
            return false;
        if (fread(sec, 1, 2048, s->cdrom) != 2048)
            return false;
        if (sec[0] == 255)   /* volume descriptor set terminator */
            return false;
        if (sec[0] == 0 && memcmp(sec + 7, "EL TORITO SPECIFICATION", 23) == 0) {
            uint32_t catalog_lba = rd32(sec + 71);
            uint8_t cat[64];
            if (fseek(s->cdrom, (long)catalog_lba * 2048, SEEK_SET) != 0 ||
                fread(cat, 1, sizeof(cat), s->cdrom) != sizeof(cat))
                return false;
            *out_lba = rd32(cat + 40);
            return true;
        }
    }
    return false;
}

static bool fat12_mount(Ia64GenericState *s) {
    if (s->fat12_valid)
        return true;
    if (!s->cdrom)
        return false;
    uint32_t boot_lba;
    if (!cdrom_find_eltorito_boot_lba(s, &boot_lba))
        return false;
    uint32_t vol_off = boot_lba * 2048u;
    uint8_t bs[512];
    if (fseek(s->cdrom, (long)vol_off, SEEK_SET) != 0 ||
        fread(bs, 1, sizeof(bs), s->cdrom) != sizeof(bs))
        return false;

    uint32_t bytes_per_sector = rd16(bs + 11);
    uint32_t sectors_per_cluster = bs[13];
    uint32_t reserved = rd16(bs + 14);
    uint32_t num_fats = bs[16];
    uint32_t root_entries = rd16(bs + 17);
    uint32_t sectors_per_fat = rd16(bs + 22);
    if (bytes_per_sector == 0 || sectors_per_cluster == 0 || num_fats == 0)
        return false;

    s->fat12_vol_off = vol_off;
    s->fat12_bytes_per_cluster = sectors_per_cluster * bytes_per_sector;
    s->fat12_fat_off = vol_off + reserved * bytes_per_sector;
    s->fat12_root_dir_off = s->fat12_fat_off + num_fats * sectors_per_fat * bytes_per_sector;
    s->fat12_root_entries = root_entries;
    s->fat12_data_area_off = s->fat12_root_dir_off + root_entries * 32u;
    s->fat12_valid = true;
    return true;
}

/* Converts "SETUPLDR.EFI" style names to space-padded 8.3 directory-entry
 * form. No support for subdirectory paths - not needed, everything this
 * loader cares about lives in the floppy image's root directory. */
static void fat12_to_83(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    int i = 0, j = 0;
    for (; name[i] && name[i] != '.' && j < 8; i++, j++)
        out[j] = (uint8_t)toupper((unsigned char)name[i]);
    if (name[i] == '.') {
        i++;
        for (int k = 0; name[i] && k < 3; i++, k++)
            out[8 + k] = (uint8_t)toupper((unsigned char)name[i]);
    }
}

static void cdrom_open_file(Ia64GenericState *s, const char *path) {
    s->file_open = false;
    if (!fat12_mount(s)) {
        s->cdrom_result = 1;
        return;
    }
    uint8_t want[11];
    fat12_to_83(path, want);

    size_t dir_bytes = (size_t)s->fat12_root_entries * 32u;
    uint8_t *dirbuf = malloc(dir_bytes);
    if (!dirbuf) {
        s->cdrom_result = 1;
        return;
    }
    bool found = false;
    uint32_t start_cluster = 0, file_size = 0;
    if (fseek(s->cdrom, (long)s->fat12_root_dir_off, SEEK_SET) == 0 &&
        fread(dirbuf, 1, dir_bytes, s->cdrom) == dir_bytes) {
        for (uint32_t i = 0; i < s->fat12_root_entries; i++) {
            uint8_t *e = dirbuf + i * 32;
            if (e[0] == 0x00)
                break;
            if (e[0] == 0xE5 || (e[11] & 0x18))   /* deleted, dir, or volume label */
                continue;
            if (memcmp(e, want, 11) == 0) {
                start_cluster = rd16(e + 26);
                file_size = rd32(e + 28);
                found = true;
                break;
            }
        }
    }
    free(dirbuf);
    if (!found) {
        s->cdrom_result = 1;
        return;
    }
    s->file_open = true;
    s->file_size = file_size;
    s->file_remaining = file_size;
    s->file_cur_cluster = start_cluster;
    s->cdrom_result = 0;
}

static uint32_t fat12_next_cluster(Ia64GenericState *s, uint32_t cluster) {
    uint32_t fat_byte_off = cluster + cluster / 2;   /* == cluster*3/2 */
    uint8_t buf[2];
    if (fseek(s->cdrom, (long)(s->fat12_fat_off + fat_byte_off), SEEK_SET) != 0 ||
        fread(buf, 1, 2, s->cdrom) != 2)
        return 0xFFF;
    uint16_t word = rd16(buf);
    return (cluster & 1) ? (word >> 4) : (word & 0x0FFF);
}

/* Fills cdrom_buf with up to GENERIC_CDROM_BUF_SIZE more bytes of the
 * file opened by cdrom_open_file(), walking the FAT12 cluster chain.
 * Assumes the cluster size evenly divides the buffer size (true for the
 * standard 1.44 MiB floppy layout these images use: 512-byte clusters
 * into a 2048-byte buffer) - a cluster is always fully consumed or is
 * the file's last, never split across two READ_NEXT calls. */
static void cdrom_read_next_chunk(Ia64GenericState *s) {
    if (!s->file_open || s->file_remaining == 0) {
        s->chunk_size = 0;
        s->cdrom_result = 1;
        return;
    }
    uint32_t cluster_bytes = s->fat12_bytes_per_cluster;
    uint32_t produced = 0;
    while (produced < GENERIC_CDROM_BUF_SIZE && s->file_remaining > 0 &&
           s->file_cur_cluster >= 2 && s->file_cur_cluster < 0xFF8) {
        uint32_t cluster_off = s->fat12_data_area_off +
            (s->file_cur_cluster - 2) * cluster_bytes;
        uint32_t want = cluster_bytes;
        if (want > s->file_remaining)
            want = s->file_remaining;
        if (fseek(s->cdrom, (long)cluster_off, SEEK_SET) != 0 ||
            fread(s->cdrom_buf + produced, 1, want, s->cdrom) != want)
            break;
        produced += want;
        s->file_remaining -= want;
        if (want == cluster_bytes)
            s->file_cur_cluster = fat12_next_cluster(s, s->file_cur_cluster);
    }
    s->chunk_size = produced;
    s->cdrom_result = (produced > 0) ? 0 : 1;
}

/* ── PE32+/IA-64 image loading ────────────────────────────────────────────
 *
 * Copies each section of an already-loaded raw PE file (at `src`, placed
 * there earlier via the OPEN/READ_NEXT streaming above) to its virtual
 * address relative to `dst`, zero-padding out to VirtualSize - the file's
 * on-disk layout does not match its in-memory layout (alignment gaps,
 * BSS), so this can't just be a flat copy. Only PE32+ (64-bit) IA-64
 * images are handled - that's what SETUPLDR.EFI and every other IA-64 EFI
 * binary of this era actually is.
 *
 * `dst` is expected to be the image's own preferred ImageBase (firmware
 * reads that from the optional header itself before issuing this
 * command), which sidesteps PE base relocations entirely: relocations
 * only patch addresses that assumed a different load address, and if
 * dst == ImageBase the adjustment is always zero. */
static void cdrom_load_pe_image(Ia64GenericState *s, uint32_t src, uint32_t dst) {
    s->cdrom_result = 1;
    s->pe_entry_rva = 0;

    if ((uint64_t)src + 0x40 > s->ram_size || dst >= s->ram_size)
        return;
    const uint8_t *raw = s->ram + src;
    uint64_t src_remaining = s->ram_size - src;

    if (raw[0] != 'M' || raw[1] != 'Z')
        return;
    uint32_t e_lfanew = rd32(raw + 0x3C);
    if ((uint64_t)e_lfanew + 24 > src_remaining)
        return;
    const uint8_t *pe = raw + e_lfanew;
    if (memcmp(pe, "PE\0\0", 4) != 0)
        return;

    const uint8_t *coff = pe + 4;
    uint16_t machine = rd16(coff + 0);
    uint16_t num_sections = rd16(coff + 2);
    uint16_t size_opt_hdr = rd16(coff + 16);
    if (machine != 0x200)
        return;

    const uint8_t *opt = coff + 20;
    if ((uint64_t)(opt - raw) + size_opt_hdr > src_remaining || size_opt_hdr < 112)
        return;
    if (rd16(opt) != 0x20B)   /* PE32+ only */
        return;

    uint32_t entry_rva = rd32(opt + 16);
    uint64_t image_base = rd64(opt + 24);
    uint32_t size_of_image = rd32(opt + 56);
    if (image_base != dst)
        return;   /* only the no-relocation-needed case is supported */
    if ((uint64_t)dst + size_of_image > s->ram_size)
        return;

    const uint8_t *sec_table = opt + size_opt_hdr;
    uint64_t sec_table_bytes = (uint64_t)num_sections * 40u;
    if ((uint64_t)(sec_table - raw) + sec_table_bytes > src_remaining)
        return;

    uint32_t size_of_headers = rd32(opt + 60);

    memset(s->ram + dst, 0, size_of_image);
    /* Copy the raw headers (DOS/PE/COFF/Optional header, Data
     * Directories, section table) to the image base too, not just the
     * sections - PE images routinely read their own loaded headers at
     * runtime (e.g. to find the resource directory), and since the
     * first section is typically well past the header region (.text
     * often starts at RVA 0x2000), that gap would otherwise stay
     * zeroed forever. Matches what a real loader does (see
     * reference/qemu-system-ia64/roms/ia64-firmware/firmware.c's
     * fw_copy_mem(...,size_of_headers) step). */
    if (size_of_headers > 0 && size_of_headers <= src_remaining &&
        size_of_headers <= size_of_image)
        memcpy(s->ram + dst, raw, size_of_headers);

    for (unsigned i = 0; i < num_sections; i++) {
        const uint8_t *sh = sec_table + i * 40u;
        uint32_t vsize = rd32(sh + 8);
        uint32_t vaddr = rd32(sh + 12);
        uint32_t rawsize = rd32(sh + 16);
        uint32_t rawptr = rd32(sh + 20);
        uint32_t copy_size = (vsize != 0 && vsize < rawsize) ? vsize : rawsize;

        if (copy_size == 0)
            continue;
        if ((uint64_t)rawptr + copy_size > src_remaining)
            return;
        if ((uint64_t)vaddr + copy_size > size_of_image)
            return;
        memcpy(s->ram + dst + vaddr, s->ram + src + rawptr, copy_size);
    }

    if (entry_rva >= size_of_image)
        return;
    s->pe_entry_rva = entry_rva;
    s->cdrom_result = 0;
}

/* ── Physical address space ─────────────────────────────────────────────── */

static bool vga_mem_window(Ia64GenericState *s, uint64_t addr, unsigned size,
                           uint32_t *voff) {
    uint32_t base, wsize;
    vga_ibm_aperture(&s->vga, &base, &wsize);
    if (addr < base || addr + size > (uint64_t)base + wsize)
        return false;
    *voff = (uint32_t)(addr - base);
    return true;
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64GenericState *s = ud;
    uint32_t voff;
    if (vga_mem_window(s, addr, size, &voff)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)vga_ibm_mem_read(&s->vga, voff + i) << (i * 8);
        return v;
    }
    if (addr >= GENERIC_VGA_IO_BASE &&
        addr + size <= GENERIC_VGA_IO_BASE + GENERIC_VGA_IO_SIZE) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)vga_ibm_io_read(&s->vga,
                    (uint16_t)(0x3B0 + (addr - GENERIC_VGA_IO_BASE) + i)) << (i * 8);
        return v;
    }
    if (addr >= GENERIC_KBD_IO_BASE && addr + size <= GENERIC_KBD_IO_BASE + GENERIC_KBD_IO_SIZE) {
        uint32_t off = (uint32_t)(addr - GENERIC_KBD_IO_BASE);
        if (off == 0)
            return s->kbd_head != s->kbd_tail;
        if (off == 4)
            return kbd_pop(s);
        return 0;
    }
    if (addr >= GENERIC_CDROM_IO_BASE && addr + size <= GENERIC_CDROM_IO_BASE + GENERIC_CDROM_IO_SIZE) {
        uint32_t off = (uint32_t)(addr - GENERIC_CDROM_IO_BASE);
        if (off == 0)
            return s->cdrom != NULL;
        if (off == 0xC)
            return s->cdrom_result;
        if (off == 0x10)
            return s->file_size;
        if (off == 0x14)
            return s->chunk_size;
        if (off == 0x20)
            return s->pe_entry_rva;
        return 0;
    }
    if (addr >= GENERIC_CDROM_BUF_BASE && addr + size <= GENERIC_CDROM_BUF_BASE + GENERIC_CDROM_BUF_SIZE) {
        uint64_t v = 0;
        memcpy(&v, s->cdrom_buf + (addr - GENERIC_CDROM_BUF_BASE), size);
        return v;
    }
    if (addr + size <= s->ram_size) {
        uint64_t v = 0;
        memcpy(&v, s->ram + addr, size);
        return v;
    }
    if (addr >= GENERIC_ROM_BASE && addr + size <= GENERIC_ROM_BASE + GENERIC_ROM_SIZE) {
        uint64_t v = 0;
        memcpy(&v, s->rom + (addr - GENERIC_ROM_BASE), size);
        return v;
    }
    return ~0ull;
}

/* Every character the guest ever puts on screen - our own boot manager's
 * text and SETUPLDR.EFI's own ConOut->OutputString calls alike - passes
 * through here, since both ultimately write to the same VGA text plane.
 * Mirroring it to host stdout here (rather than in every assembly
 * routine that prints something) gives a "COM1"-style debug log for
 * free, with no firmware-side bookkeeping and nothing to remember to
 * wire up for future text output paths. */
static void vga_mirror_to_stdout(Ia64GenericState *s, uint32_t voff, uint8_t val) {
    if (!vga_ibm_is_text_mode(&s->vga) || (voff & 1) != 0)
        return;   /* only even offsets are the character plane (odd = attribute) */
    if (val >= 0x20 && val < 0x7F)
        putchar((int)val);
    else if (val == '\n' || val == '\r')
        putchar('\n');
}

static void bus_write(void *ud, uint64_t addr, uint64_t val, unsigned size) {
    Ia64GenericState *s = ud;
    uint32_t voff;
    if (vga_mem_window(s, addr, size, &voff)) {
        for (unsigned i = 0; i < size; i++) {
            uint8_t b = (uint8_t)(val >> (i * 8));
            vga_ibm_mem_write(&s->vga, voff + i, b);
            vga_mirror_to_stdout(s, voff + i, b);
        }
        fflush(stdout);
        return;
    }
    if (addr >= GENERIC_VGA_IO_BASE &&
        addr + size <= GENERIC_VGA_IO_BASE + GENERIC_VGA_IO_SIZE) {
        for (unsigned i = 0; i < size; i++)
            vga_ibm_io_write(&s->vga,
                    (uint16_t)(0x3B0 + (addr - GENERIC_VGA_IO_BASE) + i),
                    (uint8_t)(val >> (i * 8)));
        return;
    }
    if (addr >= GENERIC_CDROM_IO_BASE && addr + size <= GENERIC_CDROM_IO_BASE + GENERIC_CDROM_IO_SIZE) {
        uint32_t off = (uint32_t)(addr - GENERIC_CDROM_IO_BASE);
        if (off == 4) {
            s->cdrom_lba = (uint32_t)val;
        } else if (off == 8) {
            uint8_t cmd = (uint8_t)val;
            if (cmd == GENERIC_CDROM_CMD_READ)
                cdrom_do_read(s);
            else if (cmd == GENERIC_CDROM_CMD_OPEN)
                cdrom_open_file(s, s->cdrom_path_buf);
            else if (cmd == GENERIC_CDROM_CMD_READ_NEXT)
                cdrom_read_next_chunk(s);
            else if (cmd == GENERIC_CDROM_CMD_LOAD_PE)
                cdrom_load_pe_image(s, s->pe_src, s->pe_dst);
            else if (cmd == GENERIC_CDROM_CMD_READ_DMA)
                cdrom_do_read_dma(s);
        } else if (off == 0x18) {
            s->pe_src = (uint32_t)val;
        } else if (off == 0x1C) {
            s->pe_dst = (uint32_t)val;
        } else if (off == 0x24) {
            s->cdrom_dma_dst = (uint32_t)val;
        } else if (off == 0x28) {
            s->cdrom_dma_size = (uint32_t)val;
        }
        return;
    }
    if (addr >= GENERIC_CDROM_PATH_BASE && addr + size <= GENERIC_CDROM_PATH_BASE + GENERIC_CDROM_PATH_SIZE) {
        for (unsigned i = 0; i < size; i++) {
            uint32_t off = (uint32_t)(addr - GENERIC_CDROM_PATH_BASE) + i;
            if (off < sizeof(s->cdrom_path_buf))
                s->cdrom_path_buf[off] = (char)(val >> (i * 8));
        }
        return;
    }
    if (addr + size <= s->ram_size) {
        memcpy(s->ram + addr, &val, size);
        return;
    }
    /* ROM: read-only, like real (unprogrammed) flash - writes are dropped. */
}

/* Code and data share the same view - our own firmware, no legacy shadow
 * dance to model (see hardware/generic.h). */
static uint64_t bus_fetch(void *ud, uint64_t addr, unsigned size) {
    return bus_read(ud, addr, size);
}

static bool bus_fill(void *ud, uint64_t addr, uint8_t val, uint64_t len) {
    Ia64GenericState *s = ud;
    if (addr + len <= s->ram_size) {
        memset(s->ram + addr, val, (size_t)len);
        return true;
    }
    return false;
}

/* ── Firmware loading ────────────────────────────────────────────────────── */

bool ia64_generic_load_firmware(Ia64GenericState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu: cannot open firmware '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (uint64_t)len > GENERIC_ROM_SIZE) {
        fprintf(stderr, "gemu: firmware '%s' is %ld bytes (expected at most %u)\n",
                path, len, GENERIC_ROM_SIZE);
        fclose(f);
        return false;
    }
    uint32_t off = GENERIC_ROM_SIZE - (uint32_t)len;
    memset(s->rom, 0xFF, off);
    size_t rd = fread(s->rom + off, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) {
        fprintf(stderr, "gemu: short read on firmware '%s'\n", path);
        return false;
    }
    snprintf(s->rom_file, sizeof(s->rom_file), "%s", path);
    s->rom_loaded = true;
    gemu_monitor_register_rom(s->monitor, (uint32_t)(GENERIC_ROM_BASE + off),
                              (uint32_t)len, path);
    return true;
}

/* ── Monitor / display glue ──────────────────────────────────────────────── */

static void generic_cpu_state(void *ud, char *buf, size_t buf_len) {
    Ia64GenericState *s = ud;
    merced_dump_state(s->cpu, buf, buf_len);
}

static void generic_render_frame(Ia64GenericState *s) {
    vga_ibm_render(&s->vga, s->fb, FB_W, FB_H, vgafont16);
}

static bool generic_screendump(void *ud, const char *path) {
    Ia64GenericState *s = ud;
    generic_render_frame(s);
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

static void generic_custom_cmd(Ia64GenericState *s) {
    const char *txt = gemu_monitor_command_text(s->monitor);
    uint64_t n;
    if (txt && sscanf(txt, "trace %" SCNu64, &n) == 1) {
        s->cpu->trace_n = n;
        printf("tracing next %" PRIu64 " slots to stderr\n", n);
        return;
    }
    if (txt && strncmp(txt, "history", 7) == 0) {
        unsigned count = 128;
        (void)sscanf(txt + 7, "%u", &count);
        if (count > MERCED_TRACE_HISTORY) count = MERCED_TRACE_HISTORY;
        merced_dump_trace(s->cpu, count, stderr);
        return;
    }
    if (txt && strncmp(txt, "calls", 5) == 0) {
        merced_dump_calls(s->cpu, MERCED_CALL_HISTORY, stderr);
        return;
    }
    if (txt && strncmp(txt, "key ", 4) == 0) {
        const char *arg = txt + 4;
        if (strcmp(arg, "up") == 0) kbd_push(s, GENERIC_KBD_KEY_UP);
        else if (strcmp(arg, "down") == 0) kbd_push(s, GENERIC_KBD_KEY_DOWN);
        else if (strcmp(arg, "enter") == 0) kbd_push(s, GENERIC_KBD_KEY_ENTER);
        else printf("usage: key <up|down|enter>\n");
        return;
    }
    if (txt && strncmp(txt, "peek ", 5) == 0) {
        uint64_t addr = strtoull(txt + 5, NULL, 16);
        if (addr + 8 <= s->ram_size) {
            uint64_t v;
            memcpy(&v, s->ram + addr, 8);
            printf("peek 0x%" PRIx64 " = 0x%016" PRIx64 "\n", addr, v);
        } else {
            printf("peek: address out of range\n");
        }
        return;
    }
    if (txt && strncmp(txt, "vhptstats", 9) == 0) {
        uint64_t calls, disabled, hit, unmapped, tagfail, np;
        merced_vhpt_stats(&calls, &disabled, &hit, &unmapped, &tagfail, &np);
        printf("vhpt: calls=%" PRIu64 " disabled=%" PRIu64 " hit=%" PRIu64
               " unmapped=%" PRIu64 " tagfail=%" PRIu64 " np=%" PRIu64 "\n",
               calls, disabled, hit, unmapped, tagfail, np);
        return;
    }
    if (txt && strncmp(txt, "vgapeek ", 8) == 0) {
        unsigned plane;
        unsigned off;
        unsigned count;
        if (sscanf(txt + 8, "%u %x %u", &plane, &off, &count) == 3 &&
            plane < 4 && count <= 256) {
            printf("vgapeek plane=%u off=0x%x:", plane, off);
            for (unsigned i = 0; i < count; i++)
                printf(" %02x", s->vga.vram[plane][off + i]);
            printf("\n");
        } else {
            printf("usage: vgapeek <plane 0-3> <hex offset> <count>\n");
        }
        return;
    }
    gemu_monitor_unknown_command(s->monitor);
}

static void generic_report_halt(Ia64GenericState *s) {
    Merced *m = s->cpu;
    char buf[4096];
    fprintf(stderr, "\ngeneric: CPU halted after %" PRIu64 " instructions\n"
                    "generic: %s\n",
            m->ninsts, m->halt_msg);
    fprintf(stderr, "generic: recent instruction slots:\n");
    merced_dump_trace(m, HALT_TRACE_LINES, stderr);
    fprintf(stderr, "generic: recent calls/returns:\n");
    merced_dump_calls(m, HALT_CALL_LINES, stderr);
    merced_dump_state(m, buf, sizeof(buf));
    fputs(buf, stderr);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Ia64GenericState *ia64_generic_create(const GenericConfig *cfg) {
    Ia64GenericState *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->ram_size = cfg->ram_size;
    s->ram = calloc(1, (size_t)s->ram_size);
    if (!s->ram) {
        fprintf(stderr, "gemu: cannot allocate %" PRIu64 " MiB guest RAM\n",
                cfg->ram_size >> 20);
        ia64_generic_destroy(s);
        return NULL;
    }
    memset(s->rom, 0xFF, sizeof(s->rom));
    vga_ibm_reset(&s->vga);

    MercedBus bus = {
        .ud = s, .read = bus_read, .fetch = bus_fetch, .write = bus_write,
        .fill = bus_fill
    };
    s->cpu = merced_create(&bus);
    if (!s->cpu) {
        ia64_generic_destroy(s);
        return NULL;
    }
    /* Windows for Itanium refuses to proceed past "pre-B3 stepping" -
     * real historical behavior. This machine doesn't run i2000's
     * firmware, so its cpuid revision cross-check doesn't apply here
     * (see the comment on cpuid[3] in cpu/merced.c). */
    merced_set_cpu_revision(s->cpu, 6);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, generic_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, generic_screendump, s);

    if (cfg->cdrom_path) {
        s->cdrom = fopen(cfg->cdrom_path, "rb");
        if (!s->cdrom) {
            fprintf(stderr, "gemu: cannot open CD-ROM image '%s'\n", cfg->cdrom_path);
            ia64_generic_destroy(s);
            return NULL;
        }
        fseek(s->cdrom, 0, SEEK_END);
        s->cdrom_size = (uint64_t)ftell(s->cdrom);
        fseek(s->cdrom, 0, SEEK_SET);
        snprintf(s->cdrom_file, sizeof(s->cdrom_file), "%s", cfg->cdrom_path);
    }

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        GemuDisplayConfig dc = {
            .title = "GEMU",
            .fb_width = FB_W,
            .fb_height = FB_H,
            .scale = cfg->display_scale,
            .ini_section = "generic",
            .actions = generic_kbd_actions,
            .n_actions = (int)(sizeof generic_kbd_actions / sizeof *generic_kbd_actions),
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) {
            ia64_generic_destroy(s);
            return NULL;
        }
    }
    return s;
}

void ia64_generic_destroy(Ia64GenericState *s) {
    if (!s)
        return;
    if (s->display) gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    if (s->cpu) merced_destroy(s->cpu);
    if (s->cdrom) fclose(s->cdrom);
    free(s->ram);
    free(s);
}

void ia64_generic_run(Ia64GenericState *s, const GenericConfig *cfg) {
    (void)cfg;
    gemu_monitor_start(s->monitor);

    bool running = true;
    while (running) {
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                running = false;
                break;
            case GEMU_MON_RESET:
                merced_reset(s->cpu);
                vga_ibm_reset(&s->vga);
                s->halted = false;
                printf("generic: processor reset, IP=0x%016" PRIX64 "\n",
                       s->cpu->ip);
                break;
            case GEMU_MON_STEP: {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                if (n == 0) n = 1;
                s->halted = false;
                for (uint32_t i = 0; i < n && !s->halted; i++) {
                    if (gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
                        break;
                    MercedStatus st = merced_step(s->cpu);
                    if (st != MERCED_OK) {
                        s->halted = true;
                        s->halt_status = st;
                        generic_report_halt(s);
                    }
                }
                char buf[128];
                snprintf(buf, sizeof(buf), "IP=0x%016" PRIX64 " insts=%" PRIu64 "\n",
                         s->cpu->ip, s->cpu->ninsts);
                fputs(buf, stdout);
                break;
            }
            case GEMU_MON_CUSTOM:
                generic_custom_cmd(s);
                break;
            default:
                break;
            }
            if (!running)
                break;
        }
        if (!running)
            break;

        if (!gemu_monitor_is_paused(s->monitor) && !s->halted) {
            for (int i = 0; i < INSTR_PER_FRAME; i++) {
                if (gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
                    break;
                MercedStatus st = merced_step(s->cpu);
                if (st != MERCED_OK) {
                    s->halted = true;
                    s->halt_status = st;
                    generic_report_halt(s);
                    break;
                }
            }
        }

        if (s->display) {
            gemu_display_poll(s->display);
            uint32_t pressed = gemu_display_last_pressed(s->display);
            if (pressed & KBD_ACTION_UP) kbd_push(s, GENERIC_KBD_KEY_UP);
            if (pressed & KBD_ACTION_DOWN) kbd_push(s, GENERIC_KBD_KEY_DOWN);
            if (pressed & KBD_ACTION_ENTER) kbd_push(s, GENERIC_KBD_KEY_ENTER);
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));
            generic_render_frame(s);
            gemu_display_render(s->display, s->fb, FB_W, FB_H);
            gemu_sleep_ms(s->halted ? 30 : 1);
        } else {
            gemu_sleep_ms(s->halted ? 30 : 0);
        }
    }
    gemu_monitor_stop(s->monitor);
}
