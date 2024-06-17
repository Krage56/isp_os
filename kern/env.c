/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>
#include <inc/vsyscall.h>

#include <kern/env.h>
#include <kern/kdebug.h>
#include <kern/macro.h>
#include <kern/monitor.h>
#include <kern/pmap.h>
#include <kern/pmap.h>
#include <kern/sched.h>
#include <kern/timer.h>
#include <kern/traceopt.h>
#include <kern/trap.h>
#include <kern/vsyscall.h>

/* Currently active environment */
struct Env *curenv = NULL;

#ifdef CONFIG_KSPACE
/* All environments */
struct Env env_array[NENV];
struct Env *envs = env_array;
#else
/* All environments */
struct Env *envs = NULL;
#endif

/* Virtual syscall page address */
volatile int *vsys;

/* Free environment list
 * (linked by Env->env_link) */
static struct Env *env_free_list;


/* NOTE: Should be at least LOGNENV */
#define ENVGENSHIFT 12

/* Converts an envid to an env pointer.
 * If checkperm is set, the specified environment must be either the
 * current environment or an immediate child of the current environment.
 *
 * RETURNS
 *     0 on success, -E_BAD_ENV on error.
 *   On success, sets *env_store to the environment.
 *   On error, sets *env_store to NULL. */
int
envid2env(envid_t envid, struct Env **env_store, bool need_check_perm) {
    struct Env *env;

    /* If envid is zero, return the current environment. */
    if (!envid) {
        *env_store = curenv;
        return 0;
    }

    /* Look up the Env structure via the index part of the envid,
     * then check the env_id field in that struct Env
     * to ensure that the envid is not stale
     * (i.e., does not refer to a _previous_ environment
     * that used the same slot in the envs[] array). */
    env = &envs[ENVX(envid)];
    if (env->env_status == ENV_FREE || env->env_id != envid) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    /* Check that the calling environment has legitimate permission
     * to manipulate the specified environment.
     * If checkperm is set, the specified environment
     * must be either the current environment
     * or an immediate child of the current environment. */
    if (need_check_perm && env != curenv && env->env_parent_id != curenv->env_id) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    *env_store = env;
    return 0;
}

/* Mark all environments in 'envs' as free, set their env_ids to 0,
 * and insert them into the env_free_list.
 * Make sure the environments are in the free list in the same order
 * they are in the envs array (i.e., so that the first call to
 * env_alloc() returns envs[0]).
 */
void
env_init(void) {
    /* Allocate vsys array with kzalloc_region().
     * Don't forget about rounding.
     * kzalloc_region only works with current_space != NULL */
    // LAB 12: Your code here
    size_t uvsys_size = ROUNDUP(sizeof(int) * NVSYSCALLS, PAGE_SIZE);
    void* uvsys_mem = kzalloc_region(uvsys_size);
    assert(uvsys_mem != NULL);

    int res = map_region(&kspace, UVSYS, &kspace, (uintptr_t) uvsys_mem, uvsys_size, PROT_R | PROT_USER_);
    if (res < 0)
        panic("env_init: %i\n", res);

    vsys = (volatile int*) uvsys_mem;

    /* Allocate envs array with kzalloc_region().
     * Don't forget about rounding.
     * kzalloc_region() only works with current_space != NULL */
    // LAB 8: Your code here

    size_t envs_mem_size = ROUNDUP(sizeof(struct Env) * NENV, PAGE_SIZE);
    void*  envs_mem = kzalloc_region(envs_mem_size);
    assert(envs_mem != NULL);

    /* Map envs to UENVS read-only,
     * but user-accessible (with PROT_USER_ set) */
    // LAB 8: Your code here

    res = map_region(&kspace, UENVS, &kspace, (uintptr_t) envs_mem, envs_mem_size, PROT_R | PROT_USER_);
    if (res < 0)
        panic("env_init: %i\n", res);

    envs = (struct Env*) envs_mem;

    /* Set up envs array */

    // LAB 3: Your code here
    env_free_list = envs;
    for (int32_t env_counter = 0; env_counter < NENV; env_counter++)
    {
        struct Env* current_env = &(envs[env_counter]);

        current_env->env_link = (env_counter != NENV - 1) ? &(envs[env_counter + 1]) : NULL;
        current_env->env_id = 0;
        current_env->env_parent_id = 0;
        current_env->env_type = ENV_TYPE_KERNEL;
        current_env->env_status = ENV_FREE;
        current_env->binary = NULL;
    }
}

