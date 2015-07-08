/*
  This will submit commands to the NV2A code in qemu.
  This is useful to test GPU compatibilty changes and find regressions in xbox games.

  Part of the xqemu project.

  (c)2015 Jannik Vogel
*/

#include "qapi-visit.h"
#include "qapi/qmp-output-visitor.h"
#include "qapi/qmp/qjson.h"
#include "qemu-common.h"
#include "qemu/option.h"
#include "qemu/error-report.h"
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"
#include "sysemu/arch_init.h"
#include "block/block_int.h"
#include "block/qapi.h"

#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "ui/console.h"
#include "hw/pci/pci.h"
#include "ui/console.h"
#include "hw/display/vga.h"
#include "hw/display/vga_int.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "qapi/qmp/qstring.h"
#include "gl/gloffscreen.h"
#include "qemu/config-file.h"

#include "hw/xbox/swizzle.h"
#include "hw/xbox/u_format_r11g11b10f.h"
#include "hw/xbox/nv2a_vsh.h"
#include "hw/xbox/nv2a_psh.h"

#include "hw/xbox/nv2a.h"

#include "hw/hw.h"
#include "sysemu/arch_init.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bridge.h"
#include "hw/boards.h"
#include "hw/ide.h"
#include "hw/timer/mc146818rtc.h"
#include "hw/timer/i8254.h"
#include "hw/audio/pcspk.h"
#include "sysemu/sysemu.h"
#include "hw/cpu/icc_bus.h"
#include "hw/sysbus.h"
#include "hw/i2c/smbus.h"
#include "sysemu/blockdev.h"
#include "hw/loader.h"
#include "exec/address-spaces.h"

#include "hw/xbox/xbox_pci.h"
#include "hw/xbox/nv2a.h"

#include "hw/xbox/xbox.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#if 0
#define dprintf(fmt,...) printf(fmt,##__VA_ARGS__)
#else
#define dprintf(fmt,...)
#endif

static struct NV2AState *d;
static MemoryRegion* vram = NULL;
static MemoryRegion* ramin = NULL;

typedef struct {
  FILE* handle;
} File;

/* This must be kept in sync with nv2a.c! */
typedef struct NV2ABlockInfo {
    const char* name;
    hwaddr offset;
    uint64_t size;
    MemoryRegionOps ops;
} NV2ABlockInfo;
extern const struct NV2ABlockInfo* nv2a_blocktable;
extern const size_t nv2a_blocktable_size;
extern bool record_playback;

/* HELPERS */


static File* open_file(const char* path) {
  FILE* handle = fopen(path,"rb");
  if (handle == NULL) {
    return NULL;
  }
  File* f = malloc(sizeof(File));
  f->handle = handle;
  return f;
}

static uint32_t read_uint8(File* f) {
  uint8_t buffer;
  fread(&buffer,1,1,f->handle);
  return buffer;
}

static uint32_t read_uint32(File* f) {
  uint32_t buffer;
  fread(&buffer,4,1,f->handle);
  return buffer;
}

static uint64_t read_uint64(File* f) {
  uint64_t buffer;
  fread(&buffer,8,1,f->handle);
  return buffer;
}

static bool is_eof(File* f) {
  return feof(f->handle);
}

static void close_file(File* f) {
  fclose(f->handle);
  free(f);
  return;
}

static char* temporary_file(const char* name) {
  //FIXME: Actual temp path
  return strdup(name);
}

static const NV2ABlockInfo* find_block(hwaddr address) {
  unsigned int i;
  for(i = 0; i < nv2a_blocktable_size/sizeof(NV2ABlockInfo); i++) {
    const NV2ABlockInfo* b = &nv2a_blocktable[i];
    if ((address >= b->offset) && (address < (b->offset + b->size))) {
      return b;
    }
  }
  return NULL;
}

static QEMUMachine* machine = NULL;
int qemu_register_machine(QEMUMachine *m)
{
  dprintf("Attempting to register '%s'\n",m->name);
  if (!strcmp(m->name, "xbox")) {
    machine = m;
  }
  return 0;
}

