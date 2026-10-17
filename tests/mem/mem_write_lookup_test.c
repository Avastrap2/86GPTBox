/* Offline regression test using the production memory implementation.
 * No guest execution, tracing, or device emulation. Whole-program optimization
 * removes unused memory and platform functions, without testing a copied model.
 */
#include "../../src/mem/mem.c"

cpu_state_t cpu_state;
uint32_t recomp_page = UINT32_MAX;
int codegen_in_recompile;

static unsigned checks;
static unsigned failures;
static uint8_t test_ram[0x20000];
static page_t test_pages[0x20];

static void
check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static void
reset_fixture(void)
{
    resetreadlookup();
    memset(&cpu_state, 0, sizeof(cpu_state));
    memset(test_ram, 0, sizeof(test_ram));
    memset(test_pages, 0, sizeof(test_pages));
    ram = test_ram;
    pages = test_pages;
    recomp_page = UINT32_MAX;
    for (unsigned i = 0; i < 0x20; i++) {
        pages[i].mem = ram + i * 4096;
    }
}

static void
set_block_presence(unsigned physical_page, int end_only)
{
    if (end_only)
        pages[physical_page].block_2 = 1;
    else
        pages[physical_page].block = 1;
}

int
main(void)
{
    const uint32_t physical = 0x4000;
    const uint32_t linear = 0x12000000;
    char name[128];

    reset_fixture();
    addwritelookup(linear, physical);
    check(page_lookup[linear >> 12] == NULL, "ordinary RAM keeps direct writes");
    check(writelookup2[linear >> 12] + linear == (uintptr_t) (ram + physical),
          "ordinary RAM translation addresses the physical page");

    for (unsigned end_only = 0; end_only < 2; end_only++) {
        reset_fixture();
        set_block_presence(physical >> 12, end_only);
        addwritelookup(linear, physical);
        snprintf(name, sizeof(name), "%s block selects tracked writes", end_only ? "ending" : "starting");
        check(page_lookup[linear >> 12] == &pages[physical >> 12], name);
        snprintf(name, sizeof(name), "%s block has no direct-write bypass", end_only ? "ending" : "starting");
        check(writelookup2[linear >> 12] == LOOKUP_INV, name);
    }

    reset_fixture();
    recomp_page = physical;
    addwritelookup(linear, physical);
#ifdef USE_DYNAREC
    check(page_lookup[linear >> 12] == &pages[physical >> 12],
          "page being compiled selects tracked writes");
#else
    check(page_lookup[linear >> 12] == NULL, "non-dynarec build ignores recomp_page");
#endif

    const uint32_t alias1 = 0x10000;
    const uint32_t alias2 = 0x15000;
    const uint32_t unrelated = 0x11000;
    for (unsigned remove = 0; remove < 3; remove++) {
        reset_fixture();
        addwritelookup(alias1, physical);
        addwritelookup(alias2, physical);
        /* Same translation bias as alias1, but a different physical page. */
        addwritelookup(unrelated, physical + 0x1000);
        /* Code removal uses zero; compilation may use either alias or a
           third address that was never populated in the write cache. */
        mem_flush_write_page(physical + 0x24, remove == 0 ? alias1 : remove == 1 ? 0 : linear);
        check(writelookup2[alias1 >> 12] == LOOKUP_INV, "flush invalidates first physical-page alias");
        check(writelookup2[alias2 >> 12] == LOOKUP_INV, "flush invalidates second physical-page alias");
        check(writelookup2[unrelated >> 12] != LOOKUP_INV, "flush preserves unrelated physical page");
    }

    reset_fixture();
    set_block_presence(physical >> 12, 0);
    addwritelookup(alias1, physical);
    addwritelookup(alias2, physical);
    mem_flush_write_page(physical, 0);
    check(page_lookup[alias1 >> 12] == NULL, "flush invalidates first tracked alias");
    check(page_lookup[alias2 >> 12] == NULL, "flush invalidates second tracked alias");

    reset_fixture();
    const uint32_t high_alias = 0xfffff000;
    addwritelookup(high_alias, physical);
    mem_flush_write_page(physical, alias1);
    check(writelookup2[high_alias >> 12] == LOOKUP_INV, "flush handles highest virtual page");

    reset_fixture();
    /* Fill and wrap the replacement ring, then flush one physical page.
       Surviving aliases must not depend on the ring's current position. */
    for (unsigned i = 0; i < 320; i++)
        addwritelookup(linear + i * 4096, physical + ((i & 1) << 12));
    mem_flush_write_page(physical, 0);
    for (unsigned i = 0; i < 320; i++) {
        unsigned virtual_page = (linear >> 12) + i;
        int expected_valid = i >= 64 && (i & 1);
        check((writelookup2[virtual_page] != LOOKUP_INV) == expected_valid,
              "replacement-ring flush invalidates only aliases of target page");
    }

    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0;
}