/* Allocates and initializes a new environment.
 * On success, the new environment is stored in *newenv_store.
 *
 * Returns
 *     0 on success, < 0 on failure.
 * Errors
 *    -E_NO_FREE_ENV if all NENVS environments are allocated
 *    -E_NO_MEM on memory exhaustion
 */
int
env_alloc(struct Env **newenv_store, envid_t parent_id, enum EnvType type) {

    struct Env *env;
    if (!(env = env_free_list))
        return -E_NO_FREE_ENV;

    /* Allocate and set up the page directory for this environment. */
    int res = init_address_space(&env->address_space);
    if (res < 0) return res;

    /* Generate an env_id for this environment */
    int32_t generation = (env->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
    /* Don't create a negative env_id */
    if (generation <= 0) generation = 1 << ENVGENSHIFT;
    env->env_id = generation | (env - envs);

    /* Set the basic status variables */
    env->env_parent_id = parent_id;
#ifdef CONFIG_KSPACE
    env->env_type = ENV_TYPE_KERNEL;
#else
    env->env_type = type;
#endif
    env->env_status = ENV_RUNNABLE;
    env->env_runs = 0;

    /* Clear out all the saved register state,
     * to prevent the register values
     * of a prior environment inhabiting this Env structure
     * from "leaking" into our new environment */
    memset(&env->env_tf, 0, sizeof(env->env_tf));

    /* Set up appropriate initial values for the segment registers.
     * GD_UD is the user data (KD - kernel data) segment selector in the GDT, and
     * GD_UT is the user text (KT - kernel text) segment selector (see inc/memlayout.h).
     * The low 2 bits of each segment register contains the
     * Requestor Privilege Level (RPL); 3 means user mode, 0 - kernel mode.  When
     * we switch privilege levels, the hardware does various
     * checks involving the RPL and the Descriptor Privilege Level
     * (DPL) stored in the descriptors themselves */

#ifdef CONFIG_KSPACE
    env->env_tf.tf_ds = GD_KD;
    env->env_tf.tf_es = GD_KD;
    env->env_tf.tf_ss = GD_KD;
    env->env_tf.tf_cs = GD_KT;

    // LAB 3: Your code here:
    static uintptr_t stack_top = 0x2000000;
    env->env_tf.tf_rsp = stack_top - (env - envs) * 2 * PAGE_SIZE;
#else
    env->env_tf.tf_ds = GD_UD | 3;
    env->env_tf.tf_es = GD_UD | 3;
    env->env_tf.tf_ss = GD_UD | 3;
    env->env_tf.tf_cs = GD_UT | 3;
    env->env_tf.tf_rsp = USER_STACK_TOP;
#endif

    /* For now init trapframe with IF set */
    env->env_tf.tf_rflags = FL_IF | (type == ENV_TYPE_FS ? FL_IOPL_3 : FL_IOPL_0);

    /* Clear the page fault handler until user installs one. */
    env->env_pgfault_upcall = 0;

    /* Also clear the IPC receiving flag. */
    env->env_ipc_recving = 0;

    /* Commit the allocation */
    env_free_list = env->env_link;
    *newenv_store = env;

    if (trace_envs) cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, env->env_id);
    return 0;
}

bool __builtin_uaddmull_overflow(uint64_t first, uint64_t second, uint64_t multiplier, uint64_t* result)
{
    bool overflow = false;
    uint64_t multiplied = 0;
    if (__builtin_umull_overflow(second, multiplier, &multiplied))
        overflow = true;
    if (__builtin_uaddl_overflow(first, multiplied, result))
        overflow = true;
    return overflow;
}

bool valid_bounds(uint64_t start_addr, uint64_t count, uint64_t size, Bounds bounds)
{
    if (start_addr < bounds.start || start_addr > bounds.end)
        return false;
    
    uint64_t end_addr = 0;
    if (__builtin_uaddmull_overflow(start_addr, count, size, &end_addr))
        return false;

    if (end_addr < bounds.start || end_addr > bounds.end)
        return false;
    
    return true;
}

/* Pass the original ELF image to binary/size and bind all the symbols within
 * its loaded address space specified by image_start/image_end.
 * Make sure you understand why you need to check that each binding
 * must be performed within the image_start/image_end range.
 */
