/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
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
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"

#include "hw/xbox/dsp/dsp.h"

#include <math.h>

#define NV_PAPU_ISTS                                     0x00001000
#   define NV_PAPU_ISTS_GINTSTS                               (1 << 0)
#   define NV_PAPU_ISTS_FETINTSTS                             (1 << 4)
#define NV_PAPU_IEN                                      0x00001004
#define NV_PAPU_FECTL                                    0x00001100
#   define NV_PAPU_FECTL_FEMETHMODE                         0x000000E0
#       define NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING            0x00000000
#       define NV_PAPU_FECTL_FEMETHMODE_HALTED                  0x00000080
#       define NV_PAPU_FECTL_FEMETHMODE_TRAPPED                 0x000000E0
#   define NV_PAPU_FECTL_FETRAPREASON                       0x00000F00
#       define NV_PAPU_FECTL_FETRAPREASON_REQUESTED             0x00000F00
#define NV_PAPU_FECV                                     0x00001110
#define NV_PAPU_FEAV                                     0x00001118
#   define NV_PAPU_FEAV_VALUE                               0x0000FFFF
#   define NV_PAPU_FEAV_LST                                 0x00030000
#define NV_PAPU_FEDECMETH                                0x00001300
#define NV_PAPU_FEDECPARAM                               0x00001304
#define NV_PAPU_FEMEMADDR                                0x00001324
#define NV_PAPU_FEMEMDATA                                0x00001334
#define NV_PAPU_FETFORCE0                                0x00001500
#define NV_PAPU_FETFORCE1                                0x00001504
#   define NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE               (1 << 15)
#define NV_PAPU_SECTL                                    0x00002000
#   define NV_PAPU_SECTL_XCNTMODE                           0x00000018
#       define NV_PAPU_SECTL_XCNTMODE_OFF                       0
#define NV_PAPU_XGSCNT                                   0x0000200C
#define NV_PAPU_VPVADDR                                  0x0000202C
#define NV_PAPU_VPSGEADDR                                0x00002030
#define NV_PAPU_GPSADDR                                  0x00002040
#define NV_PAPU_GPFADDR                                  0x00002044
#define NV_PAPU_EPSADDR                                  0x00002048
#define NV_PAPU_EPFADDR                                  0x0000204C
#define NV_PAPU_TVL2D                                    0x00002054
#define NV_PAPU_CVL2D                                    0x00002058
#define NV_PAPU_NVL2D                                    0x0000205C
#define NV_PAPU_TVL3D                                    0x00002060
#define NV_PAPU_CVL3D                                    0x00002064
#define NV_PAPU_NVL3D                                    0x00002068
#define NV_PAPU_TVLMP                                    0x0000206C
#define NV_PAPU_CVLMP                                    0x00002070
#define NV_PAPU_NVLMP                                    0x00002074
#define NV_PAPU_GPSMAXSGE                                0x000020D4
#define NV_PAPU_GPFMAXSGE                                0x000020D8
#define NV_PAPU_EPSMAXSGE                                0x000020DC
#define NV_PAPU_EPFMAXSGE                                0x000020E0

#define NV_PAPU_GPXMEM                                   0x00000000
#define NV_PAPU_GPMIXBUF                                 0x00005000
#define NV_PAPU_GPYMEM                                   0x00006000
#define NV_PAPU_GPPMEM                                   0x0000A000
#define NV_PAPU_GPRST                                    0x0000FFFC
#define NV_PAPU_GPRST_GPRST                                 (1 << 0)
#define NV_PAPU_GPRST_GPDSPRST                              (1 << 1)
#define NV_PAPU_GPRST_GPNMI                                 (1 << 2)
#define NV_PAPU_GPRST_GPABORT                               (1 << 3)

#define NV_PAPU_EPXMEM                                   0x00000000
#define NV_PAPU_EPYMEM                                   0x00006000
#define NV_PAPU_EPPMEM                                   0x0000A000
#define NV_PAPU_EPRST                                    0x0000FFFC

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    {NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D}, //2D
    {NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D}, //3D
    {NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP}, //MP
};


