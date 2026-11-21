/* Host unit tests for fake com.apple.iohideventsystem (hidsys.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "hidsys.h"
#include "ipc.h"

/* Host stubs required by hidsys.c when linked standalone */
struct ipc_port {
    int id;
    int retains;
};

static struct ipc_port g_fake_svc_port = { 5, 1 };
static struct ipc_port g_fake_conn_port = { 6, 1 };
static const char *g_env_override = NULL;
static int g_use_env_override = 0;

int g_ipc_trace = 0;

const char *lsk_getenv(const char *name) {
    if (g_use_env_override && strcmp(name, "IOSWINE_HIDSYS") == 0) {
        return g_env_override;
    }
    return getenv(name);
}

struct ipc_port *ipc_get_hidsys_port(void) {
    if (!hidsys_is_enabled()) return NULL;
    return &g_fake_svc_port;
}

struct ipc_port *ipc_get_hidsys_conn_port(void) {
    if (!hidsys_is_enabled()) return NULL;
    return &g_fake_conn_port;
}

void ipc_desc_store_port(uint8_t *desc_ptr, struct ipc_port *port) {
    uint32_t slot = port ? (uint32_t)port->id : 0;
    uint32_t gen = 0x100u + slot;
    memcpy(desc_ptr, &slot, 4);
    memcpy(desc_ptr + 4, &gen, 4);
}

void ipc_port_retain_send(struct ipc_port *port) {
    if (port) port->retains++;
}

int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    (void)target;
    (void)flags;
    if (!address || size == 0) return 4; /* KERN_INVALID_ARGUMENT */
    void *p = calloc(1, (size_t)size);
    if (!p) return 5;
    *address = (uint64_t)(uintptr_t)p;
    return 0;
}

static uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int32_t rd_i32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void init_req(mach_msg_header_t *req, int32_t msgh_id, mach_port_t reply_port) {
    memset(req, 0, sizeof(*req));
    req->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req->msgh_size = sizeof(*req);
    req->msgh_remote_port = 0x503;
    req->msgh_local_port = reply_port;
    req->msgh_id = msgh_id;
}

static void test_gating(void) {
    g_use_env_override = 1;
    g_env_override = NULL;
    assert(hidsys_is_enabled() == 1);
    assert(ipc_get_hidsys_port() != NULL);

    g_env_override = "1";
    assert(hidsys_is_enabled() == 1);
    assert(ipc_get_hidsys_port() != NULL);

    g_env_override = "";
    assert(hidsys_is_enabled() == 1);

    g_env_override = "0";
    assert(hidsys_is_enabled() == 0);
    assert(ipc_get_hidsys_port() == NULL);

    g_use_env_override = 0;
}

static void test_open(void) {
    mach_msg_header_t req;
    uint8_t rep[HIDSYS_MAX_REPLY];
    uint32_t rep_sz = 0;

    int prev_retains = g_fake_conn_port.retains;
    init_req(&req, HIDSYS_MIG_OPEN, 0x903);

    int ok = hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz);
    assert(ok == 1);
    assert(rep_sz == 40);

    const mach_msg_header_t *rh = (const mach_msg_header_t *)rep;
    assert((rh->msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0);
    assert(rh->msgh_size == 40);
    assert(rh->msgh_id == 70100);
    assert(rh->msgh_remote_port == 0x903);
    assert(rd_u32(rep + 24) == 1);
    assert(rd_u32(rep + 28) == (uint32_t)g_fake_conn_port.id);
    assert(rep[38] == MACH_MSG_TYPE_COPY_SEND);
    assert(rep[39] == MACH_MSG_PORT_DESCRIPTOR);
    assert(g_fake_conn_port.retains == prev_retains + 1);
}

