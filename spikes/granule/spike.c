/*
 * Granule spike: does hv_vm_protect() enforce 4 KiB stage-2 permissions
 * inside one 16 KiB host page when the VM uses HV_IPA_GRANULE_4KB?
 *
 * Usage: spike 4k|16k guest.bin
 */
#include <Hypervisor/Hypervisor.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define MEM_SIZE (64ul << 20)
#define EC(esr) (((esr) >> 26) & 0x3f)

static hv_vcpu_t vcpu;
static hv_vcpu_exit_t *ex;

static const char *ec_name(uint64_t esr)
{
    switch (EC(esr)) {
    case 0x16: return "HVC";
    case 0x20: return "IABT";
    case 0x24: return "DABT";
    default: return "other";
    }
}

static const char *run_at(uint64_t pc, uint64_t *ipa)
{
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc);
    hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5);
    if (hv_vcpu_run(vcpu) != HV_SUCCESS)
        errx(1, "hv_vcpu_run failed");
    if (ex->reason != HV_EXIT_REASON_EXCEPTION)
        errx(1, "unexpected exit reason %u", ex->reason);
    *ipa = ex->exception.physical_address;
    return ec_name(ex->exception.syndrome);
}

static int check(const char *label, uint64_t pc, const char *want)
{
    uint64_t ipa;
    const char *got = run_at(pc, &ipa);
    int ok = strcmp(got, want) == 0;
    printf("%-44s want %-4s got %-5s ipa=0x%llx  %s\n", label, want, got,
           (unsigned long long)ipa, ok ? "PASS" : "FAIL");
    return ok;
}

static double perf(void)
{
    uint64_t ipa;
    hv_vcpu_set_reg(vcpu, HV_REG_X0, 0x100000);
    hv_vcpu_set_reg(vcpu, HV_REG_X2, MEM_SIZE - 0x100000);
    hv_vcpu_set_reg(vcpu, HV_REG_X3, 1);
    run_at(0x180, &ipa); /* first touch */
    hv_vcpu_set_reg(vcpu, HV_REG_X0, 0x100000);
    hv_vcpu_set_reg(vcpu, HV_REG_X2, MEM_SIZE - 0x100000);
    hv_vcpu_set_reg(vcpu, HV_REG_X3, 64);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    run_at(0x180, &ipa);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    double n = 64.0 * (MEM_SIZE - 0x100000) / 4096;
    return s / n * 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        errx(1, "usage: spike 4k|16k guest.bin");
    int want4k = strcmp(argv[1], "4k") == 0;

    hv_ipa_granule_t def;
    if (hv_vm_config_get_default_ipa_granule(&def) != HV_SUCCESS)
        errx(1, "get_default_ipa_granule failed");
    printf("default IPA granule: %s\n",
           def == HV_IPA_GRANULE_4KB ? "4KB" : "16KB");

    hv_vm_config_t cfg = hv_vm_config_create();
    hv_return_t r = hv_vm_config_set_ipa_granule(
        cfg, want4k ? HV_IPA_GRANULE_4KB : HV_IPA_GRANULE_16KB);
    if (r != HV_SUCCESS)
        errx(1, "set_ipa_granule failed: 0x%x", r);
    if ((r = hv_vm_create(cfg)) != HV_SUCCESS)
        errx(1, "hv_vm_create failed: 0x%x", r);

    uint8_t *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED)
        err(1, "mmap");
    FILE *f = fopen(argv[2], "rb");
    if (!f)
        err(1, "%s", argv[2]);
    size_t n = fread(mem, 1, 0x4000, f);
    fclose(f);
    printf("guest image: %zu bytes\n", n);

    if ((r = hv_vm_map(mem, 0, MEM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE |
                                             HV_MEMORY_EXEC)) != HV_SUCCESS)
        errx(1, "hv_vm_map failed: 0x%x", r);

    hv_return_t p1 = hv_vm_protect(0x1000, 0x1000, HV_MEMORY_READ);
    hv_return_t p2 =
        hv_vm_protect(0x2000, 0x1000, HV_MEMORY_READ | HV_MEMORY_WRITE);
    hv_return_t p3 = hv_vm_protect(0x3000, 0x1000, HV_MEMORY_EXEC);
    printf("hv_vm_protect 4 KiB ranges: R=0x%x RW=0x%x X=0x%x\n", p1, p2, p3);

    if ((r = hv_vcpu_create(&vcpu, &ex, NULL)) != HV_SUCCESS)
        errx(1, "hv_vcpu_create failed: 0x%x", r);

    int ok = 1;
    if (p1 == HV_SUCCESS && p2 == HV_SUCCESS && p3 == HV_SUCCESS) {
        ok &= check("(a) store to R page [0x1000]", 0x000, "DABT");
        ok &= check("(b) store to RW page [0x2000]", 0x040, "HVC");
        ok &= check("(c) exec in RW (no X) page [0x2000]", 0x080, "IABT");
        ok &= check("(d) exec in X-only page [0x3000]", 0x0c0, "HVC");
        check("(e) load from X-only page [0x3000] (info)", 0x100, "DABT");
        ok &= check("(f) load from R page [0x1000]", 0x140, "HVC");
    } else {
        printf("4 KiB protections not accepted under this granule\n");
        ok = 0;
    }
    printf("perf: %.2f ns per 4 KiB-stride store over %lu MiB\n", perf(),
           (MEM_SIZE >> 20) - 1);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