/* audio processor object / front-end messages */
#define NV1BA0_PIO_FREE                                  0x00000010
#define NV1BA0_PIO_SET_ANTECEDENT_VOICE                  0x00000120
#   define NV1BA0_PIO_SET_ANTECEDENT_VOICE_HANDLE           0x0000FFFF
#   define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST             0x00030000
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT     0
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP      1
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP      2
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP      3
#define NV1BA0_PIO_VOICE_ON                              0x00000124
#   define NV1BA0_PIO_VOICE_ON_HANDLE                       0x0000FFFF
#define NV1BA0_PIO_VOICE_OFF                             0x00000128
#define NV1BA0_PIO_VOICE_PAUSE                           0x00000140
#   define NV1BA0_PIO_VOICE_PAUSE_HANDLE                    0x0000FFFF
#   define NV1BA0_PIO_VOICE_PAUSE_ACTION                    (1 << 18)
#define NV1BA0_PIO_SET_CURRENT_VOICE                     0x000002F8
#define NV1BA0_PIO_SET_VOICE_CFG_VBIN                    0x00000300
#define NV1BA0_PIO_SET_VOICE_CFG_FMT                     0x00000304
#define NV1BA0_PIO_SET_VOICE_TAR_VOLA                    0x00000360
#define NV1BA0_PIO_SET_VOICE_TAR_VOLB                    0x00000364
#define NV1BA0_PIO_SET_VOICE_TAR_VOLC                    0x00000368
#define NV1BA0_PIO_SET_VOICE_TAR_PITCH                   0x0000037C
#   define NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP              0xFFFF0000
#define NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE                0x000003A0
#   define NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE_OFFSET         0x00FFFFFF
#define NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO                 0x000003A4
#   define NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO_OFFSET          0x00FFFFFF
#define NV1BA0_PIO_SET_VOICE_BUF_CBO                     0x000003D8
#   define NV1BA0_PIO_SET_VOICE_BUF_CBO_OFFSET              0x00FFFFFF
#define NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO                 0x000003DC
#   define NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO_OFFSET          0x00FFFFFF
#define NV1BA0_PIO_SET_CURRENT_INBUF_SGE                 0x00000804
#   define NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE          0xFFFFFFFF
#define NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET          0x00000808
#   define NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER 0xFFFFF000
#define NV1BA0_PIO_SET_OUTBUF_BA                         0x00001000 // 8 byte pitch, 4 entries
#   define NV1BA0_PIO_SET_OUTBUF_BA_ADDRESS                  0x007FFF00
#define NV1BA0_PIO_SET_OUTBUF_LEN                        0x00001004 // 8 byte pitch, 4 entries
#   define NV1BA0_PIO_SET_OUTBUF_LEN_VALUE                   0x007FFF00
#define NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE                0x00001800
#   define NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE          0xFFFFFFFF
#define NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET         0x00001808
#   define NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER 0xFFFFF000

#define SE2FE_IDLE_VOICE                                 0x00008000


/* voice structure */
#define NV_PAVS_SIZE                                     0x00000080
#define NV_PAVS_VOICE_CFG_VBIN                           0x00000000
#   define NV_PAVS_VOICE_CFG_VBIN_V0BIN                     (0x1F << 0)
#   define NV_PAVS_VOICE_CFG_VBIN_V1BIN                     (0x1F << 5)
#   define NV_PAVS_VOICE_CFG_VBIN_V2BIN                     (0x1F << 10)
#   define NV_PAVS_VOICE_CFG_VBIN_V3BIN                     (0x1F << 16)
#   define NV_PAVS_VOICE_CFG_VBIN_V4BIN                     (0x1F << 21)
#   define NV_PAVS_VOICE_CFG_VBIN_V5BIN                     (0x1F << 26)
#define NV_PAVS_VOICE_CFG_FMT                            0x00000004
#   define NV_PAVS_VOICE_CFG_FMT_V6BIN                      (0x1F << 0)
#   define NV_PAVS_VOICE_CFG_FMT_V7BIN                      (0x1F << 5)
#   define NV_PAVS_VOICE_CFG_FMT_DATA_TYPE                  (1 << 24)
#   define NV_PAVS_VOICE_CFG_FMT_LOOP                       (1 << 25)
#   define NV_PAVS_VOICE_CFG_FMT_STEREO                     (1 << 27)
#   define NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE                (0x3 << 28)
#       define NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8             0
#       define NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16            1
#       define NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24            2
#       define NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32            3
#   define NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE             (0x3 << 30)
#       define NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B8          0
#       define NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16         1
#       define NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM       2
#       define NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B32         3

