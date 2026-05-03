/* ================================================================
 *  Systrix OS — drivers/fat32.c
 *  FAT32 filesystem driver
 *
 *  Extracted from kernel.c (lines ~343–1054).
 *  Provides the on-disk FAT32 layer used by the kernel VFS.
 *
 *  Features:
 *    - 28-bit cluster addressing
 *    - Directory traversal (8.3 names)
 *    - File create / read / write / delete / rename
 *    - Sub-directory create / delete
 *    - Cluster allocation with free-list scan
 *    - Cached FAT sector (single-entry cache)
 *    - CMOS date/time stamps on created entries
 *
 *  NOTE: This file contains the FAT32 code separated from kernel.c.
 *  Functions here are declared in include/kernel.h and called by
 *  vfs.c and kernel.c.
 * ================================================================ */
#include "../include/kernel.h"

/* ── Volume geometry (filled by fat32_init) ─────────────────── */
static u8  fat32_spc        = 0;   /* sectors per cluster */
static u32 fat32_fat_start  = 0;   /* LBA of first FAT sector */
static u32 fat32_data_start = 0;   /* LBA of first data sector */
static u32 fat32_root_clus  = 0;   /* root directory start cluster */
static u32 fat32_total_clus = 0;   /* total data clusters on volume */

static u8 sector_buf[512];
static u8 dir_buf[512];
static u8 fat_buf[512];
static u8 file_buf[512];

/* FAT sector cache */
static u32 fat_buf_lba = (u32)-1;

static void fat_read_cached(u32 lba) {
    if (lba == fat_buf_lba) return;
    ata_read_sector(lba, fat_buf);
    fat_buf_lba = lba;
}
static void fat_write_cached(u32 lba) {
    ata_write_sector(lba, fat_buf);
    fat_buf_lba = lba;
}

/* ── Initialisation ─────────────────────────────────────────── */
void fat32_init(void) {
    ata_read_sector(512, sector_buf);    /* partition starts at LBA 512 */
    u8 *bpb = sector_buf;
    u8  spc   = bpb[13];
    u16 rsvd  = (u16)(bpb[14] | (bpb[15] << 8));
    u8  nfats = bpb[16];
    u32 fat_sz32 = (u32)(bpb[36] | (bpb[37]<<8) | (bpb[38]<<16) | (bpb[39]<<24));
    u32 root_clus = (u32)(bpb[44] | (bpb[45]<<8) | (bpb[46]<<16) | (bpb[47]<<24));
    u32 total_secs = (u32)(bpb[32] | (bpb[33]<<8) | (bpb[34]<<16) | (bpb[35]<<24));

    fat32_spc        = spc;
    fat32_fat_start  = 512 + rsvd;
    fat32_data_start = fat32_fat_start + nfats * fat_sz32;
    fat32_root_clus  = root_clus;
    fat32_total_clus = (total_secs - (fat32_data_start - 512)) / spc;
}

/* ── Cluster helpers ─────────────────────────────────────────── */
u32 cluster_to_lba(u32 clus) {
    return fat32_data_start + (clus - 2) * fat32_spc;
}

u32 fat32_next_cluster(u32 clus) {
    u32 fat_offset = clus * 4;
    u32 fat_sector = fat32_fat_start + fat_offset / 512;
    u32 ent_offset = fat_offset % 512;
    fat_read_cached(fat_sector);
    u32 val = (u32)(fat_buf[ent_offset]     |
                   (fat_buf[ent_offset+1]<<8)|
                   (fat_buf[ent_offset+2]<<16)|
                   (fat_buf[ent_offset+3]<<24));
    return val & 0x0FFFFFFF;
}

u32 fat32_alloc_cluster(void) {
    for (u32 c = 2; c < fat32_total_clus + 2; c++) {
        u32 fat_off = c * 4;
        u32 fat_sec = fat32_fat_start + fat_off / 512;
        u32 ent_off = fat_off % 512;
        fat_read_cached(fat_sec);
        u32 val = (u32)(fat_buf[ent_off] | (fat_buf[ent_off+1]<<8) |
                        (fat_buf[ent_off+2]<<16) | (fat_buf[ent_off+3]<<24));
        if ((val & 0x0FFFFFFF) == 0) {
            /* Mark end-of-chain */
            fat_buf[ent_off]   = 0xFF;
            fat_buf[ent_off+1] = 0xFF;
            fat_buf[ent_off+2] = 0xFF;
            fat_buf[ent_off+3] = 0x0F;
            fat_write_cached(fat_sec);
            /* Zero out the cluster data */
            u32 lba = cluster_to_lba(c);
            u8 zero[512];
            for (int i = 0; i < 512; i++) zero[i] = 0;
            for (u8 s = 0; s < fat32_spc; s++) ata_write_sector(lba + s, zero);
            return c;
        }
    }
    return 0;   /* disk full */
}

