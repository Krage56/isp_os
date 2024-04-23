/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/kdebug.h>
#include <kern/macro.h>
#include <kern/traceopt.h>

typedef unsigned long long int ull;

#define ULL_ADD_OVERFLOW(a, b, res) \
   __builtin_uaddll_overflow (a, b, &res)

#define ULL_MUL_OVERFLOW(a, b, res) \
  __builtin_umulll_overflow (a, b, &res)

/* Currently active environment */
struct Env *curenv = NULL; // Указатель на текущий процесс

#ifdef CONFIG_KSPACE
/* All environments */
struct Env env_array[NENV];
struct Env *envs = env_array;
#else
/* All environments */
struct Env *envs = NULL; // Массив всех процессов, используемых и свободных
#endif

/* Free environment list
 * (linked by Env->env_link) */
// Список свободных процессов; Сущности процессов в виде структурок указывают друг на дружку
// по умолчанию NULL
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
    /* Set up envs array */
    // LAB 3: Your code here

    envs->env_id = 0;
    envs->env_status = ENV_FREE;
    env_free_list = envs;

    struct Env *next = env_free_list;
    for (unsigned int i = 1; i < NENV; ++i) {
        envs[i].env_id = 0;
        envs[i].env_status = ENV_FREE;

        next->env_link = envs + i;
        next = envs + i;
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
    // задаём уникальный адрес стека для процесса
    env->env_tf.tf_rsp = stack_top - (env - envs) * (2 * PAGE_SIZE);
#else
    env->env_tf.tf_ds = GD_UD | 3;
    env->env_tf.tf_es = GD_UD | 3;
    env->env_tf.tf_ss = GD_UD | 3;
    env->env_tf.tf_cs = GD_UT | 3;
    env->env_tf.tf_rsp = USER_STACK_TOP;
#endif

    /* Commit the allocation */
    env_free_list = env->env_link;
    *newenv_store = env;

    if (trace_envs)
        cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, env->env_id);
    return 0;
}

/**
 * выполняет поиск по таблице секций,
 * перебирая каждый хэдер из таблицы,
 * пытается 
*/
static int
find_section(struct Secthdr *sc_hdr_tbl, char *sc_tbl_names, int sc_num, uint32_t type, const char *sc_name) {
    struct Secthdr *section_hdr = NULL;
    ull tmp_addr_saver = 0; // ull - unsigned long long int

    for (int i = 0; i < sc_num; i++) {
        // получаем указатель на заголовок из таблицы секций
        section_hdr = sc_hdr_tbl + i;
        
        cprintf("section name = %s; needle = %s\n", (unsigned char *)(uintptr_t)(section_hdr->sh_name), sc_name);

        if (ULL_ADD_OVERFLOW((ull)sc_tbl_names, offsetof(struct Secthdr, sh_type), tmp_addr_saver) ||
            ULL_ADD_OVERFLOW((ull)sc_tbl_names, offsetof(struct Secthdr, sh_name), tmp_addr_saver))
        {
            panic("invalid access to section of table\n");
            return -E_INVALID_EXE;
        }
        if (section_hdr->sh_type == type && !strcmp(sc_name, sc_tbl_names + section_hdr->sh_name))
        {
            return i;
        }
    }
    return -E_UNSPECIFIED;
}

/* Pass the original ELF image to binary/size and bind all the symbols within
 * its loaded address space specified by image_start/image_end.
 * Make sure you understand why you need to check that each binding
 * must be performed within the image_start/image_end range.
 */