static int
bind_functions(struct Env *env, uint8_t *binary, size_t size, uintptr_t image_start, uintptr_t image_end) {
    // LAB 3: Your code here:

    /* NOTE: find_function from kdebug.c should be used */
    Bounds binary_bounds = {(uint64_t)binary, (uint64_t)(binary + size)}; // [binary, binary+size]
    //Bounds image_bounds = {(uint64_t)image_start, (uint64_t)image_end};

    struct Elf* elf_data = (struct Elf*)binary;

    struct Proghdr* prog_headers = NULL;
    if (__builtin_uaddl_overflow((uint64_t)binary, elf_data->e_phoff, (uint64_t*)&prog_headers))
        return -E_INVALID_EXE;
    if (!valid_bounds((uint64_t)prog_headers, elf_data->e_phnum, sizeof(struct Proghdr), binary_bounds))
        return -E_INVALID_EXE;

    //Bounds bounds[Loaded_segments_num] = {};

    // for (uint64_t prog_header_iter = 0, bounds_iter = 0; prog_header_iter < elf_data->e_phnum; prog_header_iter++)
    // {
    //     struct Proghdr* cur_prog_header = prog_headers + prog_header_iter;

    //     if (cur_prog_header->p_type != ELF_PROG_LOAD)
    //         continue;

    //     bounds[bounds_iter].start = cur_prog_header->p_va;
    //     if (__builtin_uaddl_overflow(cur_prog_header->p_va, cur_prog_header->p_memsz, &bounds[bounds_iter].end))
    //         return -E_INVALID_EXE;
    //     if (bounds[bounds_iter].start < image_start || bounds[bounds_iter].end > image_end)
    //         return -E_INVALID_EXE;
    //     bounds_iter++;
    //     if (bounds_iter >= Loaded_segments_num)
    //         return -E_INVALID_EXE;
    // }

    struct Secthdr* sec_headers = NULL;
    if (__builtin_uaddl_overflow((uint64_t)binary, elf_data->e_shoff, (uint64_t*)&sec_headers))
        return -E_INVALID_EXE;
    if (!valid_bounds((uint64_t)sec_headers, elf_data->e_shnum, sizeof(struct Secthdr), binary_bounds))
        return -E_INVALID_EXE;

    uint16_t sec_headers_num  = (uint16_t) elf_data->e_shnum;
    uint16_t string_tab_ind = 0;
    bool string_tab_found = false;
    struct Elf64_Sym* sym_tab = NULL;
    size_t sym_num = 0;

    for (uint16_t sec_header_iter = 0; sec_header_iter < sec_headers_num; sec_header_iter++)
    {
        struct Secthdr* cur_sec_header = sec_headers + sec_header_iter;

        if (cur_sec_header->sh_type == ELF_SHT_SYMTAB && cur_sec_header->sh_size != 0)
        {
            if (__builtin_uaddl_overflow((uint64_t)binary, cur_sec_header->sh_offset, (uint64_t*)&sym_tab))
                return -E_INVALID_EXE;
            sym_num = (size_t) (cur_sec_header->sh_size / sizeof(struct Elf64_Sym));
        }

        else if (cur_sec_header->sh_type == ELF_SHT_STRTAB)
        {
            if (sec_header_iter != elf_data->e_shstrndx)
            {
                string_tab_ind = sec_header_iter;
                string_tab_found = true;
            }
        }
    }

    if (!string_tab_found || sym_tab == NULL)
    {
        cprintf("hello %d %p\n", string_tab_ind, sym_tab);
        return -E_INVALID_EXE;
    }
    
    struct Secthdr* string_tab_hdr = sec_headers + string_tab_ind;

    char* string_tab = NULL;
    if (__builtin_uaddl_overflow((uint64_t)binary, string_tab_hdr->sh_offset, (uint64_t*)&string_tab))
        return -E_INVALID_EXE;
    if (!valid_bounds((uint64_t)string_tab, string_tab_hdr->sh_size, 1, binary_bounds))
        return -E_INVALID_EXE;

    if (!valid_bounds((uint64_t)sym_tab, sym_num, sizeof(struct Elf64_Sym), binary_bounds))
        return -E_INVALID_EXE;

    for (size_t sym_iter = 0; sym_iter < sym_num; sym_iter++)
    {
        struct Elf64_Sym* cur_sym = sym_tab + sym_iter;
        uint8_t st_info = cur_sym->st_info;

        if ((ELF64_ST_TYPE(st_info) != STT_OBJECT) 
            || (ELF64_ST_BIND(st_info) != STB_GLOBAL))
            continue;

        if (cur_sym->st_name == 0)
            continue;

        char* sym_name = NULL;
        if (__builtin_uaddl_overflow((uint64_t)string_tab, cur_sym->st_name, (uint64_t*)&sym_name))
            return -E_INVALID_EXE;
        if ((uint64_t)sym_name < binary_bounds.start || (uint64_t)sym_name > binary_bounds.end)
            return -E_INVALID_EXE;
        uint64_t name_len = strnlen(sym_name, binary_bounds.end - (uint64_t)sym_name);
        if (name_len == binary_bounds.end - (uint64_t)sym_name)
            return -E_INVALID_EXE;
        uintptr_t sym_value = (uintptr_t) cur_sym->st_value;

        bool is_correct = false;

        for (uint64_t prog_header_iter = 0; prog_header_iter < elf_data->e_phnum; prog_header_iter++)
        {
            struct Proghdr* cur_prog_header = prog_headers + prog_header_iter;

            if (cur_prog_header->p_type != ELF_PROG_LOAD)
                continue;

            uint64_t start = cur_prog_header->p_va;
            uint64_t end = 0;
            if (__builtin_uaddl_overflow(cur_prog_header->p_va, cur_prog_header->p_memsz, &end))
                return -E_INVALID_EXE;
            if (start < image_start || end > image_end)
                return -E_INVALID_EXE;
            
            if ((ELF64_ST_BIND(cur_sym->st_info) == STB_GLOBAL) && (sym_value >= start) && (sym_value <= end))  // incorrect
            {
                is_correct = true;
                break;
            }
        }

        // for (unsigned iter = 0; iter < Loaded_segments_num; iter++)
        // {
        //     if ((sym_value >= bounds[iter].start) && (sym_value <= bounds[iter].end))
        //     {
        //         is_correct = true;
        //         break;
        //     }
        // }

        if (!is_correct)
            panic("bind_functions: sym_value is outside of image: sym_value = %p\n", (void*) sym_value);

        uintptr_t zero = 0;
        if (sym_value != 0 && memcmp((void*)sym_value, &zero, sizeof(uintptr_t)))
            continue;

        uintptr_t offset = find_function(sym_name);
        if (offset == 0)
            continue;

        if (offset != 0 && sym_value != 0) {
            memcpy((void*)sym_value, &offset, sizeof(uintptr_t));
        }
    }

    return 0;
}

