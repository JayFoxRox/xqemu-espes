#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

uint32_t read32(hwaddr addr) {
    uint32_t buf;
    cpu_memory_rw_debug(first_cpu, addr, &buf, 4, false);
    return buf;
}

static hwaddr locate_kernel_function(unsigned int index) {
    uint32_t tmp = read32(0x80010000 + 0x3C);
    tmp = read32(0x80010000 + tmp + 0x78);
    uint32_t export_count = read32(0x80010000 + tmp + 0x14);
    printf("Found %i exports\n", export_count);
    hwaddr exports = 0x80010000 + read32(0x80010000 + tmp + 0x1C);
    return 0x80010000 + read32(exports + (index - 1) * 4);
}

static void insert_breakpoint(hwaddr address) {
    if (kvm_enabled()) {
        kvm_insert_breakpoint(first_cpu, address, 1, 0);
    } else {
        cpu_breakpoint_insert(first_cpu->env_ptr, address, 0, NULL);
    }
}

static void remove_breakpoint(hwaddr address) {
    if (kvm_enabled()) {
        kvm_remove_breakpoint(first_cpu, address, 1, 0);
    } else {
        cpu_breakpoint_remove(first_cpu->env_ptr, address, 0);
    }
}

static hwaddr hook_kernel_function(unsigned int index) {
    hwaddr address = locate_kernel_function(index);
    insert_breakpoint(address);
    return address;
}

hwaddr step = 0;

static void kernel_hle_vm_state_change(void *opaque, int running, RunState state) {
    CPUState *cpu = first_cpu;
    CPUArchState *env = cpu->env_ptr;

    printf("State change! running: %d state: %d, 0x%08X\n", running, state, env->eip);

    if (running) {
        return;
    }


    if (state == RUN_STATE_DEBUG) {

        if (!runstate_needs_reset()) {
          vm_start();
        }

        /* We came from single stepping */
        if (step != 0) {
            cpu_single_step(cpu, 0);
            insert_breakpoint(step);
            step = 0;
            return;
        }

        if (env->eip == 0xfffffe00) { /* We entered protected mode */
            remove_breakpoint(env->eip);
            insert_breakpoint(0x8005504c); // kernel entry point FIXME: Calculate..
        } else if (env->eip == 0x8005504c) { /* The kernel is starting */
            remove_breakpoint(env->eip);

            /* The kernel is in memory now! we can start hooking stuff */
#if 0
            hwaddr kernel_RtlSprintf = hook_kernel_function(362);
            hwaddr kernel_RtlSnprintf = hook_kernel_function(361);
            hwaddr kernel_RtlEqualString = hook_kernel_function(279);
            hwaddr kernel_RtlCopyString = hook_kernel_function(272);
#endif
            hwaddr kernel_IoCreateFile = hook_kernel_function(66);
printf("%X\n",kernel_IoCreateFile);
usleep(1000000);
        //    hwaddr kernel_IoQueryFileInformation = hook_kernel_function(75);

        } else {
            uint32_t stack = env->regs[R_ESP];
            printf("Stack: 0x%08X\n", stack);
            printf("Return address 0x%08X\n", read32(stack));
            int i;
            for (i = 0; i < 10; i++) {
                uint32_t value = read32(stack + 4 + 4 * i);
                value = read32(value + 8);
                value = read32(value + 8);
                uint32_t str[] = {
                    read32(value),
                    read32(value += 4),
                    read32(value += 4),
                    read32(value += 4),
                    read32(value += 4),
                    read32(value += 4),
                    read32(value += 4)
                };
                printf("%i. 0x%08X '%.*s'\n", i, value, sizeof(str), &str[0]);
            }

            /* Step over the breakpoint */
            step = env->eip;
            remove_breakpoint(step);
            cpu_single_step(cpu, SSTEP_ENABLE|SSTEP_NOIRQ|SSTEP_NOTIMER);


        }

    }


}

void init_kernel_hle(void) {
//    insert_breakpoint(0x90000); // 2BL
    insert_breakpoint(0xfffffe00); // protected mode entry point
    qemu_add_vm_change_state_handler(kernel_hle_vm_state_change, NULL);
}
