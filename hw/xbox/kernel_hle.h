/*
 * QEMU Xbox Kernel High-Level Emulation implementation
 *
 * Copyright (c) 2015 Jannik Vogel
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This HLE works by patching the kernel.
 * Future versions will hopefully be able to provide a fake in-memory kernel
 * image and all necessary kernel functions to emulate games.
 * At the moment the patching is done which will:
 *
 *  - Expand the xboxkrnl PE image
 *  - Add code in the new xboxkrnl space which accesses memory-IO regions
 *    (Code such as: `exp1: movb <addr+0>, 0; exp2: movb <addr+1>, 0; ...` )
 *  - Patch the xboxkrnl export table to point to the access-code.
 *    (Export 1 points to `exp1`; Export 2 points to `exp 2`; ...)
 *    For dynamic variables the export will to point another watched region.
 *    static variable data is interleaved with the access-code.
 *  - QEMU will handle those accesses and emulate the kernel functions and
 *    variable updates accordingly
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include "exec/gdbstub.h"
#include "sysemu/kvm.h"

#define ARRAYSIZE(x) (sizeof(x)/sizeof(x[0]))

static MemoryRegion *ram; //FIXME: This is also accessible elsewhere? */

typedef void(*MemoryCallback)(hwaddr addr, uint8_t* buf,
                              unsigned int size, bool is_write);

typedef struct {
    const char* title;
    void(*callback)(void);
    const uint8_t* data;
    union {
        size_t data_size;
        const char* type;
    };
} KernelExport;

#define EXPORT_STDCALL(function, return_type, argument_types) {.title=#function,.callback=kernel_hle_##function,.data=NULL,.type=#return_type #argument_types}
#define EXPORT_STATIC_VAR(variable, size, bytes) {.title=#variable,.callback=NULL,.data=bytes,.data_size=size}
#define EXPORT_DYNAMIC_VAR(variable, size) {.title=#variable,.callback=kernel_hle_##variable,.data={0},.data_size=size}

void kernel_hle_IoCreateFile(void) {
    assert(false);

    CPUState *cpu = first_cpu;
    CPUArchState *env = cpu->env_ptr;

/* Get stack pointer */
    uint32_t stack = env->regs[R_ESP];
#if 0
    /* Pop return address */
    uint32_t return_addr = read32(stack);
    stack += 4;

    /* Inspect possible arguments */
    int i;
    for (i = 0; i < 10; i++) {

        uint32_t value = read32(stack);
        stack += 4;

        uint32_t pstr = read32(value + 8);
        pstr = read32(pstr + 8);
        uint8_t str[32] = {0};
        readBytes(str, pstr, sizeof(str));
        printf("%i. 0x%08X '%.*s'\n", i, value, (int)sizeof(str), str);
    }
#endif
usleep(1000*1000);

}

KernelExport kernel_exports[] = {
    /* ... */
    [66] = EXPORT_STDCALL(IoCreateFile, FOO, FOO),
#if 0
    /* ... */
    [75] = EXPORT_STDCALL(IoQueryFileInformation);
    /* ... */
    [154] = EXPORT_DYNAMIC_VAR(KeSystemTime, 8), //FIXME: Size?
    /* ... */
    [272] = EXPORT_STDCALL(RtlCopyString),
    /* ... */
    [279] = EXPORT_STDCALL(RtlEqualString),
    /* ... */
    [321] = EXPORT_STATIC_VAR(XboxEEPROMKey, 16),
    /* ... */
    [361] = EXPORT_STDCALL(RtlSnprintf),
    /* ... */
    [362] = EXPORT_STDCALL(RtlSprintf)
    /* ... */
#endif
};

/* The xboxkrnl PE is loaded to this location */
static const hwaddr xboxkrnl_base = 0x80010000;