#define NV_PAVS_VOICE_CUR_PSL_START                      0x00000020
#   define NV_PAVS_VOICE_CUR_PSL_START_BA                   0x00FFFFFF
#define NV_PAVS_VOICE_CUR_PSH_SAMPLE                     0x00000024
#   define NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO                 0x00FFFFFF
#define NV_PAVS_VOICE_PAR_STATE                          0x00000054
#   define NV_PAVS_VOICE_PAR_STATE_PAUSED                   (1 << 18)
#   define NV_PAVS_VOICE_PAR_STATE_NEW_VOICE                (1 << 20)
#   define NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE             (1 << 21)
#define NV_PAVS_VOICE_PAR_OFFSET                         0x00000058
#   define NV_PAVS_VOICE_PAR_OFFSET_CBO                     0x00FFFFFF
#define NV_PAVS_VOICE_PAR_NEXT                           0x0000005C
#   define NV_PAVS_VOICE_PAR_NEXT_EBO                       0x00FFFFFF
#define NV_PAVS_VOICE_TAR_VOLA                           0x00000060
#   define NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0              0x0000000F
#   define NV_PAVS_VOICE_TAR_VOLA_VOLUME0                   0x0000FFF0
#   define NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0              0x000F0000
#   define NV_PAVS_VOICE_TAR_VOLA_VOLUME1                   0xFFF00000
#define NV_PAVS_VOICE_TAR_VOLB                           0x00000064
#   define NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4              0x0000000F
#   define NV_PAVS_VOICE_TAR_VOLB_VOLUME2                   0x0000FFF0
#   define NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4              0x000F0000
#   define NV_PAVS_VOICE_TAR_VOLB_VOLUME3                   0xFFF00000
#define NV_PAVS_VOICE_TAR_VOLC                           0x00000068
#   define NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8             0x0000000F
#   define NV_PAVS_VOICE_TAR_VOLC_VOLUME4                   0x0000FFF0
#   define NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8             0x000F0000
#   define NV_PAVS_VOICE_TAR_VOLC_VOLUME5                   0xFFF00000

#define NV_PAVS_VOICE_TAR_PITCH_LINK                     0x0000007C
#   define NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE   0x0000FFFF
#   define NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH               0xFFFF0000



#define MCPX_HW_MAX_VOICES 256


#define GET_MASK(v, mask) (((v) & (mask)) >> (ffs(mask)-1))

#define SET_MASK(v, mask, val)                                       \
    do {                                                             \
        (v) &= ~(mask);                                              \
        (v) |= ((val) << (ffs(mask)-1)) & (mask);                    \
    } while (0)

#define CASE_4(v, step)                                              \
    case (v):                                                        \
    case (v)+(step):                                                 \
    case (v)+(step)*2:                                               \
    case (v)+(step)*3


// #define MCPX_DEBUG
#ifdef MCPX_DEBUG
# define MCPX_DPRINTF(format, ...)       printf(format, ## __VA_ARGS__)
#else
# define MCPX_DPRINTF(format, ...)       do { } while (0)
#endif

#define APU_DEBUG
#ifdef APU_DEBUG
#include "wav_out.h"
#endif

typedef struct MCPXAPUState {
    PCIDevice dev;

    MemoryRegion mmio;

    /* Setup Engine */
    struct {
        QEMUTimer *frame_timer;
    } se;

    /* Voice Processor */
    struct {
        MemoryRegion mmio;
    } vp;

    /* Global Processor */
    struct {
        MemoryRegion mmio;
        DSPState *dsp;
        uint32_t regs[0x10000];
    } gp;

    /* Encode Processor */
    struct {
        MemoryRegion mmio;
        DSPState *dsp;
        uint32_t regs[0x10000];
    } ep;

    uint32_t inbuf_sge_handle; //FIXME: Where is this stored?
    uint32_t outbuf_sge_handle; //FIXME: Where is this stored?
    uint32_t regs[0x20000];

} MCPXAPUState;


#define MCPX_APU_DEVICE(obj) \
    OBJECT_CHECK(MCPXAPUState, (obj), "mcpx-apu")

static uint32_t voice_get_mask(MCPXAPUState *d,
                               unsigned int voice_handle,
                               hwaddr offset,
                               uint32_t mask)
{
    assert(voice_handle < 0xFFFF);
    hwaddr voice = d->regs[NV_PAPU_VPVADDR]
                    + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(voice + offset) & mask) >> (ffs(mask)-1);
}
static void voice_set_mask(MCPXAPUState *d,
                           unsigned int voice_handle,
                           hwaddr offset,
                           uint32_t mask,
                           uint32_t val)
{
    assert(voice_handle < 0xFFFF);
    hwaddr voice = d->regs[NV_PAPU_VPVADDR]
                    + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(voice + offset) & ~mask;
    stl_le_phys(voice + offset,
                v | ((val << (ffs(mask)-1)) & mask));
}



static void update_irq(MCPXAPUState *d)
{
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS)
        && ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS)
              & d->regs[NV_PAPU_IEN])) {

        d->regs[NV_PAPU_ISTS] |= NV_PAPU_ISTS_GINTSTS;
        MCPX_DPRINTF("mcpx irq raise\n");
        pci_irq_assert(&d->dev);
    } else {
        d->regs[NV_PAPU_ISTS] &= ~NV_PAPU_ISTS_GINTSTS;
        MCPX_DPRINTF("mcpx irq lower\n");
        pci_irq_deassert(&d->dev);
    }
}

