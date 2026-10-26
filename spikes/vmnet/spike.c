/*
 * vmnet spike: can an unprivileged, ad-hoc signed process start a shared
 * (NAT) vmnet interface, with the classic API and with the macOS 26
 * vmnet_network API?
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <unistd.h>
#include <vmnet/vmnet.h>

static const char *rs(vmnet_return_t r)
{
    switch (r) {
    case VMNET_SUCCESS: return "VMNET_SUCCESS";
    case VMNET_FAILURE: return "VMNET_FAILURE";
    case VMNET_INVALID_ARGUMENT: return "VMNET_INVALID_ARGUMENT";
    case VMNET_INVALID_ACCESS: return "VMNET_INVALID_ACCESS";
    case VMNET_NOT_AUTHORIZED: return "VMNET_NOT_AUTHORIZED";
    default: return "other";
    }
}

static void report(const char *api, vmnet_return_t st, xpc_object_t params)
{
    printf("%s: %s (%u)\n", api, rs(st), st);
    if (st == VMNET_SUCCESS && params) {
        const char *mac = xpc_dictionary_get_string(params, vmnet_mac_address_key);
        uint64_t mtu = xpc_dictionary_get_uint64(params, vmnet_mtu_key);
        uint64_t mps = xpc_dictionary_get_uint64(params, vmnet_max_packet_size_key);
        const char *start = xpc_dictionary_get_string(params, vmnet_start_address_key);
        const char *end = xpc_dictionary_get_string(params, vmnet_end_address_key);
        const char *mask = xpc_dictionary_get_string(params, vmnet_subnet_mask_key);
        printf("  mac=%s mtu=%llu max_packet=%llu range=%s-%s mask=%s\n",
               mac ? mac : "?", mtu, mps, start ? start : "?",
               end ? end : "?", mask ? mask : "?");
    }
}

int main(void)
{
    dispatch_queue_t q = dispatch_queue_create("vmnet", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    printf("uid=%d\n", getuid());

    /* Classic API. */
    xpc_object_t desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(desc, vmnet_operation_mode_key, VMNET_SHARED_MODE);
    __block interface_ref iface = vmnet_start_interface(
        desc, q, ^(vmnet_return_t st, xpc_object_t params) {
            report("vmnet_start_interface(shared)", st, params);
            dispatch_semaphore_signal(sem);
        });
    if (iface == NULL)
        printf("vmnet_start_interface returned NULL\n");
    else
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (iface) {
        vmnet_stop_interface(iface, q, ^(vmnet_return_t st) { (void)st; dispatch_semaphore_signal(sem); });
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }

    /* macOS 26 network API. */
    if (__builtin_available(macOS 26.0, *)) {
        vmnet_return_t st = 0;
        vmnet_network_configuration_ref cfg =
            vmnet_network_configuration_create(VMNET_SHARED_MODE, &st);
        printf("vmnet_network_configuration_create: %s (%u)\n", cfg ? "ok" : rs(st), st);
        if (cfg) {
            vmnet_network_ref net = vmnet_network_create(cfg, &st);
            printf("vmnet_network_create: %s (%u)\n", net ? "ok" : rs(st), st);
            if (net) {
                xpc_object_t d2 = xpc_dictionary_create(NULL, NULL, 0);
                interface_ref i2 = vmnet_interface_start_with_network(
                    net, d2, q, ^(vmnet_return_t s2, xpc_object_t params) {
                        report("vmnet_interface_start_with_network", s2, params);
                        dispatch_semaphore_signal(sem);
                    });
                if (i2 == NULL)
                    printf("vmnet_interface_start_with_network returned NULL\n");
                else
                    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            }
        }
    }
    return 0;
}