static int
bind_functions(struct Env *env, uint8_t *binary, size_t size, uintptr_t image_start, uintptr_t image_end) {
    // LAB 3: Your code here:

    /** @brief #рассуждения
     * то есть у нас есть бинарь, есть процесс, у бинаря есть таблица символов (objdump -X elf)
     * нам нужно в глобальные указатели на функции записать адреса
     * одноименных функций ядра.
     * 
     * Для этого необходимо получить адреса глобальных переменных бинаря
     * с конкретными названиями. ОК - пройдёмся по бинарю, по таблице символов.
     * 
     * Для каждого элемента из таблицы символов смотреть есть ли такая функция 
     * в ядре (посредстовом find_function из kern/kdebug.c).
     * Если да, то положить в указатель адрес функции. 
    */
    struct Elf *_binary = (struct Elf *)binary;
    ull tmp_addr_saver = 0; // ull - unsigned long long int 
    ull real_addr = 0;

    // проверка на невалидное значение смещения
    // от начала elf до входа в таблицу заголовков секций
    if (_binary->e_shoff == 0)
    {
        panic("invalid offset to section header table's\n");
        return -E_INVALID_EXE;
    }
    long offset = offsetof(struct Elf, e_shoff);
    bool overflow = ULL_ADD_OVERFLOW((ull)(binary), (ull)offset, tmp_addr_saver);

    // cprintf("Ovwerflow is %d, offset is %llu, tmd_addr_saver is %llu\n", overflow, (ull)offset, tmp_addr_saver);
    // проверка валидного доступа к таблице секций
    // if (overflow || size < tmp_addr_saver)
    if (overflow || ULL_ADD_OVERFLOW((ull)binary, size, real_addr) || real_addr < tmp_addr_saver)
    {
        panic("binary file has big address, that gets a overflow\n");
        return -E_INVALID_EXE;
    }

    // проверка, что таблица секций не вылезает за размер `_binary`
    // ELF ... ... ... [] [] [] [*] ... ... ...
    if (ULL_MUL_OVERFLOW(_binary->e_shnum, sizeof(struct Secthdr), tmp_addr_saver)      ||
        ULL_ADD_OVERFLOW(offsetof(struct Elf, e_shoff), tmp_addr_saver, tmp_addr_saver) ||
        ULL_ADD_OVERFLOW((ull)binary, size, real_addr) ||
        real_addr < tmp_addr_saver)
    {
        panic("invalid try to access elements of section headers table's\n");
        return -E_FAULT;
    }

    // получаем доступ до таблицы секций
    struct Secthdr *section_headers_table = (struct Secthdr *)(binary + _binary->e_shoff);

    // проверка по спецификации + проверка индекса в пределах
    if (_binary->e_shstrndx == ELF_SHN_UNDEF ||
        _binary->e_shstrndx >= _binary->e_shnum) {
        panic("invalid index of section name string table !\n");
        return -E_INVALID_EXE;
    }

    // проверяем, что доступ до таблицы строк разделов (секций) валиден. 
    if (ULL_MUL_OVERFLOW(_binary->e_shstrndx, sizeof(struct Secthdr), tmp_addr_saver)         ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, (ull)section_headers_table, tmp_addr_saver)          ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, offsetof(struct Secthdr, sh_offset), tmp_addr_saver) ||
        ULL_ADD_OVERFLOW((ull)binary, tmp_addr_saver, tmp_addr_saver)                         ||
        ULL_ADD_OVERFLOW((ull)binary, size, real_addr))
    {
        panic("overflow after trying to access section name string table !\n");
        return -E_FAULT;
    }
    // получаем доступ до таблицы строк с названиями секций
    char *section_names_string_table = (char *)(binary + section_headers_table[_binary->e_shstrndx].sh_offset);

    // индекс раздела (секции) ".strtab" - индекс таблицы строк (имена глоб. функций, переменных
    // в таблице заголовков.

    int strtab_section_idx = find_section(section_headers_table, section_names_string_table,
                                          _binary->e_shnum, ELF_SHT_STRTAB, ".strtab");
    if (strtab_section_idx == -E_UNSPECIFIED) {
        panic("cannot find '.strtab' section!\n");
        return -E_FAULT;
    }
    // индекс раздела (секции) ".symtab" - индекс таблицы символов в таблице заголовков.
    int symtab_section_idx = find_section(section_headers_table, section_names_string_table,
                                          _binary->e_shnum, ELF_SHT_SYMTAB, ".symtab");
    if (symtab_section_idx == -E_UNSPECIFIED) {
        panic("cannot find '.symtab' section!\n");
        return -E_FAULT;
    }
    // зачем нужна таблица символов, если есть таблица строк ?
    // потому что без таблицы символов, мы не найдём в таблице строк название функции, потому что в таблице символов
    // по спецификации лежат индексы к таблице строк

    // попытка доступа до содержимого ".strtab"
    if (ULL_MUL_OVERFLOW(strtab_section_idx, sizeof(struct Secthdr), tmp_addr_saver)          ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, (ull)section_headers_table, tmp_addr_saver)          ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, offsetof(struct Secthdr, sh_offset), tmp_addr_saver) ||
        ULL_ADD_OVERFLOW((ull)binary, tmp_addr_saver, tmp_addr_saver)                         ||
        ULL_ADD_OVERFLOW((ull)binary, size, real_addr))
    {
        panic("invalid try to access to '.strtab' section\n");
        return -E_FAULT;
    }
    char *names = (char *)(binary + section_headers_table[strtab_section_idx].sh_offset);

    // попытка доступа до содержимого ".symtab"
    if (ULL_MUL_OVERFLOW(symtab_section_idx, sizeof(struct Secthdr), tmp_addr_saver)           ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, (ull)section_headers_table, tmp_addr_saver)           ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, offsetof(struct Secthdr, sh_offset), tmp_addr_saver)  ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, (ull)binary, tmp_addr_saver)                          ||
        ULL_ADD_OVERFLOW((ull)binary, size, real_addr))
    {
        panic("invalid try to access to '.symtab' section\n");
        return -E_FAULT;
    }

    // получаем содержимое ".symtab" и получения числа записей в таблице символов
    struct Elf64_Sym* symbols = (struct Elf64_Sym *)(binary + section_headers_table[symtab_section_idx].sh_offset);

    // попытка доступа до числа записей
    if (ULL_MUL_OVERFLOW(symtab_section_idx, sizeof(struct Secthdr), tmp_addr_saver)        ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, (ull)section_headers_table, tmp_addr_saver)        ||
        ULL_ADD_OVERFLOW(tmp_addr_saver, offsetof(struct Secthdr, sh_size), tmp_addr_saver) ||
        ULL_ADD_OVERFLOW((ull)binary, size, real_addr))
    {
        panic("invalid try to access to '.symtab' section\n");
        return -E_FAULT;
    }
    size_t symbols_num = section_headers_table[symtab_section_idx].sh_size / sizeof(struct Elf64_Sym);

    for (size_t symbol_num = 0; symbol_num < symbols_num; symbol_num++) {
        if (ELF64_ST_BIND(symbols[symbol_num].st_info) == STB_GLOBAL &&
            ELF64_ST_TYPE(symbols[symbol_num].st_info) == STT_OBJECT &&
            symbols[symbol_num].st_size == sizeof(void *)) {

            const char *name = names + symbols[symbol_num].st_name;
            uintptr_t function_address = find_function(name);
            if (function_address &&
                symbols[symbol_num].st_value >= image_start &&
                symbols[symbol_num].st_value <= image_end) {
                memcpy((void *)symbols[symbol_num].st_value, &function_address, sizeof(void *));
            }
        }
    }

    return E_NO_ENT;
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
 * loader also needs to read the code from disk. Take a look at
 * LoaderPkg/Loader/Bootloader.c to get ideas.
 *
 * Finally, this function maps one page for the program's initial stack.
 *
 * load_icode returns -E_INVALID_EXE if it encounters problems.
 *  - How might load_icode fail ? 
 *  - What might be wrong with the given input ?
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
    struct Elf *elf = (struct Elf *)binary;

    /* сигнатурная проверка исполняемого файла */
    if (elf->e_magic != ELF_MAGIC                  ||
        elf->e_shentsize != sizeof(struct Secthdr) ||
        elf->e_phentsize != sizeof(struct Proghdr) ||
        elf->e_shstrndx >= elf->e_shnum)
    {
        panic("signatures check of ELF failed !\n");
        return -E_INVALID_EXE;
    }

    // число заголовков программы
    int i = 0;
    int phnum = elf->e_phnum;
    // ph - содержит адрес таблицы заголовков программы
    struct Proghdr *phs = (struct Proghdr *)(binary + elf->e_phoff);
    if (phs == 0) {
        return -E_INVALID_EXE;
    }
    for (; i < phnum; ++i) {
        // загружаем только те сегменты, что могут быть загружены -- .text (cs), .data (ds), ...
        if ((phs + i)->p_type == ELF_PROG_LOAD) {
            // загружаем адрес очередного сегмента
            memcpy((void *)phs[i].p_va, binary + phs[i].p_offset, phs[i].p_filesz);
            // зануляем всё, что идёт после сегмента
            memset((void *)phs[i].p_va + phs[i].p_filesz, 0, phs[i].p_memsz - phs[i].p_filesz);
        }
    }
    // загружаем точку входа в ELF-файл (для его исполнения).
    // e_entry - куда система передаст управление, таким образом запуская процесс
    // env->env_tf - должны лежать сохранённые регистры для безопасного 
    //               переключения между процессами. 
    env->env_tf.tf_rip = elf->e_entry;
    // выполняем связывание, чтобы они пользовательские программы могли использовать
    // функции ядра
    return bind_functions(env, binary, size, elf->e_entry, elf->e_entry + size);
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
    struct Env *new;
    // создали новый процесс, точнее, взяли свободный из свободных,
    // проинициализировали его поля, задали нач. значения регистрам.
    if (env_alloc(&new, 0, type) < 0) {
        panic("Can't allocate new environment\n");
    }
    // связываем бинарь с процессом, который будет его выполнять
    if (0 > load_icode(new, binary, size)) {
        panic("Can't load and bind process with binary\n");
    }
}

/* Frees env and all memory it uses */
void
env_free(struct Env *env) {

    /* Note the environment's demise. */
    if (trace_envs)
        cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, env->env_id);

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
    env->env_status = ENV_DYING;
    env_free(env);
    if (curenv == env) {
        sched_yield();
    }
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

    /* Push RIP on program stack */
    tf->tf_rsp -= sizeof(uintptr_t);
    *((uintptr_t *)tf->tf_rsp) = tf->tf_rip;
    /* Push RFLAGS on program stack */
    tf->tf_rsp -= sizeof(uintptr_t);
    *((uintptr_t *)tf->tf_rsp) = tf->tf_rflags;

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
            "movq (128+48)(%%rsp), %%rsp\n"
            "popfq; ret" ::"g"(tf)
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

    if (curenv && curenv->env_status == ENV_RUNNING) {
        curenv->env_status = ENV_RUNNABLE;
    }

    curenv = env;
    curenv->env_status = ENV_RUNNING;
    curenv->env_runs++;

    env_pop_tf(&curenv->env_tf);
}