static QemuOptsList qemu_machine_opts = {
    .name = "machine",
    .implied_opt_name = "type",
    .merge_lists = true,
    .head = QTAILQ_HEAD_INITIALIZER(qemu_machine_opts.head),
    .desc = {
        {
            .name = "type",
            .type = QEMU_OPT_STRING,
            .help = "emulated machine"
        }, {
            .name = "accel",
            .type = QEMU_OPT_STRING,
            .help = "accelerator list",
        }, {
            .name = "kernel_irqchip",
            .type = QEMU_OPT_BOOL,
            .help = "use KVM in-kernel irqchip",
        }, {
            .name = "kvm_shadow_mem",
            .type = QEMU_OPT_SIZE,
            .help = "KVM shadow MMU size",
        }, {
            .name = "kernel",
            .type = QEMU_OPT_STRING,
            .help = "Linux kernel image file",
        }, {
            .name = "initrd",
            .type = QEMU_OPT_STRING,
            .help = "Linux initial ramdisk file",
        }, {
            .name = "append",
            .type = QEMU_OPT_STRING,
            .help = "Linux kernel command line",
        }, {
            .name = "dtb",
            .type = QEMU_OPT_STRING,
            .help = "Linux kernel device tree file",
        }, {
            .name = "dumpdtb",
            .type = QEMU_OPT_STRING,
            .help = "Dump current dtb to a file and quit",
        }, {
            .name = "phandle_start",
            .type = QEMU_OPT_NUMBER,
            .help = "The first phandle ID we may generate dynamically",
        }, {
            .name = "dt_compatible",
            .type = QEMU_OPT_STRING,
            .help = "Overrides the \"compatible\" property of the dt root node",
        }, {
            .name = "dump-guest-core",
            .type = QEMU_OPT_BOOL,
            .help = "Include guest memory in  a core dump",
        }, {
            .name = "mem-merge",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable memory merge support",
        },{
            .name = "usb",
            .type = QEMU_OPT_BOOL,
            .help = "Set on/off to enable/disable usb",
        },{
            .name = "bootrom",
            .type = QEMU_OPT_STRING,
            .help = "Xbox bootrom file",
        },{
            .name = "mediaboard_rom",
            .type = QEMU_OPT_STRING,
            .help = "Chihiro mediaboard rom file",
        },{
            .name = "mediaboard_filesystem",
            .type = QEMU_OPT_STRING,
            .help = "Chihiro mediaboard filesystem file",
        },
        { /* End of list */ }
    },
};