static uint64_t mcpx_apu_read(void *opaque,
                              hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100; //???
        break;
    default:
        if (addr < 0x20000) {
            r = d->regs[addr];
        }
        break;
    }

    MCPX_DPRINTF("mcpx apu: read [0x%llx] -> 0x%llx\n", addr, r);
    return r;
}
static void mcpx_apu_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    MCPX_DPRINTF("mcpx apu: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV_PAPU_ISTS:
        /* the bits of the interrupts to clear are wrtten */
        d->regs[NV_PAPU_ISTS] &= ~val;
        update_irq(d);
        break;
    case NV_PAPU_SECTL:
        if ( ((val & NV_PAPU_SECTL_XCNTMODE) >> 3)
                == NV_PAPU_SECTL_XCNTMODE_OFF) {
            timer_del(d->se.frame_timer);
        } else {
            timer_mod(d->se.frame_timer,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
        }
        d->regs[addr] = val;
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write'
         * This value is expected to be written to FEMEMADDR on completion of
         * something to do with notifies. Just do it now :/ */
        stl_le_phys(d->regs[NV_PAPU_FEMEMADDR], val);
        d->regs[addr] = val;
        break;
    default:
        if (addr < 0x20000) {
            d->regs[addr] = val;
        }
        break;
    }
}
static const MemoryRegionOps mcpx_apu_mmio_ops = {
    .read = mcpx_apu_read,
    .write = mcpx_apu_write,
};


static void fe_method(MCPXAPUState *d,
                      uint32_t method, uint32_t argument)
{

    unsigned int slot;

    MCPX_DPRINTF("mcpx fe_method 0x%x 0x%x\n", method, argument);

    //assert((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == 0);

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;
    switch (method) {
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;
    case NV1BA0_PIO_VOICE_ON:
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            /* voice is added to the top of the selected list */
            unsigned int top_reg = voice_list_regs[list-1].top;
            voice_set_mask(d, selected_handle,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            /* voice is added after the antecedent voice */
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(d, antecedent_voice,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, selected_handle,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                next_handle);
            voice_set_mask(d, antecedent_voice,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                selected_handle);

        }
            voice_set_mask(d, selected_handle,
                    NV_PAVS_VOICE_PAR_STATE,
                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE,
                    1);
        //}
        break;
    case NV1BA0_PIO_VOICE_OFF:
        voice_set_mask(d, argument,
                NV_PAVS_VOICE_PAR_STATE,
                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE,
                0);
        break;
    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE,
                NV_PAVS_VOICE_PAR_STATE,
                NV_PAVS_VOICE_PAR_STATE_PAUSED,
                (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;
    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_CFG_VBIN,
                0xFFFFFFFF,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_CFG_FMT,
                0xFFFFFFFF,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_TAR_VOLA,
                0xFFFFFFFF,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_TAR_VOLB,
                0xFFFFFFFF,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_TAR_VOLC,
                0xFFFFFFFF,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_CUR_PSL_START,
                NV_PAVS_VOICE_CUR_PSL_START_BA,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_PAR_OFFSET,
                NV_PAVS_VOICE_PAR_OFFSET_CBO,
                argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV],
                NV_PAVS_VOICE_PAR_NEXT,
                NV_PAVS_VOICE_PAR_NEXT_EBO,
                argument);
        break;
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        //FIXME: Is there an upper limit for the SGE table size?
        //FIXME: NV_PAPU_VPSGEADDR is probably bad, as outbuf SGE use the same handle range (or that is also wrong)
        hwaddr sge_address = d->regs[NV_PAPU_VPSGEADDR] + d->inbuf_sge_handle * 8;
        stl_le_phys(sge_address, argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        printf("Wrote inbuf SGE[0x%X] = 0x%08X\n", d->inbuf_sge_handle, argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }
    CASE_4(NV1BA0_PIO_SET_OUTBUF_BA, 8): // 8 byte pitch, 4 entries
        slot = (method - NV1BA0_PIO_SET_OUTBUF_BA) / 8;
        //FIXME: Use NV1BA0_PIO_SET_OUTBUF_BA_ADDRESS = 0x007FFF00 ?
        printf("outbuf_ba[%d]: 0x%08X\n", slot, argument);
        //assert(false); //FIXME: Enable assert! no idea what this reg does
        break;
    CASE_4(NV1BA0_PIO_SET_OUTBUF_LEN, 8): // 8 byte pitch, 4 entries
        slot = (method - NV1BA0_PIO_SET_OUTBUF_LEN) / 8;
        //FIXME: Use NV1BA0_PIO_SET_OUTBUF_LEN_VALUE = 0x007FFF00 ?
        printf("outbuf_len[%d]: 0x%08X\n", slot, argument);
        //assert(false); //FIXME: Enable assert! no idea what this reg does
        break;
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->outbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        //FIXME: Is there an upper limit for the SGE table size?
        //FIXME: NV_PAPU_VPSGEADDR is probably bad, as inbuf SGE use the same handle range (or that is also wrong)
        // NV_PAPU_EPFADDR   EP outbufs
        // NV_PAPU_GPFADDR   GP outbufs
        // But how does it know which outbuf is being written?!
        hwaddr sge_address = d->regs[NV_PAPU_VPSGEADDR] + d->outbuf_sge_handle * 8;
        stl_le_phys(sge_address, argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        printf("Wrote outbuf SGE[0x%X] = 0x%08X\n", d->outbuf_sge_handle, argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }
    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;

            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;

            d->regs[NV_PAPU_ISTS] |= NV_PAPU_ISTS_FETINTSTS;
            update_irq(d);
        } else {
            assert(false);
        }
        break;
    default:
        assert(false);
        break;
    }
}