/* Set up the initial program binary, stack, and processor flags
 * for a user process.
 * This function is ONLY called during kernel initialization,
 * before running the first environment.
 *
 * This function loads all loadable segments from the ELF binary image
 * into the environment's user memory, starting at the appropriate
 * virtual addresses indicated in the ELF program header.
 * At the same time it clears to zero any portions of these segments
 * that are marked in the program header as being mapped
 * but not actually present in the ELF file - i.e., the program's bss section.
 *
 * All this is very similar to what our boot loader does, except the boot
 * loader also needs to read the code from disk.  Take a look at
 * LoaderPkg/Loader/Bootloader.c to get ideas.
 *
 * Finally, this function maps one paLoad each program segment into memory
 *   at the address specified in the ELF section header.ge for the program's initial stack.
 *env->binary = binary;
 * load_icode returns -E_INVALID_EXE if it encounters problems.
 *  - How might load_icode fail?  What might be wrong with the given input?
 *
 * Hints:
 *   Load each program segment into memory
 *   at the address specified in the ELF section header.
 *   You should only load segments with ph->p_type == ELF_PROG_LOAD.
 *   Each segment's address can be found in ph->p_va
 *   and its size in memory can be found in ph->p_memsz.
 *   The ph->p_filesz bytes from the ELF binary, starting at
 *   'binary + ph->p_offset', should be copied to address
 *   ph->p_va.  Any remaining memory bytes should be cleared to zero.
 *   (The ELF header should have ph->p_filesz <= ph->p_memsz.)
 *
 *   All page protection bits should be user read/write for now.
 *   ELF segments are not necessarily page-aligned, but you can
 *   assume for this function that no two segments will touch
 *   the same page.
 *
 *   You must also do something with the program's entry point,
 *   to make sure that the environment starts executing there.
 *   What?  (See env_run() and env_pop_tf() below.) */