/* Mostly stolen from vl.c, just the bare minimum to get QEMU working [very brokenly] */
static void init_qemu(ram_addr_t ram_size) {

  if (!g_thread_supported()) {
#if !GLIB_CHECK_VERSION(2, 31, 0)
    g_thread_init(NULL);
#else
    fprintf(stderr, "glib threading failed to initialize.\n");
    exit(1);
#endif
  }

  module_call_init(MODULE_INIT_QOM);

#if 0
  qemu_add_opts(&qemu_drive_opts);
  qemu_add_opts(&qemu_chardev_opts);
  qemu_add_opts(&qemu_device_opts);
  qemu_add_opts(&qemu_netdev_opts);
  qemu_add_opts(&qemu_net_opts);
  qemu_add_opts(&qemu_rtc_opts);
#endif
  qemu_add_opts(&qemu_global_opts);
#if 0
  qemu_add_opts(&qemu_mon_opts);
  qemu_add_opts(&qemu_trace_opts);
  qemu_add_opts(&qemu_option_rom_opts);
#endif
  qemu_add_opts(&qemu_machine_opts);
#if 0
  qemu_add_opts(&qemu_smp_opts);
  qemu_add_opts(&qemu_boot_opts);
  qemu_add_opts(&qemu_sandbox_opts);
  qemu_add_opts(&qemu_add_fd_opts);
  qemu_add_opts(&qemu_object_opts);
  qemu_add_opts(&qemu_tpmdev_opts);
  qemu_add_opts(&qemu_realtime_opts);
  qemu_add_opts(&qemu_msg_opts);
#endif

  init_clocks();
  rtc_clock = QEMU_CLOCK_HOST;

  module_call_init(MODULE_INIT_MACHINE);

  if (machine == NULL) {
    fprintf(stderr, "No Xbox support in QEMU!\n");
    exit(1);
  }

  vga_interface_type = VGA_NONE;

  if (machine->compat_props) {
      qdev_prop_register_global_list(machine->compat_props);
  }
  qemu_add_globals();

  qdev_machine_init();

  qemu_init_main_loop();

  cpudef_init();

  cpu_exec_init_all();

  printf("RAM Size is %luMB\n",ram_size/1024/1024);
  
  /* Generate a blank bios */
  char path[PATH_MAX + 1];
  get_tmp_filename(path,sizeof(path));
  printf("Temporary file in '%s'\n",path);
  FILE* f = fopen(path,"wb");
  if (f == NULL) {
    fprintf(stderr,"Unable to generate bios in %s\n",path);
  }
  size_t l = 1 * 1024 * 1024;
  while(l--) {
    uint8_t b;
    fwrite(&b,1,1,f);
  }
  fclose(f);
  bios_name = path;

  QEMUMachineInitArgs args = { .ram_size = ram_size,
                               .boot_order = machine->default_boot_order,
                               .kernel_filename = NULL,
                               .kernel_cmdline = NULL,
                               .initrd_filename = NULL,
                               .cpu_model = "pentium3" };

  dprintf("Going to init machine!\n");
  machine->init(&args);

    current_machine = machine;

    cpu_synchronize_all_post_init();

    /* Run headless .. or not*/
#if 1
    DisplayState *ds = init_displaystate();

    sdl_display_init(ds, false, false);
#endif

    /* must be after terminal init, SDL library changes signal handlers */
    os_setup_signal_handling();

    qdev_machine_creation_done();

    if (rom_load_all() != 0) {
        fprintf(stderr, "rom loading failed\n");
        exit(1);
    }

    /* TODO: once all bus devices are qdevified, this should be done
     * when bus is created by qdev.c */
    qemu_register_reset(qbus_reset_all_fn, sysbus_get_default());
//    qemu_run_machine_init_done_notifiers(); {
//    notifier_list_notify(&machine_init_done_notifiers, NULL);
//  }

    qemu_system_reset(VMRESET_SILENT);

#if 0
//vm_start();

        vm_state_notify(1, RUN_STATE_RUNNING);
        resume_all_vcpus();
#endif

    os_setup_post();




  /* Search the nv2a */

  PCIBus* bus = pci_find_primary_bus();
  PCIDevice *agp;
#if 1
  agp = pci_find_device(bus, 0, PCI_DEVFN(30, 0));// = pci_create_simple(host_bus, PCI_DEVFN(30, 0), "xbox-agp");
#else
  int ret = pci_qdev_find_device("xbox-agp",&agp);
#endif
  PCIBus *agp_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(agp));
  PCIDevice *dev = pci_find_device(agp_bus, 0, PCI_DEVFN(0, 0));
#define NV2A_DEVICE(obj) \
    OBJECT_CHECK(struct NV2AState, (obj), "nv2a")
  d = NV2A_DEVICE(dev);

  unsigned int r;
  for (r = 0; r < PCI_NUM_REGIONS; ++r) {
    PCIIORegion *region = &dev->io_regions[r];
    if (!region->size) { continue; }
//    if (!(region->type & PCI_BASE_ADDRESS_SPACE_MEMORY)) { continue; }

    MemoryRegion *mr = region->memory;

    dprintf("Found memory %s\n",mr->name);
    if (!strcmp(mr->name,"nv2a-mmio")) {

      MemoryRegion* subregion;
      QTAILQ_FOREACH(subregion, &mr->subregions, subregions_link) {
        dprintf("Found subregion: %s%s\n",subregion->name,subregion->ram?" (RAM)":"");

        if (!strcmp(subregion->name,"nv2a-ramin")) {
          dprintf("Got ramin!\n");
          ramin = subregion;
          continue;
        }

      }

      continue;
    }

    if (!strcmp(mr->name,"nv2a-vram-pci")) {
      vram = mr;
      continue;
    }

  }

  if (ramin == NULL) {
    fprintf(stderr,"Error: Unable to find ramin hack!\n");
    exit(1);
  }
  if (vram == NULL) {
    fprintf(stderr,"Error: Unable to find vram!\n");
    exit(1);
  }

  return;
}