static uint64_t vp_read(void *opaque,
                        hwaddr addr, unsigned int size)
{
    MCPX_DPRINTF("mcpx apu VP: read [0x%llx]\n", addr);
    switch (addr) {
    case NV1BA0_PIO_FREE:
        /* we don't simulate the queue for now,
         * pretend to always be empty */
        return 0x80;
    default:
        break;
    }
    return 0;
}
static void vp_write(void *opaque, hwaddr addr,
                     uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    MCPX_DPRINTF("mcpx apu VP: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
    case NV1BA0_PIO_VOICE_ON:
    case NV1BA0_PIO_VOICE_OFF:
    case NV1BA0_PIO_VOICE_PAUSE:
    case NV1BA0_PIO_SET_CURRENT_VOICE:
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET:
    CASE_4(NV1BA0_PIO_SET_OUTBUF_BA, 8): // 8 byte pitch, 4 entries
    CASE_4(NV1BA0_PIO_SET_OUTBUF_LEN, 8): // 8 byte pitch, 4 entries
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET:
        /* TODO: these should instead be queueing up fe commands */
        fe_method(d, addr, val);
        break;
    default:
        break;
    }
}
static const MemoryRegionOps vp_ops = {
    .read = vp_read,
    .write = vp_write,
};

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr) {
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address = ldl_le_phys(sge_base + entry*4*2);
    uint32_t prd_control = ldl_le_phys(sge_base + entry*4*2 + 4);
    //printf("Addr: 0x%08X, control: 0x%08X\n", prd_address, prd_control);

    return prd_address + addr % TARGET_PAGE_SIZE;
}

static void scratch_rw(hwaddr sge_base, unsigned int max_sge,
                       uint8_t* ptr, uint32_t addr, size_t len, bool dir)
{
    int i;
    for (i=0; i<len; i++) {
        hwaddr paddr = get_data_ptr(sge_base, max_sge, addr + i);

        if (dir) {
            stb_phys(paddr, ptr[i]);
        } else {
            ptr[i] = ldub_phys(paddr);
        }
    }
}

static void fifo_rw(hwaddr sge_base, unsigned int max_sge, unsigned int idx,
                    uint8_t* ptr, uint32_t addr, size_t len, bool dir)
{
    //FIXME: Get FIFO BASE, get FIFO CUR, get FIFO END
static int offset = 0;
offset %= 0x2000;
addr += offset;
    int i;
    for (i=0; i<len; i++) {
        hwaddr paddr = get_data_ptr(sge_base, max_sge, addr + i);

        if (dir) {
            stb_phys(paddr, ptr[i]);
        } else {
            ptr[i] = ldub_phys(paddr);
        }
    }
offset += len;
    // FIXME: Fixup FIFO CUR

}

static void gp_scratch_rw(void *opaque, uint8_t* ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    scratch_rw(d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
               ptr, addr, len, dir);
}

static void gp_fifo_rw(void *opaque, unsigned int idx, uint8_t* ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    fifo_rw(d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE], idx,
            ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t* ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    scratch_rw(d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
               ptr, addr, len, dir);
}

static void ep_fifo_rw(void *opaque, unsigned int idx, uint8_t* ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    fifo_rw(d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE], idx,
            ptr, addr, len, dir);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if ((!(oldval & NV_PAPU_GPRST_GPRST)
                || !(oldval & NV_PAPU_GPRST_GPDSPRST))
            && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST)) ) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP */
