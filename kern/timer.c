#include <inc/types.h>
#include <inc/assert.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/stdio.h>
#include <inc/x86.h>
#include <inc/uefi.h>
#include <kern/timer.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/trap.h>
#include <kern/pmap.h>

#define kilo      (1000ULL)
#define Mega      (kilo * kilo)
#define Giga      (kilo * Mega)
#define Tera      (kilo * Giga)
#define Peta      (kilo * Tera)
#define ULONG_MAX ~0UL

#if LAB <= 6
/* Early variant of memory mapping that does 1:1 aligned area mapping
 * in 2MB pages. You will need to reimplement this code with proper
 * virtual memory mapping in the future. */
void *
mmio_map_region(physaddr_t pa, size_t size) {
    void map_addr_early_boot(uintptr_t addr, uintptr_t addr_phys, size_t sz);
    const physaddr_t base_2mb = 0x200000;
    uintptr_t org = pa;
    size += pa & (base_2mb - 1);
    size += (base_2mb - 1);
    pa &= ~(base_2mb - 1);
    size &= ~(base_2mb - 1);
    map_addr_early_boot(pa, pa, size);
    return (void *)org;
}
void *
mmio_remap_last_region(physaddr_t pa, void *addr, size_t oldsz, size_t newsz) {
    return mmio_map_region(pa, newsz);
}
#endif

struct Timer timertab[MAX_TIMERS];
struct Timer *timer_for_schedule;

struct Timer timer_hpet0 = {
        .timer_name = "hpet0",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim0,
        .handle_interrupts = hpet_handle_interrupts_tim0,
};

struct Timer timer_hpet1 = {
        .timer_name = "hpet1",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim1,
        .handle_interrupts = hpet_handle_interrupts_tim1,
};

struct Timer timer_acpipm = {
        .timer_name = "pm",
        .timer_init = acpi_enable,
        .get_cpu_freq = pmtimer_cpu_frequency,
};

void
acpi_enable(void) {
    FADT *fadt = get_fadt();
    outb(fadt->SMI_CommandPort, fadt->AcpiEnable);
    while ((inw(fadt->PM1aControlBlock) & 1) == 0) /* nothing */
        ;
}

bool check_rsdp_checksum(RSDP *p, uint8_t rev) {
    unsigned char sum = 0;
    uint32_t length;
    if (rev) {
        length = p->Length;
    }
    else {
        // In acpi 1.0 RSDP Structure only includes the first 20 bytes
        length = 20;
    }

    for (uint32_t i = 0; i < length; ++i) {
        sum += ((char *)p)[i];
    }    
 
    return sum == 0;
}

bool check_table_checksum(ACPISDTHeader *table_header) {
    unsigned char sum = 0;
 
    for (int i = 0; i < table_header->Length; i++)
    {
        sum += ((char *)table_header)[i];
    }
 
    return sum == 0;
}

static void *
acpi_find_table(const char *sign) {
    /*
     * This function performs lookup of ACPI table by its signature
     * and returns valid pointer to the table mapped somewhere.
     *
     * It is a good idea to checksum tables before using them.
     *
     * HINT: Use mmio_map_region/mmio_remap_last_region
     * before accessing table addresses
     * (Why mmio_remap_last_region is requrired?)
     * HINT: RSDP address is stored in uefi_lp->ACPIRoot
     * HINT: You may want to distunguish RSDT/XSDT
     */
    // LAB 5: Your code here:
    EFI_PHYSICAL_ADDRESS root_ptr = uefi_lp->ACPIRoot;
    // We have to get mapped address from phisical i/o address  
    // Root System Description Pointer 
    static RSDP *root = NULL;
    root = mmio_map_region(root_ptr, sizeof(RSDP));
    if (!root) {
        panic("No rsdp\n");
    }
    uint8_t rev = root->Revision;
    
    if (!check_rsdp_checksum(root, rev)) {
        panic("Inconsistent rsdp\n");
    }
    // Root System Description Table 
    static RSDT * rsdt = NULL;
    // zero value means ACPI 1.0
    uint64_t sdt_addr;
    if (rev) {
        root = mmio_remap_last_region(uefi_lp->ACPIRoot, root, sizeof(RSDP), root->Length);
        rsdt = mmio_map_region(root->XsdtAddress, sizeof(RSDT));
        sdt_addr = root->XsdtAddress;
    }
    else {
        rsdt = mmio_map_region(root->RsdtAddress, sizeof(RSDT));
        sdt_addr = root->RsdtAddress;
    }

    // rsdt checking
    if (!rsdt) {
        panic("No rsdt\n");
    }
    // Now we need to get elements of the array in the RSDT structure
    // that's why we must remap the memeory according to the real lenght
    rsdt = mmio_remap_last_region(sdt_addr, rsdt, sizeof(RSDT), rsdt->h.Length);

    // Finally check the hashsum of table
    if (!check_table_checksum(&(rsdt->h))) {
        panic("Inconsistent rsdt\n");
    }
    
    // Let us search the table by sign
    // Firstly define lenght of entry
    size_t entry_size = 4;
    if (rev) {
        entry_size += 4;
    }
    size_t pointers_num = (rsdt->h.Length - sizeof(RSDT)) / entry_size;
    ACPISDTHeader *h = NULL;
    uint64_t t_addr = 0;
    for (size_t i = 0; i < pointers_num; ++i) {
        memset(&t_addr, 0, sizeof(uint64_t));
        memcpy(&t_addr, (uint8_t *)rsdt->PointerToOtherSDT + i * entry_size, entry_size);

        h = mmio_map_region(t_addr, sizeof(ACPISDTHeader));
        // remap to get all entries of table
        h = mmio_remap_last_region(t_addr, h, sizeof(ACPISDTHeader), h->Length);
        if (!strncmp(h->Signature, sign, sizeof(h->Signature))) {
            if (check_table_checksum(h)) {
                return (void *)h;
            }
            else {
                break;
            }
        }
    }
    return NULL;
}