static void load_memory(File* f, bool is_ramin) {

  uint8_t* ramin_ptr;
  if (is_ramin) {
    ramin_ptr = memory_region_get_ram_ptr(ramin);
  }

  while(1) {

    uint32_t size = read_uint32(f);
    if (size == 0) { break; }
    uint32_t addr = read_uint32(f);

    if (is_eof(f)) {
      fprintf(stderr,"Warning: Broken record\n");
      return;
    }

    dprintf("Updating %X, size %X%s\n",addr,size,is_ramin?" (RAMIN)":"");

    /* Update the given page */
    while(size != 0) {
      uint8_t buffer[4096];
      uint32_t l = sizeof(buffer);
      if (l > size) { l = size; }
      /* Check if the GPU Overwrote the memory itself.. */
      bool dirty = memory_region_test_and_clear_dirty(is_ramin?ramin:vram, addr, l, DIRTY_MEMORY_MIGRATION);
#if 1
      dirty = false; // Force writes!
#endif
      if (dirty) {
        //FIXME: Check *where* this page was modified
        dprintf("GPU modified memory, skipping\n");
        fseek(f->handle,l,SEEK_CUR);
//        usleep(1000*1000);
      } else {
        if (is_ramin) {
          fread(&ramin_ptr[addr],l,1,f->handle);
        } else {
          fread(&buffer,l,1,f->handle);
          cpu_physical_memory_write(addr, buffer, l); //FIXME: Make sure this updates the dirty bit!!!
        }
      }
      addr += l;
      size -= l;
    }

  }
  return;

}

/* Stolen from ui/console.c */
static void ppm_save(const char *filename, struct DisplaySurface *ds,
                     Error **errp)
{
    int width = pixman_image_get_width(ds->image);
    int height = pixman_image_get_height(ds->image);
    int fd;
    FILE *f;
    int y;
    int ret;
    pixman_image_t *linebuf;

    trace_ppm_save(filename, ds);
    fd = qemu_open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd == -1) {
        error_setg(errp, "failed to open file '%s': %s", filename,
                   strerror(errno));
        return;
    }
    f = fdopen(fd, "wb");
    ret = fprintf(f, "P6\n%d %d\n%d\n", width, height, 255);
    if (ret < 0) {
        linebuf = NULL;
        goto write_err;
    }
    linebuf = qemu_pixman_linebuf_create(PIXMAN_BE_r8g8b8, width);
    for (y = 0; y < height; y++) {
        qemu_pixman_linebuf_fill(linebuf, ds->image, width, 0, y);
        clearerr(f);
        ret = fwrite(pixman_image_get_data(linebuf), 1,
                     pixman_image_get_stride(linebuf), f);
        (void)ret;
        if (ferror(f)) {
            goto write_err;
        }
    }

out:
    qemu_pixman_image_unref(linebuf);
    fclose(f);
    return;

write_err:
    error_setg(errp, "failed to write to file '%s': %s", filename,
               strerror(errno));
    unlink(filename);
    goto out;
}

/* Takes a screenshot of the current image output in qemu */
static void screenshot(const char* path, QemuConsole *con) {
  Error *err;
  DisplaySurface *surface = qemu_console_surface(con);
  ppm_save(path, surface, &err);
  return;
}

/* COMMAND HANDLERS */