static uint64_t gp_read(void *opaque,
                        hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0xFFF*4: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', xaddr);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x3FF*4: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', 3072 + xaddr);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x7FF*4: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'Y', yaddr);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0xFFF*4: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'P', paddr);
        break;
    }
    default:
        r = d->gp.regs[addr];
        break;
    }
    MCPX_DPRINTF("mcpx apu GP: read [0x%llx] -> 0x%llx\n", addr, r);
    return r;
}
static void gp_write(void *opaque, hwaddr addr,
                     uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    MCPX_DPRINTF("mcpx apu GP: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0xFFF*4: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x3FF*4: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        dsp_write_memory(d->gp.dsp, 'X', 3072 + xaddr, val);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x7FF*4: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0xFFF*4: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_GPRST:
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], val);
        d->gp.regs[NV_PAPU_GPRST] = val;
        break;
    default:
        d->gp.regs[addr] = val;
        break;
    }
}
static const MemoryRegionOps gp_ops = {
    .read = gp_read,
    .write = gp_write,
};


/* Encode Processor - encoding DSP */
static uint64_t ep_read(void *opaque,
                        hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xBFF*4: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'X', xaddr);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0xFF*4: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'Y', yaddr);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0xFFF*4: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'P', paddr);
        break;
    }
    default:
        r = d->ep.regs[addr];
        break;
    }
    MCPX_DPRINTF("mcpx apu EP: read [0x%llx] -> 0x%llx\n", addr, r);
    return r;
}
static void ep_write(void *opaque, hwaddr addr,
                     uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    MCPX_DPRINTF("mcpx apu EP: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xBFF*4: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0xFF*4: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0xFFF*4: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_EPRST:
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], val);
        d->ep.regs[NV_PAPU_EPRST] = val;
        break;
    default:
        d->ep.regs[addr] = val;
        break;
    }
}
static const MemoryRegionOps ep_ops = {
    .read = ep_read,
    .write = ep_write,
};


/* TODO: this should be on a thread so it waits on the voice lock */
static void se_frame(void *opaque)
{
    MCPXAPUState *d = opaque;
    timer_mod(d->se.frame_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    MCPX_DPRINTF("mcpx frame ping\n");
    int list;

    uint64_t start = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    int32_t mixbuf[32][0x20] = {0};

    static WAVOutState* buf_wav_out;
    if (buf_wav_out == NULL) {
        buf_wav_out = g_malloc0(sizeof(WAVOutState) * 2);
        wav_out_init(&buf_wav_out[0], "vp-out-buf0.wav", 44100, 16, 1);
        wav_out_init(&buf_wav_out[1], "vp-out-buf1.wav", 48000, 24, 1);
    }

    for (list=0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];
        MCPX_DPRINTF("list %d current voice %d\n", list, d->regs[current]);
        while (d->regs[current] != 0xFFFF) {
            d->regs[next] = voice_get_mask(d, d->regs[current],
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            if (!voice_get_mask(d, d->regs[current],
                    NV_PAVS_VOICE_PAR_STATE,
                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                MCPX_DPRINTF("voice %d not active...!\n", d->regs[current]);
                fe_method(d, SE2FE_IDLE_VOICE, d->regs[current]);
            } else {
                uint32_t v = d->regs[current];
                int32_t samples[2][0x20] = {0};

                int16_t p = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK, NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
                float rate = powf(2.0f, p / 4096.0f);
                //printf("Got %f\n", rate * 48000.0f);

                float overdrive = 1.0f; //FIXME: This is just a hack because our APU runs too rarely

                //NV_PAVS_VOICE_PAR_OFFSET_CBO
                bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_STEREO);
                unsigned int sample_size = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);

                // B8, B16, ADPCM, B32
                unsigned int container_sizes[4] = { 1, 2, 0, 4 };
                unsigned int container_size = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);

                bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
                assert(!stream);
                bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED);
                bool loop = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_LOOP);
                uint32_t ebo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO);
                uint32_t cbo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO);
                uint32_t lbo = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE, NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
                uint32_t ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);

                // This is probably cleared when the first sample is played
                //FIXME: How will this behave if CBO > EBO on first play?
                //FIXME: How will this behave if paused?
                voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

                for(unsigned int i = 0; i < 0x20; i++) {

                    uint32_t sample_pos = (uint32_t)(i * rate * overdrive);

                    if ((cbo + sample_pos) > ebo) {
                      if (!loop) {
                        // Set to safe state
                        cbo = ebo; //FIXME: Will the hw do this?
                        //FIXME: Not sure if this happens.. needs a hwtest.
                        // Some RE also suggests that the voices will automaticly be removed from the list (!!!)
                        voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);
                        break;
                      }
                      // Now go to loop start
                      //FIXME: Make sure this logic still works for very high sample_pos greater than ebo
                      cbo += sample_pos;
                      cbo %= ebo + 1;
                      cbo += lbo;
                      cbo -= sample_pos;
                    }

                    sample_pos += cbo;
                    assert(sample_pos <= ebo);

                    //FIXME: The mod ebo thing is a hack!
                    assert(container_size != NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);
                    uint32_t linear_addr = ba + (sample_pos % (ebo + 1)) * container_sizes[container_size];
                    hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, linear_addr);
                    //FIXME: Handle reading accross pages?!

                    //printf("Sampling from 0x%08X\n", addr);

                    // Get samples for this voice
                    for(unsigned int channel = 0; channel < 2; channel++) {
                        switch(sample_size) {
                        case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                            samples[channel][i] = ldub_phys(addr);
                            break;
                        case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                            samples[channel][i] = (int16_t)lduw_le_phys(addr);
                            break;
                        case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                            samples[channel][i] = (int32_t)(ldl_le_phys(addr) << 8) >> 8;
                            break;
                        case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                            samples[channel][i] = (int32_t)ldl_le_phys(addr);
                            break;
                        }
                        if (stereo) {
                            // Advance cursor to second channel
                            //FIXME!!!
                        } else {
                            assert(sizeof(samples[0]) == 0x20 * 4);
                            memcpy(samples[1], samples[0], sizeof(samples[0]));
                            break;
                        }
                    }
                }

                //FIXME: Decode voice volume and bins
                int bin[8] = {
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN),
                  voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN)
                };
                uint16_t vol[8] = {
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0),
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1),
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2),
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3),
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4),
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5),
                  (voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8) |
                  (voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4) |
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0),
                  (voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8) |
                  (voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4) |
                  voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0),
                };

                if (v == 0x0044) {
                  static unsigned int x = 0;
                  
                  for(unsigned int i = 0; i < 0x20; i++) {
                    uint32_t sample_pos = cbo + (uint32_t)(i * rate * overdrive);
                    //uint32_t sample_pos = cbo + (uint32_t)(i * rate * overdrive);
                    //FIXME: The mod ebo thing is a hack!
                    uint32_t linear_addr = ba + (sample_pos % (ebo + 1)) * container_sizes[container_size];
                    hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, linear_addr);
                    //printf("Sampling from 0x%08X\n", addr);
                    int16_t sample = (int16_t)lduw_le_phys(addr);
                    wav_out_write(&buf_wav_out[0], &sample, 2); // voice input
                    x++;
                  }
                  wav_out_update(&buf_wav_out[0]);

                  for(unsigned int i = 0; i < 0x20; i++) {
                    wav_out_write(&buf_wav_out[1], &samples[0][i], 3); // mixbuf for voice
                    wav_out_write(&buf_wav_out[1], &samples[1][i], 3); // mixbuf for voice
                  }
                  wav_out_update(&buf_wav_out[1]);
                }

                if (paused) {
                  assert(sizeof(samples) == 0x20 * 4 * 2);
                  memset(samples, 0x00, sizeof(samples));
                } else {
                  cbo += 0x20 * rate * overdrive;
                  voice_set_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
                }

                const char* name = "unk";