uint8_t kernel_ready_value[] = { 0xEF, 0xBE, 0xAD, 0xDE  };
static const hwaddr kernel_ready_addr = 0;  //0x00103EC+4; // Physical mem addr

/* This function expands the xboxkrnl so we have enough room for the hooks */
static hwaddr expand_xboxkrnl(uint64_t expansion_size) {
#if 0
    uintptr_t pe = xboxkrnl_base;
    uintptr_t peHeader = pe + *(uint32_t*)(pe+0x3C);

    uint16_t numberOfSections = *(uint16_t*)(peHeader+0x6);
    uint16_t sizeOfOptionalHeader = *(uint16_t*)(peHeader+0x14);

    uintptr_t section = peHeader + 0x18 + sizeOfOptionalHeader + (numberOfSections-2)*0x28;

    // This is probably some .text or .data section?!
    uint32_t* s0VirtualSize = (uint32_t*)(section+0x8);
    uint32_t* s0SizeOfRawData = (uint32_t*)(section+0x10);
    *s0VirtualSize += KI_expansion_size;			//; VirtualSize
    *s0SizeOfRawData += KI_expansion_size;			//; SizeOfRawData

    // Last section is always init in xbox kernel
    uint32_t* s1VirtualSize = (uint32_t*)(section+0x28+0x8);
    uint32_t* s1SizeOfRawData = (uint32_t*)(section+0x28+0x10);
    uint32_t* s1VirtualAddress = (uint32_t*)(section+0x28+0xC);
    uint32_t* s1PointerToRawData = (uint32_t*)(section+0x28+0x14);
    *s1VirtualSize -= KI_expansion_size;			// INIT section VirtualSize 
    *s1SizeOfRawData -= KI_expansion_size;		// INIT section SizeOfRawData
    printf("New .init size is 0x%08X / 0x%08X\n",*s1VirtualSize,*s1SizeOfRawData);
    *s1VirtualAddress += KI_expansion_size;  // INIT section VirtualAddress
    *s1PointerToRawData += KI_expansion_size;// INIT section PointerToRawData

    /* FIXME: Expand CS/DS */
#endif
    return xboxkrnl_base + 0x2000;
}

static void copy_ram(hwaddr addr, uint8_t* buf,
                     unsigned int size, bool is_write) {
    uint8_t* ram_ptr = memory_region_get_ram_ptr(ram);
    if (is_write) {
        memcpy(ram_ptr + addr, buf, size);
    } else {
        memcpy(buf, ram_ptr + addr, size);
    }
}

static uint64_t fix_read_alignment(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    int i;
    uint8_t buf[8];
    uint64_t val = 0;
    MemoryCallback callback = (MemoryCallback)opaque;

    assert((addr & 3 + size) < 8);

//FIXME
    callback(addr, buf, size, false);
    for (i = 0; i < size; i++) {
        val |= buf[i] << ((i + addr & 3) * 8);
    }
    return val;
}

static void fix_write_alignment(void *opaque, hwaddr addr,
                                uint64_t val, unsigned int size)
{
    int i;
    uint8_t buf[8];
    MemoryCallback callback = (MemoryCallback)opaque;

    assert((addr & 3 + size) < 8);

//FIXME
    printf("Writing 0x%" PRIx64 " size %d, address %d\n", val, size, addr);
    for (i = 0; i < size; i++) {
        buf[i] = (val >> ((i + addr & 3) * 8)) & 0xFF;
        printf("buf[%i] = 0x%02X\n", i, buf[i]);
    }
    callback(addr, buf, size, true);
}

static const MemoryRegionOps fix_alignment_ops = {
    .read = fix_read_alignment,
    .write = fix_write_alignment
};

static MemoryRegion* add_memory_io_handler(const char* name, hwaddr addr,
                                           uint64_t size, MemoryCallback callback)
{
    MemoryRegion* mr = g_malloc(sizeof(MemoryRegion));
    memory_region_init_io(mr, NULL, &fix_alignment_ops, callback, name, size);
    memory_region_add_subregion_overlap(get_system_memory(), addr, mr, 1);
    return mr;
}