static void fat_link(u32 cur, u32 next) {
    u32 fat_off = cur * 4;
    u32 fat_sec = fat32_fat_start + fat_off / 512;
    u32 ent_off = fat_off % 512;
    fat_read_cached(fat_sec);
    fat_buf[ent_off]   = (u8)(next & 0xFF);
    fat_buf[ent_off+1] = (u8)((next >> 8) & 0xFF);
    fat_buf[ent_off+2] = (u8)((next >> 16) & 0xFF);
    fat_buf[ent_off+3] = (u8)((next >> 24) & 0x0F) | 0x00;
    fat_write_cached(fat_sec);
}

/* ── Directory search ────────────────────────────────────────── */
i64 fat32_find_file_in(const char *name83, u32 *out_size, u32 dir_clus) {
    u32 clus = dir_clus ? dir_clus : fat32_root_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *ent = dir_buf + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5 || ent[11] == 0x0F) continue;
                if (memcmp(ent, name83, 11) == 0) {
                    u32 start = (u32)((ent[21]<<24)|(ent[20]<<16)|(ent[27]<<8)|ent[26]);
                    if (out_size) *out_size = (u32)(ent[28]|(ent[29]<<8)|(ent[30]<<16)|(ent[31]<<24));
                    return (i64)start;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_find_file(const char *name83, u32 *out_size) {
    return fat32_find_file_in(name83, out_size, 0);
}

/* ── File create / delete ────────────────────────────────────── */
static void write_dir_entry_in(u8 *entry32, u32 dir_clus) {
    u32 clus = dir_clus ? dir_clus : fat32_root_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *ent = dir_buf + e * 32;
                if (ent[0] == 0x00 || ent[0] == 0xE5) {
                    for (int b = 0; b < 32; b++) ent[b] = entry32[b];
                    ata_write_sector(lba + s, dir_buf);
                    return;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
}

void fat32_create_file_in(const char *name83, u32 dir_clus) {
    u8 entry[32] = {0};
    for (int i = 0; i < 11; i++) entry[i] = (u8)name83[i];
    entry[11] = 0x20;   /* archive attribute */
    u16 date = rtc_fat_date();
    u16 time = rtc_fat_time();
    entry[14] = (u8)(time & 0xFF); entry[15] = (u8)(time >> 8);
    entry[16] = (u8)(date & 0xFF); entry[17] = (u8)(date >> 8);
    entry[18] = (u8)(date & 0xFF); entry[19] = (u8)(date >> 8);
    entry[22] = (u8)(time & 0xFF); entry[23] = (u8)(time >> 8);
    entry[24] = (u8)(date & 0xFF); entry[25] = (u8)(date >> 8);
    write_dir_entry_in(entry, dir_clus);
}

void fat32_create_file(const char *name83) {
    fat32_create_file_in(name83, 0);
}

void fat32_create_dir_entry_in(const char *name83, u32 cluster, u32 dir_clus) {
    u8 entry[32] = {0};
    for (int i = 0; i < 11; i++) entry[i] = (u8)name83[i];
    entry[11] = 0x10;   /* directory attribute */
    entry[20] = (u8)((cluster >> 16) & 0xFF);
    entry[21] = (u8)((cluster >> 24) & 0xFF);
    entry[26] = (u8)(cluster & 0xFF);
    entry[27] = (u8)((cluster >> 8) & 0xFF);
    write_dir_entry_in(entry, dir_clus);
}

void fat32_create_dir_entry(const char *name83, u32 cluster) {
    fat32_create_dir_entry_in(name83, cluster, 0);
}

i64 fat32_delete_file_in(const char *name83, u32 dir_clus) {
    u32 clus = dir_clus ? dir_clus : fat32_root_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *ent = dir_buf + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5 || ent[11] == 0x0F) continue;
                if (memcmp(ent, name83, 11) == 0) {
                    /* Free cluster chain */
                    u32 fc = (u32)((ent[21]<<24)|(ent[20]<<16)|(ent[27]<<8)|ent[26]);
                    while (fc && fc < 0x0FFFFFF8) {
                        u32 next = fat32_next_cluster(fc);
                        u32 fo = fc * 4, fs = fat32_fat_start + fo / 512, feo = fo % 512;
                        fat_read_cached(fs);
                        fat_buf[feo]=fat_buf[feo+1]=fat_buf[feo+2]=fat_buf[feo+3]=0;
                        fat_write_cached(fs);
                        fc = next;
                    }
                    ent[0] = 0xE5;   /* mark deleted */
                    ata_write_sector(lba + s, dir_buf);
                    return 0;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_delete_file(const char *name83) {
    return fat32_delete_file_in(name83, 0);
}

i64 fat32_rename_in(const char *old83, const char *new83, u32 dir_clus) {
    u32 clus = dir_clus ? dir_clus : fat32_root_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *ent = dir_buf + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5 || ent[11] == 0x0F) continue;
                if (memcmp(ent, old83, 11) == 0) {
                    for (int b = 0; b < 11; b++) ent[b] = (u8)new83[b];
                    ata_write_sector(lba + s, dir_buf);
                    return 0;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_rename(const char *old83, const char *new83) {
    return fat32_rename_in(old83, new83, 0);
}