static void test_copy_matching_services(void) {
    mach_msg_header_t req;
    uint8_t rep[HIDSYS_MAX_REPLY];
    uint32_t rep_sz = 0;

    init_req(&req, HIDSYS_MIG_COPY_MATCHING_SERVICES, 0x903);
    int ok = hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz);
    assert(ok == 1);
    assert(rep_sz == 76);

    const mach_msg_header_t *rh = (const mach_msg_header_t *)rep;
    assert((rh->msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0);
    assert(rh->msgh_size == 76);
    assert(rh->msgh_id == 70101);
    assert(rd_u32(rep + 24) == 2);

    uint64_t addr0 = rd_u64(rep + 28);
    uint32_t sz0 = rd_u32(rep + 40);
    assert(addr0 != 0 && sz0 == 42);
    assert(rep[28 + 8] == 1);
    assert(rep[28 + 11] == MACH_MSG_OOL_DESCRIPTOR);

    uint64_t addr1 = rd_u64(rep + 44);
    uint32_t sz1 = rd_u32(rep + 56);
    assert(addr1 != 0 && sz1 == 42);
    assert(rep[44 + 8] == 1);
    assert(rep[44 + 11] == MACH_MSG_OOL_DESCRIPTOR);

    assert(rd_u32(rep + 68) == 42);
    assert(rd_u32(rep + 72) == 42);

    const uint8_t *p0 = (const uint8_t *)(uintptr_t)addr0;
    const uint8_t *p1 = (const uint8_t *)(uintptr_t)addr1;
    assert(memcmp(p0, "bplist00\xa0", 9) == 0);
    assert(memcmp(p1, "bplist00\xa0", 9) == 0);

    free((void *)(uintptr_t)addr0);
    free((void *)(uintptr_t)addr1);
}

static void test_properties_and_notifications(void) {
    mach_msg_header_t req;
    uint8_t rep[HIDSYS_MAX_REPLY];
    uint32_t rep_sz = 0;

    /* set_properties (70013) -> 40-byte RetCode=0 + out=0 */
    init_req(&req, HIDSYS_MIG_SET_PROPERTIES, 0x903);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 40);
    assert(((const mach_msg_header_t *)rep)->msgh_id == 70113);
    assert(rd_i32(rep + 32) == 0);
    assert(rd_u32(rep + 36) == 0);

    /* copy_property (70014) -> 60-byte complex reply, empty OOL, inner err = kIOReturnNotFound */
    init_req(&req, HIDSYS_MIG_COPY_PROPERTY, 0x903);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 60);
    assert((((const mach_msg_header_t *)rep)->msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0);
    assert(((const mach_msg_header_t *)rep)->msgh_id == 70114);
    assert(rd_u32(rep + 24) == 1);
    assert(rd_u64(rep + 28) == 0);
    assert(rep[28 + 11] == MACH_MSG_OOL_DESCRIPTOR);
    assert(rd_u32(rep + 40) == 0);
    assert(rd_u32(rep + 52) == 0);
    assert(rd_i32(rep + 56) == HIDSYS_IORETURN_NOT_FOUND);

    /* copy_properties_for_service (70012) -> 60-byte complex reply with empty bplist dict */
    init_req(&req, HIDSYS_MIG_COPY_PROPERTIES_FOR_SVC, 0x903);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 60);
    assert(((const mach_msg_header_t *)rep)->msgh_id == 70112);
    uint64_t dict_addr = rd_u64(rep + 28);
    assert(dict_addr != 0 && rd_u32(rep + 40) == 42 && rd_u32(rep + 52) == 42);
    assert(rd_i32(rep + 56) == 0);
    assert(memcmp((const void *)(uintptr_t)dict_addr, "bplist00\xd0", 9) == 0);
    free((void *)(uintptr_t)dict_addr);

    /* do_client_refresh (70004) -> 56-byte complex reply with empty bplist array */
    init_req(&req, HIDSYS_MIG_DO_CLIENT_REFRESH, 0x903);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 56);
    assert(((const mach_msg_header_t *)rep)->msgh_id == 70104);
    uint64_t ref_addr = rd_u64(rep + 28);
    assert(ref_addr != 0 && rd_u32(rep + 40) == 42 && rd_u32(rep + 52) == 42);
    free((void *)(uintptr_t)ref_addr);

    /* queue_create (70015) -> 36-byte KERN_NOT_SUPPORTED (46) */
    init_req(&req, HIDSYS_MIG_QUEUE_CREATE, 0x903);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 36);
    assert(((const mach_msg_header_t *)rep)->msgh_id == 70115);
    assert(rd_i32(rep + 32) == HIDSYS_KERN_NOT_SUPPORTED);

    /* queue_start / queue_stop / clear_cache / notification registrations -> 36-byte RetCode=0 */
    int32_t ack_ids[] = {
        HIDSYS_MIG_CLEAR_SERVICE_CACHE,
        HIDSYS_MIG_QUEUE_START,
        HIDSYS_MIG_QUEUE_STOP,
        HIDSYS_MIG_REGISTER_RECORD_REMOVED,
        HIDSYS_MIG_REG_PROPERTY_CHANGED_NOTIF,
        HIDSYS_MIG_UNREG_PROPERTY_CHANGED_NOTIF,
        HIDSYS_MIG_REG_EVENT_FILTER_NOTIF,
        HIDSYS_MIG_UNREG_EVENT_FILTER_NOTIF,
        HIDSYS_MIG_REG_SERVICES_CHANGED_NOTIF,
        HIDSYS_MIG_UNREG_SERVICES_CHANGED_NOTIF,
    };
    for (size_t i = 0; i < sizeof(ack_ids) / sizeof(ack_ids[0]); i++) {
        init_req(&req, ack_ids[i], 0x903);
        assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
        assert(rep_sz == 36);
        assert(((const mach_msg_header_t *)rep)->msgh_id == (mach_msg_id_t)(ack_ids[i] + 100));
        assert(rd_i32(rep + 32) == 0);
    }

    /* create_virtual_service (70026) -> 44-byte RetCode=0, id=1 */
    init_req(&req, HIDSYS_MIG_CREATE_VIRTUAL_SERVICE, 0x903);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 44);
    assert(rd_i32(rep + 32) == 0);
    assert(rd_u64(rep + 36) == 1);
}

