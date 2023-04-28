/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <binaries/efi/efi.h>
#include <elfloader_common.h>
#include <stdbool.h>

void *__application_handle = NULL;             // current efi application handler
efi_system_table_t *__efi_system_table = NULL; // current efi system table

static unsigned long efi_exit_bs_result = EFI_SUCCESS;
static unsigned long exit_boot_services(void);

unsigned long efi_exit_boot_services(void)
{
    return efi_exit_bs_result;
}

extern void _start(void);
unsigned int efi_main(uintptr_t application_handle, uintptr_t efi_system_table)
{
    clear_bss();
    __application_handle = (void *)application_handle;
    __efi_system_table = (efi_system_table_t *)efi_system_table;
    efi_exit_bs_result = exit_boot_services();
    _start();
    return 0;
}

void *efi_get_fdt(void)
{
    efi_guid_t fdt_guid = make_efi_guid(0xb1b621d5, 0xf19c, 0x41a5,  0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0);
    efi_config_table_t *tables = (efi_config_table_t *)__efi_system_table->tables;

    for (uint32_t i = 0; i < __efi_system_table->nr_tables; i++) {
        if (!efi_guideq(fdt_guid, tables[i].guid))
            continue;

        return (void *)tables[i].table;
    }

    return NULL;
}

static inline bool is_useable_memory(uint32_t type)
{
    /* UEFI spec 2.10, 7.4.6. EFI_BOOT_SERVICES.ExitBootServices():
     * "On success, the UEFI OS loader owns all available memory in the system.
     *  In addition, the UEFI OS loader can treat all memory in the map marked as
     *  EfiBootServicesCode and EfiBootServicesData as available free memory."
     */
    switch(type) {
        case EFI_BOOT_SERVICES_CODE: /* fall-through */
        case EFI_BOOT_SERVICES_DATA: /* fall-through */
        case EFI_CONVENTIONAL_MEMORY:
            return true;

        default:
        break;
    }
    return false;
}

/* Before starting the kernel we should notify the UEFI firmware about it
 * otherwise the internal watchdog may reboot us after 5 min.
 *
 * This means boot time services are not available anymore. We should store
 * system information e.g. current memory map and pass them to kernel.
 */
static unsigned long exit_boot_services(void)
{
    unsigned long status;
    efi_memory_desc_t *memory_map;
    const efi_memory_desc_t *map_ent;
    unsigned long map_size;
    unsigned long desc_size, key;
    uint32_t desc_version;

    efi_boot_services_t *bts = get_efi_boot_services();

    /*
     * As the number of existing memory segments are unknown,
     * we need to start somewhere. The API then tells us how much space we need
     * if it is not enough.
     */
    map_size = sizeof(*memory_map) * 32;

    do {
        status = bts->allocate_pool(EFI_LOADER_DATA, map_size, (void **)&memory_map);
        /* If the allocation fails, there is something wrong and we cannot continue */
        if (status != EFI_SUCCESS) {
            return status;
        }

        status = bts->get_memory_map(&map_size, memory_map, &key, &desc_size, &desc_version);
        if (status != EFI_SUCCESS) {
            bts->free_pool(memory_map);
            memory_map = NULL;

            if (status == EFI_BUFFER_TOO_SMALL) {
                /* Note: "map_size" is an IN/OUT-parameter and has been updated to the
                 *       required size. We still add one more entry ("desc_size" is in bytes)
                 *       due to the hint from the spec ("since allocation of the new buffer
                 *       may potentially increase memory map size.").
                 */
                map_size += desc_size;
            } else {
                /* some other error; bail out! */
                return status;
            }
        }
    } while (status == EFI_BUFFER_TOO_SMALL);

    status = bts->exit_boot_services(__application_handle, key);
    /* Now that we're free, mask all exceptions until we enter the kernel */
    asm volatile("msr daifset, #0xF\n\t");

    unsigned long cnt = 0;
    for (unsigned long i = 0; i < map_size / desc_size; i++) {
        /* Use the advertised "desc_size", not sizeof(efi_memory_desc_t) */
        map_ent = (void *)memory_map + i * desc_size;
        if (is_useable_memory(map_ent->type)) {
            if (cnt == M_AVA_NUMS) {
                return EFI_UNUSABLE_MEMORY;
            }

            /* Check if we can merge with the previous region */
            if ((cnt != 0) &&
                (m_info.ava_regs[cnt-1].end == map_ent->phys_addr)) {
                m_info.ava_regs[cnt-1].end += (map_ent->num_pages << EFI_PAGE_BITS);
            } else {
                /* New entry */
                m_info.ava_regs[cnt].start = map_ent->phys_addr;
                m_info.ava_regs[cnt].end = map_ent->phys_addr + (map_ent->num_pages << EFI_PAGE_BITS);
                cnt++;
            }
        }
    }

    return status;
}