#if 0
                // Whitelisted: !
                if (ba == 0x1F08) {
                  name = "Thunder";
                } else if (ba == 0x141E8) { // White noise (in bad range?! 16U?!)
                  name = "White Noise";
                } else if (ba == 0xD1F0) { // Beep sound
                  name = "Beep";
                } else if (ba == 0x102E8) { // Flubber / Bubble sound
                  name = "Flubber";

                // Blacklisted: !
                } else if (ba == 0xF998) { // garbage? some sawtooth I guess
                  name = "Sawtooth";
                  goto skipvoice;
                } else if (ba == 0x978) { // silence
                  name = "Silence";
                  goto skipvoice;


                ///// Wavebank XDK sample
                } else if (ba == 0x246C50) { // garbage? some sawtooth I guess
                  name = "Sound 9"; // "The call is tails"
                } else if (ba == 0x3A6694) { // garbage? some sawtooth I guess
                  name = "Sound 13"; // Music


                ///// Catchall
                } else {
                  printf("Skipping ", ba);
                  goto skipvoice;
                }
                

#endif

                //FIXME: If phase negations means to flip the signal upside down
                //       we should modify volume of bin6 and bin7 here.

                // Mix samples into voice bins
                for(unsigned int j = 0; j < 8; j++) {
                    //printf("Adding voice 0x%04X to bin %d [Rate %.2f, Volume 0x%03X] sample %d at %d [%.2fs]\n", v, bin[j], rate, vol[j], samples[0], cbo, cbo / (rate * 48000.0f));
                    for(unsigned int i = 0; i < 0x20; i++) {
                        //FIXME: how is the volume added?
                        //FIXME: What happens to the other channel?
                        mixbuf[bin[j]][i] += (vol[j] * samples[0][i]) / 0xFFF;
                    }
                }