static void test_simpleroutines_and_unknown_ids(void) {
    mach_msg_header_t req;
    uint8_t rep[HIDSYS_MAX_REPLY];
    uint32_t rep_sz = 999;

    int32_t simple_ids[] = {
        HIDSYS_MIG_RELEASE_NOTIFICATION,
        HIDSYS_MIG_DISPATCH_EVENT,
        HIDSYS_MIG_REGISTER_RECORD_CHANGED,
        HIDSYS_MIG_UNREGISTER_RECORD_CHANGED,
        HIDSYS_MIG_DISPATCH_EVENT_VIRTUAL_SVC,
    };
    for (size_t i = 0; i < sizeof(simple_ids) / sizeof(simple_ids[0]); i++) {
        init_req(&req, simple_ids[i], 0x903);
        rep_sz = 999;
        assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
        assert(rep_sz == 0);
    }

    /* Any routine sent with MACH_PORT_NULL reply port must emit 0-byte reply */
    init_req(&req, HIDSYS_MIG_COPY_MATCHING_SERVICES, MACH_PORT_NULL);
    assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
    assert(rep_sz == 0);

    /* Unknown IDs -> MIG_BAD_ID (-303) */
    int32_t bad_ids[] = { 69999, 70029, 70999, 99999 };
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        init_req(&req, bad_ids[i], 0x903);
        assert(hidsys_process_msg(&req, sizeof(req), rep, sizeof(rep), &rep_sz) == 1);
        assert(rep_sz == 36);
        const mach_msg_header_t *rh = (const mach_msg_header_t *)rep;
        assert((rh->msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0);
        assert(rh->msgh_size == 36);
        assert(rh->msgh_id == (mach_msg_id_t)(bad_ids[i] + 100));
        assert(rep[28] == 1); /* NDR int_rep */
        assert(rd_i32(rep + 32) == HIDSYS_MIG_BAD_ID);
    }

    /* Error guards */
    assert(hidsys_process_msg(NULL, sizeof(req), rep, sizeof(rep), &rep_sz) == 0);
    init_req(&req, HIDSYS_MIG_OPEN, 0x903);
    assert(hidsys_process_msg(&req, 8, rep, sizeof(rep), &rep_sz) == 0);
    assert(hidsys_process_msg(&req, sizeof(req), rep, 16, &rep_sz) == 0);
}

int main(void) {
    test_gating();
    test_open();
    test_copy_matching_services();
    test_properties_and_notifications();
    test_simpleroutines_and_unknown_ids();
    printf("PASS: hidsys_test all scenarios passed\n");
    return 0;
}