/* Obtain and map FADT ACPI table address. */
FADT *
get_fadt(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    // HINT: ACPI table signatures are
    //       not always as their names
    static FADT *fadt;
    fadt = acpi_find_table("FACP");
    return fadt;
}

/* Obtain and map RSDP ACPI table address. */
HPET *
get_hpet(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    static HPET *hpet;
    hpet = acpi_find_table("HPET");
    return hpet;
}

/* Getting physical HPET timer address from its table. */
HPETRegister *
hpet_register(void) {
    HPET *hpet_timer = get_hpet();
    if (!hpet_timer->address.address) panic("hpet is unavailable\n");

    uintptr_t paddr = hpet_timer->address.address;
    return mmio_map_region(paddr, sizeof(HPETRegister));
}

/* Debug HPET timer state. */
void
hpet_print_struct(void) {
    HPET *hpet = get_hpet();
    assert(hpet != NULL);
    cprintf("signature = %s\n", (hpet->h).Signature);
    cprintf("length = %08x\n", (hpet->h).Length);
    cprintf("revision = %08x\n", (hpet->h).Revision);
    cprintf("checksum = %08x\n", (hpet->h).Checksum);

    cprintf("oem_revision = %08x\n", (hpet->h).OEMRevision);
    cprintf("creator_id = %08x\n", (hpet->h).CreatorID);
    cprintf("creator_revision = %08x\n", (hpet->h).CreatorRevision);

    cprintf("hardware_rev_id = %08x\n", hpet->hardware_rev_id);
    cprintf("comparator_count = %08x\n", hpet->comparator_count);
    cprintf("counter_size = %08x\n", hpet->counter_size);
    cprintf("reserved = %08x\n", hpet->reserved);
    cprintf("legacy_replacement = %08x\n", hpet->legacy_replacement);
    cprintf("pci_vendor_id = %08x\n", hpet->pci_vendor_id);
    cprintf("hpet_number = %08x\n", hpet->hpet_number);
    cprintf("minimum_tick = %08x\n", hpet->minimum_tick);

    cprintf("address_structure:\n");
    cprintf("address_space_id = %08x\n", (hpet->address).address_space_id);
    cprintf("register_bit_width = %08x\n", (hpet->address).register_bit_width);
    cprintf("register_bit_offset = %08x\n", (hpet->address).register_bit_offset);
    cprintf("address = %08lx\n", (unsigned long)(hpet->address).address);
}

static volatile HPETRegister *hpetReg;
/* HPET timer period (in femtoseconds) */
static uint64_t hpetFemto = 0;
/* HPET timer frequency */
static uint64_t hpetFreq = 0;

/* HPET timer initialisation */
void
hpet_init() {
    if (hpetReg == NULL) {
        nmi_disable();
        hpetReg = hpet_register();
        uint64_t cap = hpetReg->GCAP_ID;
        hpetFemto = (uintptr_t)(cap >> 32);
        if (!(cap & HPET_LEG_RT_CAP)) panic("HPET has no LegacyReplacement mode");

        // cprintf("hpetFemto = %llu\n", hpetFemto);
        hpetFreq = (1 * Peta) / hpetFemto;
        // cprintf("HPET: Frequency = %d.%03dMHz\n", (uintptr_t)(hpetFreq / Mega), (uintptr_t)(hpetFreq % Mega));
        /* Enable ENABLE_CNF bit to enable timer */
        hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
        nmi_enable();
    }
}