static uint8_t* readBytes(uint8_t* buf, hwaddr addr, size_t len)
{
    cpu_memory_rw_debug(first_cpu, addr, buf, len, false);
    return buf;
}

static uint32_t read32(hwaddr addr)
{
    uint32_t buf;
    readBytes((uint8_t*)&buf, addr, 4);
    return buf;
}

static void writeBytes(uint8_t* buf, hwaddr addr, size_t len)
{
    cpu_memory_rw_debug(first_cpu, addr, buf, len, true);
}

static void write8(hwaddr addr, uint8_t val)
{
    writeBytes(&val, addr, 1);
}

static void write32(hwaddr addr, uint32_t val)
{
    writeBytes((uint8_t*)&val, addr, 4);
}

static hwaddr locate_export_table(unsigned int* count)
{
    uint32_t tmp = read32(xboxkrnl_base + 0x3C);
    tmp = read32(xboxkrnl_base + tmp + 0x78);
    if (count) {
        *count = read32(xboxkrnl_base + tmp + 0x14);
    }
    return xboxkrnl_base + read32(xboxkrnl_base + tmp + 0x1C);
}

void hook_stdcall_function(void) {
    CPUState *cpu = first_cpu;
    CPUArchState *env = cpu->env_ptr;

    /* Make sure our regs are up-to-date FIXME: Is this necessary? */
    cpu_synchronize_state(cpu);

}

void hook_variable() {
    /* FIXME: Update */
}

static void kernel_exports_cb(hwaddr addr, uint8_t* buf,
                              unsigned int size, bool is_write)
{
    int i;
    CPUState *cpu = first_cpu;
    CPUArchState *env = cpu->env_ptr;
    uint8_t* ram_ptr = memory_region_get_ram_ptr(ram);

assert(false);

    unsigned int export_count;
    hwaddr export_table = locate_export_table(&export_count) & 0x7FFFFFFF;
    uint32_t* export_table_entries = (uint32_t*)&ram_ptr[export_table];
    for (i = 0; i < export_count; i++) {
        if (i > ARRAY_SIZE(kernel_exports)) { break; }
        if (addr == export_table_entries[i]) {
            KernelExport* ke = &kernel_exports[i + 1];
            if (ke->callback) {
                ke->callback();
            }
            return;
        }
    }

    fprintf(stderr, "Unable to find kernel export. Accessed offset %d\n", addr);
    assert(false); /* Unknown kernel export */

}

