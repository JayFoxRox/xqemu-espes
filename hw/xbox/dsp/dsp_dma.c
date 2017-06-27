/*
 * MCPX DSP DMA
 *
 * Copyright (c) 2015 espes
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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "dsp_dma.h"

#define DMA_CONFIGURATION_AUTOSTART (1 << 0)
#define DMA_CONFIGURATION_AUTOREADY (1 << 1)
#define DMA_CONFIGURATION_IOC_CLEAR (1 << 2)
#define DMA_CONFIGURATION_EOL_CLEAR (1 << 3)
#define DMA_CONFIGURATION_ERR_CLEAR (1 << 4)

#define DMA_CONTROL_ACTION 0x7
#define DMA_CONTROL_ACTION_NOP 0
#define DMA_CONTROL_ACTION_START 1
#define DMA_CONTROL_ACTION_STOP 2
#define DMA_CONTROL_ACTION_FREEZE 3
#define DMA_CONTROL_ACTION_UNFREEZE 4
#define DMA_CONTROL_ACTION_ABORT 5
#define DMA_CONTROL_FROZEN (1 << 3)
#define DMA_CONTROL_RUNNING (1 << 4)
#define DMA_CONTROL_STOPPED (1 << 5)

#define NODE_POINTER_VAL 0x3fff
#define NODE_POINTER_EOL (1 << 14)

#define NODE_CONTROL_DIRECTION (1 << 1)


// #define DEBUG
#ifdef DEBUG
# define DPRINTF(s, ...) printf(s, ## __VA_ARGS__)
#else
# define DPRINTF(s, ...) do { } while (0)
#endif

static uint32_t dsp_map_memory(DSPDMAState *s, uint32_t addr, int* mem_space)
{
    if (addr < 0x1800) {
        *mem_space = DSP_SPACE_X;
        return addr;
    } else if (addr >= 0x1800 && addr < 0x2000) { //?
        *mem_space = DSP_SPACE_Y;
        return addr - 0x1800;
    } else if (addr >= 0x2800 && addr < 0x3800) { //?
        *mem_space = DSP_SPACE_P;
        return addr - 0x2800;
    } else {
        assert(false);
    }
}

static uint32_t dsp_read_mapped_memory(DSPDMAState *s, uint32_t addr)
{
    int mem_space;
    addr = dsp_map_memory(s, addr, &mem_space);
    return dsp56k_read_memory(s->core, mem_space, addr);
}

static void dsp_write_mapped_memory(DSPDMAState *s, uint32_t addr, uint32_t value)
{
    int mem_space;
    addr = dsp_map_memory(s, addr, &mem_space);
    dsp56k_write_memory(s->core, mem_space, addr, value);
}

static void dsp_dma_run(DSPDMAState *s)
{
    if (!(s->control & DMA_CONTROL_RUNNING)
        || (s->control & DMA_CONTROL_FROZEN)) {
        return;
    }
    while (!(s->next_block & NODE_POINTER_EOL)) {
        uint32_t addr = s->next_block & NODE_POINTER_VAL;
        assert((addr+6) < sizeof(s->core->xram));

        uint32_t next_block = dsp_read_mapped_memory(s, addr);
        uint32_t control = dsp_read_mapped_memory(s, addr+1);
        uint32_t count = dsp_read_mapped_memory(s, addr+2);
        uint32_t dsp_offset = dsp_read_mapped_memory(s, addr+3);
        uint32_t scratch_offset = dsp_read_mapped_memory(s, addr+4);
        uint32_t scratch_base = dsp_read_mapped_memory(s, addr+5);
        uint32_t scratch_size = dsp_read_mapped_memory(s, addr+6)+1;

        s->next_block = next_block;
        if (s->next_block & NODE_POINTER_EOL) {
            s->eol = true;
        }


        DPRINTF("\n\n\nDMA addr %x, control %x, count %x, "
                 "dsp_offset %x, scratch_offset %x, base %x, size %x\n\n\n",
                addr, control, count, dsp_offset,
                scratch_offset, scratch_base, scratch_size);

        uint32_t format = (control >> 10) & 7;
        unsigned int item_size;
        uint32_t item_mask = 0xffffffff;
        switch(format) {
        case 1:
            item_size = 2;
            break;
        case 2: //big-endian?
        case 6:
            item_size = 4;
            item_mask = 0x00FFFFFF;
            break;
        default:
            fprintf(stderr, "Unknown dsp dma format: 0x%x\n", format);
            assert(false);
            break;
        }

        uint32_t buf_id = (control >> 5) & 0xf;

        size_t scratch_addr;
        if (buf_id == 0xe) { // 'circular'?
            // assert(scratch_offset == 0);
            // assert(scratch_offset + count * item_size < scratch_size);
            if (scratch_offset + count * item_size >= scratch_size) {
                // This happens during the startup sound effect.
                // I think it might actually be a bug in the code...
                DPRINTF("skipping bad dma...\n");
                continue;
            }
            scratch_addr = scratch_base + scratch_offset; //??
        } else {
            // assert(buf_id == 0xf) // 'offset'
            scratch_addr = scratch_offset;
        }

        uint8_t* scratch_buf = calloc(count, item_size);

        if (control & NODE_CONTROL_DIRECTION) {
            int i;
            for (i=0; i<count; i++) {
                uint32_t v = dsp_read_mapped_memory(s, dsp_offset+i);
                switch(item_size) {
                case 2:
                    *(uint16_t*)(scratch_buf + i*2) = v;
                    break;
                case 4:
                    *(uint32_t*)(scratch_buf + i*4) = v;
                    break;
                default:
                    assert(false);
                    break;
                }
            }

            // write to scratch memory
            s->scratch_rw(s->rw_opaque,
                scratch_buf, scratch_addr, count*item_size, 1);
        } else {
            // read from scratch memory
            s->scratch_rw(s->rw_opaque,
                scratch_buf, scratch_addr, count*item_size, 0);

            int i;
            for (i=0; i<count; i++) {
                uint32_t v;
                switch(item_size) {
                case 2:
                    v = *(uint16_t*)(scratch_buf + i*2);
                    break;
                case 4:
                    v = (*(uint32_t*)(scratch_buf + i*4)) & item_mask;
                    break;
                default:
                    assert(false);
                    break;
                }
                // DPRINTF("... %06x\n", v);
                dsp_write_mapped_memory(s, dsp_offset+i, v);
            }
        }

        free(scratch_buf);

    }
}

uint32_t dsp_dma_read(DSPDMAState *s, DSPDMARegister reg)
{
    switch (reg) {
    case DMA_CONFIGURATION:
        return s->configuration;
    case DMA_CONTROL:
        return s->control;
    case DMA_START_BLOCK:
        return s->start_block;
    case DMA_NEXT_BLOCK:
        return s->next_block;
    default:
        assert(false);
    }
    return 0;
}

void dsp_dma_write(DSPDMAState *s, DSPDMARegister reg, uint32_t v)
{
    switch (reg) {
    case DMA_CONFIGURATION:
        s->configuration = v;
        break;
    case DMA_CONTROL:
        switch(v & DMA_CONTROL_ACTION) {
        case DMA_CONTROL_ACTION_START:
            s->control |= DMA_CONTROL_RUNNING;
            s->control &= ~DMA_CONTROL_STOPPED;
            break;
        case DMA_CONTROL_ACTION_STOP:
            s->control |= DMA_CONTROL_STOPPED;
            s->control &= ~DMA_CONTROL_RUNNING;
            break;
        case DMA_CONTROL_ACTION_FREEZE:
            s->control |= DMA_CONTROL_FROZEN;
            break;
        case DMA_CONTROL_ACTION_UNFREEZE:
            s->control &= ~DMA_CONTROL_FROZEN;
            break;
        default:
            assert(false);
            break;
        }
        dsp_dma_run(s);
        break;
    case DMA_START_BLOCK:
        s->start_block = v;
        break;
    case DMA_NEXT_BLOCK:
        s->next_block = v;
        break;
    default:
        assert(false);
    }
}

