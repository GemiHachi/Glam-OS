/**
 * @file swift_elf_loader.c
 * @brief Xtensa ELF32 Dynamic PSRAM Loader for ESP32-S3
 *
 * Allocates a 64-byte aligned block in Octal PSRAM (DBUS 0x3Cxxxxxx), maps the
 * instruction bus alias (IBUS 0x42xxxxxx), loads SHF_ALLOC sections, relocates
 * internal R_XTENSA_32 pointers and whitelisted external OS symbols, flushes
 * DCache (C2M) and invalidates ICache (M2C) via esp_cache_msync(), and executes
 * directly from PSRAM.
 */

#include "swift_elf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "swift_elf";

/* Standard ELF32 constants & headers */
#define EI_NIDENT       16
#define EI_CLASS        4
#define EI_DATA         5
#define ELFCLASS32      1
#define ELFDATA2LSB     1
#define ET_REL          1
#define ET_EXEC         2
#define ET_DYN          3
#define EM_XTENSA       94

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8

#define SHF_WRITE       0x1U
#define SHF_ALLOC       0x2U
#define SHF_EXECINSTR   0x4U

#define SHN_UNDEF       0

#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_RTLD       2
#define R_XTENSA_JMP_SLOT   4
#define R_XTENSA_RELATIVE   5
#define R_XTENSA_SLOT0_OP   20

#define MAX_ELF_FILE_SIZE   (2 * 1024 * 1024)
#define MAX_IMAGE_SIZE      (2 * 1024 * 1024)

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) elf32_shdr_t;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} __attribute__((packed)) elf32_sym_t;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} __attribute__((packed)) elf32_rela_t;

static void set_err(char *buf, size_t len, const char *msg)
{
    if (buf && len > 0) {
        snprintf(buf, len, "%s", msg ? msg : "unknown error");
    }
    ESP_LOGE(TAG, "%s", msg ? msg : "unknown error");
}