static void process_export_table(hwaddr export_table, unsigned int export_count,
                                 hwaddr buf,
                                 uint64_t* dynamic_size, uint64_t* static_size)
{
    int i;
    uint8_t* ram_ptr = memory_region_get_ram_ptr(ram);
    uint8_t* buf_ptr = NULL;
    if (buf != 0) {
        buf_ptr = &ram_ptr[buf];
    }
    uint32_t* export_table_entries = NULL;
    if (export_table != 0) {
        export_table_entries = (uint32_t*)&ram_ptr[export_table];
    }
    uint64_t dynamic_cursor = 0;
    uint64_t static_cursor = 0;

    /* Skip over future dynamic section */
    for (i = 0; i < export_count; i++) {
        if ((i + 1) >= ARRAYSIZE(kernel_exports)) { break; }
        KernelExport* ke = &kernel_exports[i + 1];
        if (!ke->title) { continue; }
        if (ke->callback) {
            static_cursor += ke->data ? ke->data_size : 1;
        }
    }
    for (i = 0; i < export_count; i++) {
        if ((i + 1) >= ARRAYSIZE(kernel_exports)) { break; }
        KernelExport* ke = &kernel_exports[i + 1];
        if (!ke->title) { continue; }
        if (ke->data) {
            if (ke->callback) {
                /* Dynamic variable! */
                dynamic_cursor += ke->data_size;
                if (export_table_entries && buf != 0) {
                    export_table_entries[i] = buf + dynamic_cursor - 0x10000;
                }
            } else {
                /* Static variable! */
                if (buf_ptr) {
                    memcpy(buf_ptr, ke->data, ke->data_size);
                }
                if (export_table_entries && buf != 0) {
                    export_table_entries[i] = buf + static_cursor - 0x10000;
                }
                static_cursor += ke->data_size;
            }
        } else {
            /* Function */
            if (buf_ptr) {
                /* movb <dynamic_cursor>, <imm8> */
/*
                buf_ptr[static_cursor + 0] = 0xC6;
                buf_ptr[static_cursor + 1] = 0x05;
                *(uint32_t*)&buf_ptr[static_cursor + 2] = buf + 1
                                                              + dynamic_cursor
                                                              - static_cursor;
*/
write8(buf + static_cursor + 0, 0xC6);
write8(buf + static_cursor + 1, 0x05);
write32(buf + static_cursor + 2, buf + dynamic_cursor - static_cursor + 1);
printf("\n\n\n\nWriting code to %X\n\n\n\n\n", buf + static_cursor);
                /* We'll use the next byte in mem as imm8 */
            }
            if (export_table_entries && buf != 0) {
                export_table_entries[i] = buf + static_cursor - 0x10000;
assert(buf > xboxkrnl_base);
write32(locate_export_table(NULL) + i * 4, buf + static_cursor - xboxkrnl_base);
            }
            static_cursor += 6;
            dynamic_cursor += 1;
        }
    }
    if (dynamic_size) {
        *dynamic_size = dynamic_cursor;
    }
    if (static_size) {
        *static_size = static_cursor - dynamic_cursor;
    }
}

static void kernel_ready_cb(hwaddr addr, uint8_t* buf,
                            unsigned int size, bool is_write)
{
    printf("Checking kernel ready!\n");

    /* Punch access through to RAM */
    copy_ram(kernel_ready_addr + addr, buf, size, is_write);

    uint8_t* ram_ptr = memory_region_get_ram_ptr(ram);
    uint8_t* data = &ram_ptr[kernel_ready_addr];
    if (memcmp(data, kernel_ready_value, size)) {
        int i;
        for (i = 0; i < size; i++) {
            printf("[%d] = 0x%02X (Expected 0x%02X)\n", i, data[i], kernel_ready_value[i]);
        }
        return;
    }

    printf("Kernel is ready!\n");
    usleep(1000*1000);

    /* Gather size information about our patch */
    //FIXME: Cache this information
    uint64_t dynamic_size, static_size;
    unsigned int export_count;
    hwaddr export_table = locate_export_table(&export_count) & 0x7FFFFFFF;
    process_export_table(0, export_count, 0, &dynamic_size, &static_size);

    /* Expand the kernel image */
    uint64_t expansion_size = dynamic_size + static_size;
    printf("Need %d + %d = %d bytes\n", static_size, dynamic_size, expansion_size);
    hwaddr expansion_buffer = expand_xboxkrnl(expansion_size);

    /* Apply our patch */
    process_export_table(export_table, export_count, expansion_buffer, NULL, NULL);
    printf("Export table patched\n");

    /* Overlay for the dynamic variables and function access targets */
    add_memory_io_handler("kernel-hle-exports-cb",
                          expansion_buffer, dynamic_size,
                          kernel_exports_cb);
    printf("Watching 0x%X, %d byte(s)\n", expansion_buffer, dynamic_size);

}

void init_kernel_hle(MemoryRegion *_ram)
{
    ram = _ram;

    /* Call us back when the kernel is uncompressed */
    add_memory_io_handler("kernel-hle-ready",
                          kernel_ready_addr, sizeof(kernel_ready_value),
                          kernel_ready_cb);
}