/* Replays a recording */
static void replay(File* f) {

  /* Read header */
  uint8_t version = read_uint8(f);
  if (version > 0) {
    fprintf(stderr, "The recorded file is not supported\n");
    exit(1);
  }
  ram_addr_t ram_size = read_uint32(f);

  /* Attempt to prepare the Xbox Memory + GPU */
  init_qemu(ram_size);

  /* Find console */
  QemuConsole *con = qemu_console_lookup_by_index(0);
  if (con == NULL) {
      fprintf(stderr,"Can't find qemu console\n");
      exit(1);
  }

  /* Check machine memory and turn on logging */
  if (ram_size != memory_region_size(vram)) {
    fprintf(stderr,"Different VRAM size in machine and log!\n");
    exit(1);
  }
  memory_region_set_log(vram, true, DIRTY_MEMORY_MIGRATION);
  memory_region_reset_dirty(vram, 0, memory_region_size(vram), DIRTY_MEMORY_MIGRATION);
  memory_region_set_log(ramin, true, DIRTY_MEMORY_MIGRATION);
  memory_region_reset_dirty(ramin, 0, memory_region_size(ramin), DIRTY_MEMORY_MIGRATION);

  /* Make sure we don't record the playback */
  record_playback = true;

  unsigned int frame = 0;
  int last_io = 0;
  while(!is_eof(f)) {

#if 0
    bool nonblocking = last_io > 0;
    last_io = main_loop_wait(nonblocking);
#endif

//usleep(1000*2); //FIXME: Getting random errors without this, not sure where it races..
 
    uint8_t flags = read_uint8(f);
    bool write = flags & 1;
    bool data = flags & 2;
    uint32_t addr = read_uint32(f);
    uint64_t val;
    if (data) {
      val = read_uint64(f);
    }
    uint8_t len = read_uint8(f);
    unsigned int size = len;
fprintf(stderr,"F%i\n",frame);
    dprintf("Frame %i { %s: 0x%08X 0x%lX %i }\n",frame,write?"Read":"Write",addr,data?val:0xDEADBEEF,size);

    dprintf("Updating memory image\n");
    load_memory(f,false);
    load_memory(f,true);

    dprintf("Searching block\n");
    const NV2ABlockInfo* block = find_block(addr);
    if (block == NULL) {
      fprintf(stderr, "Error: Block could not be found!\n");
      continue;
    }

    dprintf("Executing block [%s] access\n",block->name);
    if (write) {

#if 0
      uint32_t pmc_pending_addr = 0x0+0x100;
      uint32_t pmc_enabled_addr = 0x0+0x140;
      if (addr == pmc_pending_addr) {
        if (data) {
          uint32_t pmc_pending = block->ops.read((void*)d,(hwaddr)(pmc_pending_addr-block->offset),4);
          /* Probably waiting for fifo */
          if (val & (1<<12)) {
            printf("Killing pgraph intr: 0x%08X\n",val);
          }
        }
      }
#endif

      block->ops.write((void*)d,(hwaddr)addr-block->offset,val,size);

#if 1 /* Wait for pusher to complete */
      uint32_t dma_put_addr = 0x2000+0x1240;
      uint32_t dma_get_addr = 0x2000+0x1244;
      if (addr == dma_put_addr) {
        //FIXME: Stop re-recording shortly
        unsigned int t = 1000000;
        while(t--) {
          uint32_t dma_put = block->ops.read((void*)d,(hwaddr)(dma_put_addr-block->offset),4);
          uint32_t dma_get = block->ops.read((void*)d,(hwaddr)(dma_get_addr-block->offset),4);
          if (dma_put == dma_get) {
            break;
          }
          usleep(1); //FIXME: Something better for a task switch would be nice
        }
        //FIXME: Turn it back on
      }
#endif

    } else {

#if 1 /* Do sync work */
      uint32_t pmc_pending_addr = 0x0+0x100;
      uint32_t pmc_enabled_addr = 0x0+0x140;
      if (addr == pmc_pending_addr) {
        if (data) {
          uint32_t pmc_pending = block->ops.read((void*)d,(hwaddr)(pmc_pending_addr-block->offset),4);
          /* Probably waiting for pgraph puller */
          if (val & (1<<12)) {
            printf("Waiting for pgraph interrupt\n");
            int t = 10000; /* Timeout for puller wait, hacky.. */
            while (!(pmc_pending & (1 << 12))) {
              usleep(1);
              if (!t--) { break; }
            }
          }
          /* Probably had a vblank because crtc needs some love */
          if (val & 0x01000000) {
            if (!(pmc_pending & 0x01000000)) {
              printf("\nForcing vblank!\n\n");
              graphic_hw_update(con);
            }
          }

        }
      }
#endif

#if 1 /* Allow reads? */

      uint64_t val_now = block->ops.read((void*)d,(hwaddr)addr-block->offset,size);

#if 0 /* Verify reads? */
      if (data) {
        if (val_now != val) {
          printf("GPU returned different value! Expected 0x%08X, got 0x%08X\n",val,val_now);
          usleep(1000*1000);
        }
      }
#endif

#endif

    }


    if (frame % 200 == 0) {
      dprintf("Taking screenshot\n");
      /* Generate a filename */
      char buf[256];
      sprintf(buf,"/tmp/nv2a-replay-frame%i.ppm",frame);
      char* path = temporary_file(buf);
      dprintf("Path is '%s'\n",path);
      screenshot(path,con);
      free(path);
    }

    frame++;
  
  }

  /* Reached end of file */
  return;

}

/* ENTRY POINT */

int main(int argc, char* argv[]) {

  if (argc != 2) {
    fprintf(stderr, "Error: Wrong number of arguments!\n");
    return 1;
  }

  const char* path = argv[1];

  printf("Loading '%s'\n",path);
  File* f = open_file(path);

  replay(f);

  close_file(f);

  return 0;
}