skipvoice:; // FIXME: Remove.. hack!
                printf("Voice 0x%04X: '%s' (0x%X)\n", v, name, ba);
            }
            MCPX_DPRINTF("next voice %d\n", d->regs[next]);
            d->regs[current] = d->regs[next];
        }
    }

    // Write VP result to mixbuf
    for(unsigned int j = 0; j < 32; j++) {
        for(unsigned int i = 0; i < 0x20; i++) {
            dsp_write_memory(d->gp.dsp, 'X', 3072 + j * 0x20 + i, mixbuf[j][i] & 0xFFFFFF);
        }
    }

#if 1
    // We can not guarantee sync of AC97 and the APU if the emu is not
    // running in realtime. So for development we should have a clean way
    // of looking at the data. This is said method for the VP.
    static void* vp_wav_out = NULL;
    if (vp_wav_out == NULL) {
        vp_wav_out = g_malloc0(sizeof(WAVOutState));
        wav_out_init(vp_wav_out, "vp-out.wav", 48000, 24, 2);
    }
    for(unsigned int i = 0; i < 0x20; i++) {
        // Map channels from DirectSound -> WAV
        uint32_t sample_0 = mixbuf[0][i] & 0xFFFFFF;
        sample_0 *= 0x80;
        uint32_t sample_2 = mixbuf[2][i] & 0xFFFFFF;
        sample_2 *= 0x80;
        //printf("Sample: %d %d\n", sample_0, sample_2);
        wav_out_write(vp_wav_out, &sample_0, 3); // Front left
        wav_out_write(vp_wav_out, &sample_2, 3); // Front right
/*
        wav_writer(mixbuf[1][i] & 0xFFFFFF); // Center
        wav_writer(mixbuf[5][i] & 0xFFFFFF); // LFE
        wav_writer(mixbuf[3][i] & 0xFFFFFF); // Rear left
        wav_writer(mixbuf[4][i] & 0xFFFFFF); // Rear right
*/
    }
    wav_out_update(vp_wav_out);
#endif

    uint64_t vp_done = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    if ((d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST)
        && (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->gp.dsp);

        // hax
        dsp_run(d->gp.dsp, 10000);
    }

#if 0
    // Dump the GP output of DirectSound.
    
    uint32_t offset = 0x8000;
    // NV_PAPU_GPSADDR
    wav_init("gp-out.wav");

    for(unsigned int i = 0; i < 0x20; i++) {
        // Map channels from DirectSound -> WAV
        wav_writer(gps[0][i] & 0xFFFFFF); // Front left
    }
#endif

    uint64_t gp_done = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    if ((d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST)
        && (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->ep.dsp);

        // hax
        //printf("Running EP code\n");
        dsp_run(d->ep.dsp, 1000);
    }

#if 0
    // Dump the EP output
    //FIXME: Use the actual FIFO channel addresses and output both, PCM and SPDIF

    wav_init("ep-out-pcm.wav");
    //FIXME: Support SPDIF
    //wav_init("ep-out-spdif.wav");

    //NV_PAPU_EPFADDR
    
#endif

    uint64_t ep_done = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    printf("\n");
    printf("VP took %.1f us\n", (vp_done - start) / 1000.0f);
    printf("GP took %.1f us\n", (gp_done - vp_done) / 1000.0f);
    printf("EP took %.1f us\n", (ep_done - gp_done) / 1000.0f);
    printf("Total: %.1f us\n", (ep_done - start) / 1000.0f);
    printf("Limit: %.1f us\n", (32.0 / 48000.0) * 1000000.0f);
}


static int mcpx_apu_initfn(PCIDevice *dev)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(dev);

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init_io(&d->mmio, OBJECT(dev), &mcpx_apu_mmio_ops, d,
                          "mcpx-apu-mmio", 0x80000);

    memory_region_init_io(&d->vp.mmio, OBJECT(dev), &vp_ops, d,
                          "mcpx-apu-vp", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x20000, &d->vp.mmio);

    memory_region_init_io(&d->gp.mmio, OBJECT(dev), &gp_ops, d,
                          "mcpx-apu-gp", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x30000, &d->gp.mmio);

    memory_region_init_io(&d->ep.mmio, OBJECT(dev), &ep_ops, d,
                          "mcpx-apu-ep", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x50000, &d->ep.mmio);

    pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);


    d->se.frame_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, se_frame, d);

    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw);
    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw);

    return 0;
}

static void mcpx_apu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = PCI_DEVICE_ID_NVIDIA_MCPX_APU;
    k->revision = 210;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    k->init = mcpx_apu_initfn;

    dc->desc = "MCPX Audio Processing Unit";
}

static const TypeInfo mcpx_apu_info = {
    .name          = "mcpx-apu",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MCPXAPUState),
    .class_init    = mcpx_apu_class_init,
};

static void mcpx_apu_register(void)
{
    type_register_static(&mcpx_apu_info);
}
type_init(mcpx_apu_register);