static int
load_icode(struct Env *env, uint8_t *binary, size_t size) {
    // LAB 3: Your code here
    
    struct Elf* elf_data = (struct Elf*) binary;
    uintptr_t image_start = 0;
    uintptr_t image_end = 0;

    if (elf_data->e_magic != ELF_MAGIC)
        return -E_INVALID_EXE;

    if (elf_data->e_shentsize != sizeof(struct Secthdr))
        return -E_INVALID_EXE;

    if (elf_data->e_phentsize != sizeof(struct Proghdr))
        return -E_INVALID_EXE;
        
    if (elf_data->e_shstrndx >= elf_data->e_shnum)
        return -E_INVALID_EXE;

    struct Proghdr* ph = (struct Proghdr*)(binary + elf_data->e_phoff);
    for (int counter = 0; counter < elf_data->e_phnum; counter++)
    {
        if (ph->p_type == ELF_PROG_LOAD)
        {
            if (image_start == 0 || ph->p_va < image_start)
                image_start = ph->p_va;
            if (image_start == 0 || ph->p_va < image_start)
                image_start = ph->p_va;

            if (image_end == 0 || ph->p_va + ph->p_memsz > image_end)
                image_end = ph->p_va + ph->p_memsz;

            int res = map_region(&kspace, ph->p_va, NULL, 0, ph->p_memsz, PROT_RWX | ALLOC_ZERO);
            if (res < 0) panic("map prog to kspace: %i \n", res);

            memcpy((void*)(ph->p_va), (void*)(binary + ph->p_offset), (size_t)(ph->p_filesz));
            uint32_t prog_flags = ph->p_flags; 
            cprintf("prog_flags: 0x%x \n", prog_flags);

            res = map_region(&env->address_space, ph->p_va, &kspace, ph->p_va, ph->p_memsz, prog_flags | PROT_USER_);
            if (res < 0) panic("map prog to env->address_space: %i \n", res);
        }
        
        ph += 1;

        unmap_region(&kspace, ph->p_va, ph->p_memsz);
    }
    env->binary = binary;
    env->env_tf.tf_rip = (uintptr_t) elf_data->e_entry;

    // int err = bind_functions(env, binary, size, image_start, image_end);
    // if (err < 0)
    //     panic("bind_functions: %i", err);

    // ph = (struct Proghdr*)(binary + elf_data->e_phoff);
    // for (unsigned iter = 0; iter < elf_data->e_phnum; iter++)
    // {
    //     unmap_region(&kspace, ph->p_va, ph->p_memsz);
    //     ph += 1;
    // }

    // LAB 8: Your code here

    int res = map_region(&env->address_space, USER_STACK_TOP - USER_STACK_SIZE, NULL, 0, USER_STACK_SIZE, PROT_R | PROT_W | PROT_USER_ | ALLOC_ZERO);
    if (res < 0) 
        panic("load_icode: %i \n", res);
    /* NOTE: When merging origin/lab10 put this hunk at the end
     *       of the function, when user stack is already mapped. */
    if (env->env_type == ENV_TYPE_FS) {
        /* If we are about to start filesystem server we need to pass
         * information about PCIe MMIO region to it. */
        struct AddressSpace *as = switch_address_space(&env->address_space);
        env->env_tf.tf_rsp = make_fs_args((char *)env->env_tf.tf_rsp);
        switch_address_space(as);
    }

    return 0;
}

/* Allocates a new env with env_alloc, loads the named elf
 * binary into it with load_icode, and sets its env_type.
 * This function is ONLY called during kernel initialization,
 * before running the first user-mode environment.
 * The new env's parent ID is set to 0.
 */
void
env_create(uint8_t *binary, size_t size, enum EnvType type) {
    // LAB 3: Your code here
    // LAB 8: Your code here
    // LAB 10: Your code here
    struct Env* env = NULL;
    int error = env_alloc(&env, 0, type);
    if (error < 0)
        panic("env_alloc: %i", error);

    // env->env_type = type;
    error = load_icode(env, binary, size);
    if (error < 0)
        panic("load_icode: %i", error);

    // env->env_parent_id = 0;
    if (type == ENV_TYPE_FS)
        env->env_tf.tf_rflags |= FL_IOPL_3;
    return;    
}


/* Frees env and all memory it uses */
void
env_free(struct Env *env) {

    /* Note the environment's demise. */
    if (trace_envs) cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, env->env_id);

#ifndef CONFIG_KSPACE
    /* If freeing the current environment, switch to kern_pgdir
     * before freeing the page directory, just in case the page
     * gets reused. */
    if (&env->address_space == current_space)
        switch_address_space(&kspace);

    static_assert(MAX_USER_ADDRESS % HUGE_PAGE_SIZE == 0, "Misaligned MAX_USER_ADDRESS");
    release_address_space(&env->address_space);