static uint32_t read_u32_unaligned(const void *p)
{
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write_u32_unaligned(void *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static bool is_valid_elf_header(const elf32_ehdr_t *ehdr, size_t elf_len, char *err, size_t err_len)
{
    if (elf_len < sizeof(elf32_ehdr_t)) {
        set_err(err, err_len, "ELF buffer shorter than header");
        return false;
    }
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        set_err(err, err_len, "Bad ELF magic");
        return false;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        set_err(err, err_len, "Only 32-bit little-endian ELF supported");
        return false;
    }
    if (ehdr->e_machine != EM_XTENSA) {
        set_err(err, err_len, "Not an ESP32-S3 (Xtensa) ELF");
        return false;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        set_err(err, err_len, "Unsupported ELF e_type (link with -Wl,-q)");
        return false;
    }
    if (ehdr->e_shentsize != sizeof(elf32_shdr_t) || ehdr->e_shnum == 0 ||
        ehdr->e_shoff >= elf_len ||
        (size_t)ehdr->e_shoff + (size_t)ehdr->e_shnum * sizeof(elf32_shdr_t) > elf_len) {
        set_err(err, err_len, "Corrupt ELF section header table");
        return false;
    }
    return true;
}

esp_err_t swift_elf_load_and_run_mem(swift_elf_app_t *ctx,
                                      const char *app_id,
                                      const uint8_t *elf_data,
                                      size_t elf_len,
                                      lv_obj_t *app_root,
                                      char *err_msg,
                                      size_t err_msg_len)
{
    if (!ctx || !app_id || !elf_data || !app_root) {
        set_err(err_msg, err_msg_len, "Invalid loader args");
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->loaded) {
        swift_elf_unload(ctx);
    }
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->app_id, sizeof(ctx->app_id), "%s", app_id);

    if (elf_len > MAX_ELF_FILE_SIZE) {
        set_err(err_msg, err_msg_len, "ELF exceeds 2MB safety cap");
        return ESP_ERR_INVALID_SIZE;
    }

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)elf_data;
    if (!is_valid_elf_header(ehdr, elf_len, err_msg, err_msg_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    const elf32_shdr_t *shdrs = (const elf32_shdr_t *)(elf_data + ehdr->e_shoff);
    uint32_t min_vaddr = UINT32_MAX;
    uint32_t max_vaddr = 0;

    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if ((shdrs[i].sh_flags & SHF_ALLOC) == 0) {
            continue;
        }
        if (shdrs[i].sh_addr < min_vaddr) {
            min_vaddr = shdrs[i].sh_addr;
        }
        uint32_t end_vaddr = shdrs[i].sh_addr + shdrs[i].sh_size;
        if (end_vaddr > max_vaddr) {
            max_vaddr = end_vaddr;
        }
        if (shdrs[i].sh_type != SHT_NOBITS && shdrs[i].sh_size > 0) {
            if ((size_t)shdrs[i].sh_offset + shdrs[i].sh_size > elf_len) {
                set_err(err_msg, err_msg_len, "Section extends past EOF");
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }

    if (min_vaddr != 0 || max_vaddr == 0 || max_vaddr > MAX_IMAGE_SIZE) {
        set_err(err_msg, err_msg_len, "Invalid ELF memory layout (must start at 0x0)");
        return ESP_ERR_INVALID_ARG;
    }

    size_t alloc_size = (max_vaddr + 63U) & ~63U;
    uint8_t *dbus_base = (uint8_t *)heap_caps_aligned_alloc(64, alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dbus_base) {
        set_err(err_msg, err_msg_len, "PSRAM alloc failed");
        return ESP_ERR_NO_MEM;
    }

    uintptr_t dbus_u = (uintptr_t)dbus_base;
    if (dbus_u < 0x3C000000U || dbus_u >= 0x3E000000U) {
        heap_caps_free(dbus_base);
        set_err(err_msg, err_msg_len, "Allocated pointer outside PSRAM DBUS window");
        return ESP_ERR_INVALID_STATE;
    }

    uintptr_t ibus_base = (dbus_u & 0x01FFFFFFU) | 0x42000000U;
    memset(dbus_base, 0, alloc_size);

    /* 1. Copy SHF_ALLOC PROGBITS sections into PSRAM */
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if ((shdrs[i].sh_flags & SHF_ALLOC) && shdrs[i].sh_type != SHT_NOBITS && shdrs[i].sh_size > 0) {
            memcpy(dbus_base + shdrs[i].sh_addr, elf_data + shdrs[i].sh_offset, shdrs[i].sh_size);
        }
    }

    /* 2. Apply SHT_RELA relocations for SHF_ALLOC target sections only */
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_RELA) {
            continue;
        }
        uint32_t target_idx = shdrs[i].sh_info;
        uint32_t symtab_idx = shdrs[i].sh_link;
        if (target_idx >= ehdr->e_shnum || symtab_idx >= ehdr->e_shnum) {
            heap_caps_free(dbus_base);
            set_err(err_msg, err_msg_len, "Bad RELA section indices");
            return ESP_ERR_INVALID_ARG;
        }
        const elf32_shdr_t *target_sh = &shdrs[target_idx];
        if ((target_sh->sh_flags & SHF_ALLOC) == 0) {
            continue; /* Skip .xt.prop, .xt.lit, .debug_* relocations */
        }

        const elf32_shdr_t *sym_sh = &shdrs[symtab_idx];
        uint32_t strtab_idx = sym_sh->sh_link;
        if (strtab_idx >= ehdr->e_shnum ||
            (size_t)shdrs[i].sh_offset + shdrs[i].sh_size > elf_len ||
            (size_t)sym_sh->sh_offset + sym_sh->sh_size > elf_len ||
            (size_t)shdrs[strtab_idx].sh_offset + shdrs[strtab_idx].sh_size > elf_len) {
            heap_caps_free(dbus_base);
            set_err(err_msg, err_msg_len, "Corrupt symtab/strtab bounds");
            return ESP_ERR_INVALID_ARG;
        }

        const elf32_rela_t *relas = (const elf32_rela_t *)(elf_data + shdrs[i].sh_offset);
        size_t rela_count = shdrs[i].sh_size / sizeof(elf32_rela_t);
        const elf32_sym_t *syms = (const elf32_sym_t *)(elf_data + sym_sh->sh_offset);
        size_t sym_count = sym_sh->sh_size / sizeof(elf32_sym_t);
        const char *strtab = (const char *)(elf_data + shdrs[strtab_idx].sh_offset);
        size_t strtab_sz = shdrs[strtab_idx].sh_size;

        for (size_t j = 0; j < rela_count; j++) {
            uint32_t r_off = relas[j].r_offset;
            uint8_t r_type = (uint8_t)(relas[j].r_info & 0xFFU);
            uint32_t r_sym = relas[j].r_info >> 8;

            if (r_type == R_XTENSA_NONE) {
                continue;
            }
            if (r_sym >= sym_count) {
                heap_caps_free(dbus_base);
                set_err(err_msg, err_msg_len, "Relocation symbol out of range");
                return ESP_ERR_INVALID_ARG;
            }
            const elf32_sym_t *sym = &syms[r_sym];

            if (r_type == R_XTENSA_SLOT0_OP) {
                if (sym->st_shndx == SHN_UNDEF) {
                    heap_caps_free(dbus_base);
                    set_err(err_msg, err_msg_len, "External SLOT0_OP unsupported (compile with -mlongcalls)");
                    return ESP_ERR_NOT_SUPPORTED;
                }
                /* Contiguous image: PC-relative CALL/L32R/branch offsets remain valid as linked */
                continue;
            }

            if (r_off + sizeof(uint32_t) > max_vaddr) {
                heap_caps_free(dbus_base);
                set_err(err_msg, err_msg_len, "Relocation offset outside image");
                return ESP_ERR_INVALID_ARG;
            }

            if (r_type == R_XTENSA_32 || r_type == R_XTENSA_JMP_SLOT || r_type == R_XTENSA_RELATIVE) {
                if (sym->st_shndx == SHN_UNDEF) {
                    if (sym->st_name >= strtab_sz) {
                        heap_caps_free(dbus_base);
                        set_err(err_msg, err_msg_len, "Bad symbol name offset");
                        return ESP_ERR_INVALID_ARG;
                    }
                    const char *sym_name = strtab + sym->st_name;
                    const void *os_sym = swift_os_lookup_symbol(sym_name);
                    if (!os_sym) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Blocked or unknown symbol: %s", sym_name);
                        heap_caps_free(dbus_base);
                        set_err(err_msg, err_msg_len, msg);
                        return ESP_ERR_NOT_SUPPORTED;
                    }
                    write_u32_unaligned(dbus_base + r_off, (uint32_t)(uintptr_t)os_sym);
                } else {
                    if (sym->st_shndx >= ehdr->e_shnum) {
                        heap_caps_free(dbus_base);
                        set_err(err_msg, err_msg_len, "Invalid symbol section index");
                        return ESP_ERR_INVALID_ARG;
                    }
                    const elf32_shdr_t *sym_sec = &shdrs[sym->st_shndx];
                    /* ET_EXEC with ld -q already stores the exact linked offset in place */
                    uint32_t val = (ehdr->e_type == ET_EXEC)
                                   ? read_u32_unaligned(dbus_base + r_off)
                                   : (uint32_t)((int32_t)sym->st_value + relas[j].r_addend);
                    uintptr_t base_addr = (sym_sec->sh_flags & SHF_EXECINSTR) ? ibus_base : dbus_u;
                    write_u32_unaligned(dbus_base + r_off, (uint32_t)(base_addr + val));
                }
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "Unsupported reloc type %u", (unsigned)r_type);
                heap_caps_free(dbus_base);
                set_err(err_msg, err_msg_len, msg);
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
    }

    /* 3. Flush Data Cache to PSRAM (C2M) and Invalidate Instruction Cache (M2C) */
    esp_err_t c_err = esp_cache_msync(dbus_base, alloc_size,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (c_err != ESP_OK) {
        heap_caps_free(dbus_base);
        set_err(err_msg, err_msg_len, "D-Cache C2M sync failed");
        return c_err;
    }

    c_err = esp_cache_msync((void *)ibus_base, alloc_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (c_err != ESP_OK) {
        heap_caps_free(dbus_base);
        set_err(err_msg, err_msg_len, "I-Cache M2C sync failed");
        return c_err;
    }

    if (ehdr->e_entry >= max_vaddr) {
        heap_caps_free(dbus_base);
        set_err(err_msg, err_msg_len, "Invalid ELF entry offset");
        return ESP_ERR_INVALID_ARG;
    }

    ctx->dbus_base = dbus_base;
    ctx->ibus_base = ibus_base;
    ctx->alloc_size = alloc_size;
    ctx->loaded = true;

    swift_app_entry_fn_t entry_fn = (swift_app_entry_fn_t)(ibus_base + ehdr->e_entry);
    ESP_LOGI(TAG, "Launching %s: DBUS=%p IBUS=0x%08x Size=%uB Entry=0x%08x",
             app_id, dbus_base, (unsigned)ibus_base, (unsigned)alloc_size, (unsigned)(uintptr_t)entry_fn);

    const swift_app_desc_t *desc = entry_fn();
    if (!desc || !desc->on_create) {
        swift_elf_unload(ctx);
        set_err(err_msg, err_msg_len, "ELF entry returned invalid app descriptor");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ctx->desc = desc;

    int rc = desc->on_create(swift_os_abi_get(), (swift_obj_t)app_root, app_id, "/sdcard/data");
    if (rc != 0) {
        swift_elf_unload(ctx);
        set_err(err_msg, err_msg_len, "App on_create returned error");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t swift_elf_load_and_run_file(swift_elf_app_t *ctx,
                                       const char *app_id,
                                       const char *elf_path,
                                       lv_obj_t *app_root,
                                       char *err_msg,
                                       size_t err_msg_len)
{
    if (!elf_path) {
        set_err(err_msg, err_msg_len, "Null ELF path");
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(elf_path, "rb");
    if (!f) {
        set_err(err_msg, err_msg_len, "Failed to open app.elf");
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > MAX_ELF_FILE_SIZE) {
        fclose(f);
        set_err(err_msg, err_msg_len, "Invalid app.elf size");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *file_buf = (uint8_t *)heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file_buf) {
        fclose(f);
        set_err(err_msg, err_msg_len, "PSRAM temp buffer OOM");
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(file_buf, 1, (size_t)fsz, f);
    fclose(f);
    if (got != (size_t)fsz) {
        heap_caps_free(file_buf);
        set_err(err_msg, err_msg_len, "Failed reading app.elf");
        return ESP_FAIL;
    }

    esp_err_t ret = swift_elf_load_and_run_mem(ctx, app_id, file_buf, (size_t)fsz,
                                                app_root, err_msg, err_msg_len);
    heap_caps_free(file_buf);
    return ret;
}

void swift_elf_unload(swift_elf_app_t *ctx)
{
    if (!ctx || !ctx->loaded) {
        return;
    }
    ESP_LOGI(TAG, "Unloading %s (freeing %uB PSRAM)", ctx->app_id, (unsigned)ctx->alloc_size);
    if (ctx->desc && ctx->desc->on_destroy) {
        ctx->desc->on_destroy();
    }
    swift_os_abi_cleanup_app_resources();
    if (ctx->dbus_base) {
        heap_caps_free(ctx->dbus_base);
    }
    memset(ctx, 0, sizeof(*ctx));
}