/* HPET register contents debugging. */
void
hpet_print_reg(void) {
    cprintf("GCAP_ID = %016lx\n", (unsigned long)hpetReg->GCAP_ID);
    cprintf("GEN_CONF = %016lx\n", (unsigned long)hpetReg->GEN_CONF);
    cprintf("GINTR_STA = %016lx\n", (unsigned long)hpetReg->GINTR_STA);
    cprintf("MAIN_CNT = %016lx\n", (unsigned long)hpetReg->MAIN_CNT);
    cprintf("TIM0_CONF = %016lx\n", (unsigned long)hpetReg->TIM0_CONF);
    cprintf("TIM0_COMP = %016lx\n", (unsigned long)hpetReg->TIM0_COMP);
    cprintf("TIM0_FSB = %016lx\n", (unsigned long)hpetReg->TIM0_FSB);
    cprintf("TIM1_CONF = %016lx\n", (unsigned long)hpetReg->TIM1_CONF);
    cprintf("TIM1_COMP = %016lx\n", (unsigned long)hpetReg->TIM1_COMP);
    cprintf("TIM1_FSB = %016lx\n", (unsigned long)hpetReg->TIM1_FSB);
    cprintf("TIM2_CONF = %016lx\n", (unsigned long)hpetReg->TIM2_CONF);
    cprintf("TIM2_COMP = %016lx\n", (unsigned long)hpetReg->TIM2_COMP);
    cprintf("TIM2_FSB = %016lx\n", (unsigned long)hpetReg->TIM2_FSB);
}

/* HPET main timer counter value. */
uint64_t
hpet_get_main_cnt(void) {
    return hpetReg->MAIN_CNT;
}

/* - Configure HPET timer 0 to trigger every 0.5 seconds on IRQ_TIMER line
 * - Configure HPET timer 1 to trigger every 1.5 seconds on IRQ_CLOCK line
 *
 * HINT To be able to use HPET as PIT replacement consult
 *      LegacyReplacement functionality in HPET spec.
 * HINT Don't forget to unmask interrupt in PIC */
void
hpet_enable_interrupts_tim0(void) {
    // LAB 5: Your code here
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;
    hpetReg->MAIN_CNT = 0;
    hpetReg->TIM0_CONF = (IRQ_TIMER << 9); 
    hpetReg->TIM0_CONF |= HPET_TN_TYPE_CNF | HPET_TN_INT_ENB_CNF | HPET_TN_VAL_SET_CNF;
    hpetReg->TIM0_COMP = hpet_get_main_cnt() + Peta / hpetFemto / 2;
    hpetReg->TIM0_COMP = Peta / hpetFemto / 2;
    pic_irq_unmask(IRQ_TIMER);
}

void
hpet_enable_interrupts_tim1(void) {
    // LAB 5: Your code here
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;
    hpetReg->TIM1_CONF = (IRQ_CLOCK << 9);
    hpetReg->MAIN_CNT = 0;
    hpetReg->TIM1_CONF |= HPET_TN_TYPE_CNF | HPET_TN_INT_ENB_CNF | HPET_TN_VAL_SET_CNF;
    hpetReg->TIM1_COMP = hpet_get_main_cnt() + Peta / hpetFemto / 2 * 3;
    hpetReg->TIM1_COMP = Peta / hpetFemto / 2 * 3;
    pic_irq_unmask(IRQ_CLOCK);
}

void
hpet_handle_interrupts_tim0(void) {
    pic_send_eoi(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim1(void) {
    pic_send_eoi(IRQ_CLOCK);
}

/* Calculate CPU frequency in Hz with the help with HPET timer.
 * HINT Use hpet_get_main_cnt function and do not forget about
 * about pause instruction. */
uint64_t
hpet_cpu_frequency(void) {
    static uint64_t cpu_freq;

    // LAB 5: Your code here
    uint64_t first = hpet_get_main_cnt();
    uint64_t first_tsc = read_tsc();
    uint64_t next = first;
    uint64_t eps = hpetFreq / 10;
    while (next - first < eps) {
        next = hpet_get_main_cnt();
        asm volatile("pause"); //????
    }
    uint64_t next_tsc = read_tsc();
    cpu_freq = (next_tsc - first_tsc) * 10;
    return cpu_freq;
}

uint32_t
pmtimer_get_timeval(void) {
    FADT *fadt = get_fadt();
    return inl(fadt->PMTimerBlock);
}

/* Calculate CPU frequency in Hz with the help with ACPI PowerManagement timer.
 * HINT Use pmtimer_get_timeval function and do not forget that ACPI PM timer
 *      can be 24-bit or 32-bit. */
uint64_t
pmtimer_cpu_frequency(void) {
    static uint64_t cpu_freq;

    // LAB 5: Your code here

    const uint64_t fraction = 10;
    uint32_t pm_cnt = pmtimer_get_timeval();
    uint64_t tsc = read_tsc();
    uint32_t current_pm_cnt = pm_cnt;
    uint64_t d = 0;
    uint64_t delta = PM_FREQ / fraction;
    while (d < delta) {
        current_pm_cnt = pmtimer_get_timeval();
        d = current_pm_cnt - pm_cnt;
        if (pm_cnt - current_pm_cnt <= 0xFFFFFF) {
            d += 0xFFFFFF;
        }
    }
    cpu_freq = (read_tsc() - tsc) * fraction;
    return cpu_freq;
}