#endif

    /* Return the environment to the free list */
    env->env_status = ENV_FREE;
    env->env_link = env_free_list;
    env_free_list = env;
}

/* Frees environment env
 *
 * If env was the current one, then runs a new environment
 * (and does not return to the caller)
 */
void
env_destroy(struct Env *env) {
    /* If env is currently running on other CPUs, we change its state to
     * ENV_DYING. A zombie environment will be freed the next time
     * it traps to the kernel. */

    // LAB 3: Your code here
    // LAB 10: Your code here
    env_free(env);
    if (env == curenv)
        sched_yield();

    /* Reset in_page_fault flags in case *current* environment
     * is getting destroyed after performing invalid memory access. */
    // LAB 8: Your code here
    in_page_fault = 0;
}

#ifdef CONFIG_KSPACE
void
csys_exit(void) {
    if (!curenv) panic("curenv = NULL");
    env_destroy(curenv);
}

void
csys_yield(struct Trapframe *tf) {
    memcpy(&curenv->env_tf, tf, sizeof(struct Trapframe));
    sched_yield();
}
#endif

/* Restores the register values in the Trapframe with the 'ret' instruction.
 * This exits the kernel and starts executing some environment's code.
 *
 * This function does not return.
 */

_Noreturn void
env_pop_tf(struct Trapframe *tf) {
    asm volatile(
            "movq %0, %%rsp\n"
            "movq 0(%%rsp), %%r15\n"
            "movq 8(%%rsp), %%r14\n"
            "movq 16(%%rsp), %%r13\n"
            "movq 24(%%rsp), %%r12\n"
            "movq 32(%%rsp), %%r11\n"
            "movq 40(%%rsp), %%r10\n"
            "movq 48(%%rsp), %%r9\n"
            "movq 56(%%rsp), %%r8\n"
            "movq 64(%%rsp), %%rsi\n"
            "movq 72(%%rsp), %%rdi\n"
            "movq 80(%%rsp), %%rbp\n"
            "movq 88(%%rsp), %%rdx\n"
            "movq 96(%%rsp), %%rcx\n"
            "movq 104(%%rsp), %%rbx\n"
            "movq 112(%%rsp), %%rax\n"
            "movw 120(%%rsp), %%es\n"
            "movw 128(%%rsp), %%ds\n"
            "addq $152,%%rsp\n" /* skip tf_trapno and tf_errcode */
            "iretq" ::"g"(tf)
            : "memory");

    /* Mostly to placate the compiler */
    panic("Reached unrecheble\n");
}

/* Context switch from curenv to env.
 * This function does not return.
 *
 * Step 1: If this is a context switch (a new environment is running):
 *       1. Set the current environment (if any) back to
 *          ENV_RUNNABLE if it is ENV_RUNNING (think about
 *          what other states it can be in),
 *       2. Set 'curenv' to the new environment,
 *       3. Set its status to ENV_RUNNING,
 *       4. Update its 'env_runs' counter,
 * Step 2: Use env_pop_tf() to restore the environment's
 *       registers and starting execution of process.

 * Hints:
 *    If this is the first call to env_run, curenv is NULL.
 *
 *    This function loads the new environment's state from
 *    env->env_tf.  Go back through the code you wrote above
 *    and make sure you have set the relevant parts of
 *    env->env_tf to sensible values.
 */
_Noreturn void
env_run(struct Env *env) {
    assert(env);

    if (trace_envs_more) {
        const char *state[] = {"FREE", "DYING", "RUNNABLE", "RUNNING", "NOT_RUNNABLE"};
        if (curenv) cprintf("[%08X] env stopped: %s\n", curenv->env_id, state[curenv->env_status]);
        cprintf("[%08X] env started: %s\n", env->env_id, state[env->env_status]);
    }

    // LAB 3: Your code here
    // LAB 8: Your code here
    if (curenv != env)
    {
        if (curenv != NULL && curenv->env_status == ENV_RUNNING)
            curenv->env_status = ENV_RUNNABLE;
        
        curenv = env;
        curenv->env_status = ENV_RUNNING;
        curenv->env_runs++;
        switch_address_space(&curenv->address_space);
    }
    
    env_pop_tf(&curenv->env_tf);

    while (1)
        ;
}
