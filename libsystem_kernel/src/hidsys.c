/*
 * Minimal fake com.apple.iohideventsystem (IOHIDEventSystemClient MIG server).
 *
 * Responds to IOHIDEventSystem MIG subsystem (70000..70028) requests so
 * GameController / UIKit startup succeeds without hitting error-path cleanup
 * bugs inside IOKit.framework's HID client.
 *
 * Constraints: freestanding (no libc, no malloc, no TLS), called under ipc_lock
 * (never acquire ipc_lock; mach_vm_allocate uses sys_mmap directly).
 */

#include "hidsys.h"
#include "ipc.h"
#include "linux.h"
#include <stdint.h>
#include <stddef.h>

extern int g_ipc_trace;
extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags);

/* Binary plist ('bplist00') payloads verified against CoreFoundation's
 * CFPropertyListCreateWithData(..., kCFPropertyListMutableContainers, ...). */
static const uint8_t g_bplist_empty_array[42] = {
    'b', 'p', 'l', 'i', 's', 't', '0', '0',
    0xa0,                                           /* empty CFArray */
    0x08,                                           /* offset table: obj 0 @ 8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* unused */
    0x01, 0x01,                                     /* offsetIntSize=1, objectRefSize=1 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, /* numObjects = 1 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* topObject = 0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09  /* offsetTableOffset = 9 */
};

static const uint8_t g_bplist_empty_dict[42] = {
    'b', 'p', 'l', 'i', 's', 't', '0', '0',
    0xd0,                                           /* empty CFDictionary */
    0x08,                                           /* offset table: obj 0 @ 8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* unused */
    0x01, 0x01,                                     /* offsetIntSize=1, objectRefSize=1 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, /* numObjects = 1 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* topObject = 0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09  /* offsetTableOffset = 9 */
};

/* Freestanding helpers */
static size_t hs_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void hs_memset(void *dst, uint8_t v, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) p[i] = v;
}

static void hs_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void hs_put_u32(uint8_t *p, uint32_t v) {
    hs_memcpy(p, &v, sizeof(v));
}

static void hs_put_u64(uint8_t *p, uint64_t v) {
    hs_memcpy(p, &v, sizeof(v));
}

static void hs_print_str(const char *s) {
    if (s) sys_write(2, s, hs_strlen(s));
}

static void hs_print_uint(uint64_t val) {
    char buf[24];
    int i = 23;
    buf[i--] = '\0';
    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0 && i >= 0) {
            buf[i--] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    hs_print_str(&buf[i + 1]);
}

static void hs_print_int(int32_t val) {
    if (val < 0) {
        hs_print_str("-");
        hs_print_uint((uint64_t)(-(int64_t)val));
    } else {
        hs_print_uint((uint64_t)val);
    }
}

int hidsys_is_enabled(void) {
    const char *env = lsk_getenv("IOSWINE_HIDSYS");
    if (env && env[0] == '0' && env[1] == '\0') return 0;
    return 1;
}

static uint32_t hs_reply_disp(const mach_msg_header_t *req) {
    uint32_t disp = MACH_MSGH_BITS_LOCAL(req->msgh_bits);
    if (disp == 0) disp = MACH_MSG_TYPE_MOVE_SEND_ONCE;
    return disp;
}

static void hs_fill_ndr(uint8_t *dst) {
    NDR_record_t ndr;
    hs_memset(&ndr, 0, sizeof(ndr));
    ndr.int_rep = 1; /* NDR little-endian */
    hs_memcpy(dst, &ndr, sizeof(ndr));
}

static void hs_init_reply_hdr(uint8_t *buf, const mach_msg_header_t *req,
                              uint32_t size, int is_complex) {
    mach_msg_header_t rep;
    uint32_t bits = MACH_MSGH_BITS(hs_reply_disp(req), 0);
    if (is_complex) bits |= MACH_MSGH_BITS_COMPLEX;
    rep.msgh_bits = bits;
    rep.msgh_size = size;
    rep.msgh_remote_port = req->msgh_local_port;
    rep.msgh_local_port = MACH_PORT_NULL;
    rep.msgh_voucher_port = MACH_PORT_NULL;
    rep.msgh_id = req->msgh_id + HIDSYS_MIG_REPLY_OFFSET;
    hs_memcpy(buf, &rep, sizeof(rep));
}

/* Standard 36-byte MIG status reply (hdr + NDR + RetCode). */
static int hs_build_retcode_reply(const mach_msg_header_t *req,
                                  uint8_t *reply_buf, uint32_t max_reply_size,
                                  uint32_t *out_reply_size, int32_t rc) {
    const uint32_t rep_sz = 36;
    if (max_reply_size < rep_sz) return 0;
    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 0);
    hs_fill_ndr(reply_buf + 24);
    hs_put_u32(reply_buf + 32, (uint32_t)rc);
    *out_reply_size = rep_sz;
    return 1;
}

/* 40-byte status reply with an extra 32-bit output word at +36
 * (used by set_properties 70013, set_properties_for_service 70010, conforms_to 70006, etc.). */
static int hs_build_retcode_u32_reply(const mach_msg_header_t *req,
                                      uint8_t *reply_buf, uint32_t max_reply_size,
                                      uint32_t *out_reply_size,
                                      int32_t rc, uint32_t out_val) {
    const uint32_t rep_sz = 40;
    if (max_reply_size < rep_sz) return 0;
    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 0);
    hs_fill_ndr(reply_buf + 24);
    hs_put_u32(reply_buf + 32, (uint32_t)rc);
    hs_put_u32(reply_buf + 36, out_val);
    *out_reply_size = rep_sz;
    return 1;
}

/* 44-byte status reply with an extra 64-bit output value at +36
 * (used by create_virtual_service 70026). */
static int hs_build_retcode_u64_reply(const mach_msg_header_t *req,
                                      uint8_t *reply_buf, uint32_t max_reply_size,
                                      uint32_t *out_reply_size,
                                      int32_t rc, uint64_t out_val) {
    const uint32_t rep_sz = 44;
    if (max_reply_size < rep_sz) return 0;
    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 0);
    hs_fill_ndr(reply_buf + 24);
    hs_put_u32(reply_buf + 32, (uint32_t)rc);
    hs_put_u64(reply_buf + 36, out_val);
    *out_reply_size = rep_sz;
    return 1;
}

/* Allocate a fresh OOL page via mach_vm_allocate so ipc_receive's
 * mach_vm_deallocate() on the in-flight buffer safely unmaps it. */
static int hs_alloc_ool(const uint8_t *src, uint32_t size, uint64_t *out_addr) {
    *out_addr = 0;
    if (size == 0) return 0;
    uint64_t addr = 0;
    int kr = mach_vm_allocate(0x103 /* mach_task_self() */, &addr, size, 1 /* VM_FLAGS_ANYWHERE */);
    if (kr != 0 || addr == 0) return -1;
    hs_memcpy((void *)(uintptr_t)addr, src, size);
    *out_addr = addr;
    return 0;
}

/* Write a 16-byte mach_msg_ool_descriptor64_t at desc_ptr (unaligned-safe). */
static void hs_write_ool_desc(uint8_t *desc_ptr, uint64_t addr, uint32_t size) {
    hs_put_u64(desc_ptr + 0, addr);
    desc_ptr[8] = 1;  /* deallocate = TRUE */
    desc_ptr[9] = 0;  /* copy = MACH_MSG_PHYSICAL_COPY */
    desc_ptr[10] = 0; /* pad2 */
    desc_ptr[11] = MACH_MSG_OOL_DESCRIPTOR; /* 1 */
    hs_put_u32(desc_ptr + 12, size);
}

/* Complex 40-byte reply carrying 1 Mach port descriptor (open 70000). */
static int hs_build_port_reply(const mach_msg_header_t *req,
                               uint8_t *reply_buf, uint32_t max_reply_size,
                               uint32_t *out_reply_size,
                               struct ipc_port *port) {
    const uint32_t rep_sz = 40;
    if (max_reply_size < rep_sz || !port) return 0;
    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 1);
    hs_put_u32(reply_buf + 24, 1); /* msgh_descriptor_count */

    uint8_t *desc = reply_buf + 28;
    ipc_desc_store_port(desc, port);
    ipc_port_retain_send(port);
    desc[10] = MACH_MSG_TYPE_COPY_SEND;
    desc[11] = MACH_MSG_PORT_DESCRIPTOR;

    *out_reply_size = rep_sz;
    return 1;
}

/* Complex 76-byte reply carrying 2 OOL descriptors (copy_matching_services 70001).
 * Client expects: hdr(24) + body(4) + ool0(16) + ool1(16) + NDR(8) + cnt0(4) + cnt1(4). */
static int hs_build_copy_matching_services_reply(const mach_msg_header_t *req,
                                                 uint8_t *reply_buf,
                                                 uint32_t max_reply_size,
                                                 uint32_t *out_reply_size) {
    const uint32_t rep_sz = 76;
    if (max_reply_size < rep_sz) return 0;

    uint32_t sz = (uint32_t)sizeof(g_bplist_empty_array);
    uint64_t addr0 = 0, addr1 = 0;
    if (hs_alloc_ool(g_bplist_empty_array, sz, &addr0) != 0 ||
        hs_alloc_ool(g_bplist_empty_array, sz, &addr1) != 0) {
        return hs_build_retcode_reply(req, reply_buf, max_reply_size,
                                      out_reply_size, 5 /* KERN_FAILURE */);
    }

    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 1);
    hs_put_u32(reply_buf + 24, 2); /* msgh_descriptor_count = 2 */
    hs_write_ool_desc(reply_buf + 28, addr0, sz);
    hs_write_ool_desc(reply_buf + 44, addr1, sz);
    hs_fill_ndr(reply_buf + 60);
    hs_put_u32(reply_buf + 68, sz); /* matchingCnt */
    hs_put_u32(reply_buf + 72, sz); /* behaviorCnt */

    *out_reply_size = rep_sz;
    return 1;
}

/* Complex 60-byte reply carrying 1 OOL descriptor (copy_property 70014 /
 * copy_property_for_service 70011 / copy_properties_for_service 70012).
 * Layout: hdr(24) + body(4) + ool0(16) + NDR(8) + cnt(4) + out_err(4). */
static int hs_build_copy_ool_reply(const mach_msg_header_t *req,
                                   uint8_t *reply_buf, uint32_t max_reply_size,
                                   uint32_t *out_reply_size,
                                   const uint8_t *payload, uint32_t payload_sz,
                                   int32_t inner_err) {
    const uint32_t rep_sz = 60;
    if (max_reply_size < rep_sz) return 0;

    uint64_t addr = 0;
    uint32_t actual_sz = 0;
    if (payload && payload_sz > 0) {
        if (hs_alloc_ool(payload, payload_sz, &addr) != 0) {
            return hs_build_retcode_reply(req, reply_buf, max_reply_size,
                                          out_reply_size, 5 /* KERN_FAILURE */);
        }
        actual_sz = payload_sz;
    }

    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 1);
    hs_put_u32(reply_buf + 24, 1); /* msgh_descriptor_count = 1 */
    hs_write_ool_desc(reply_buf + 28, addr, actual_sz);
    hs_fill_ndr(reply_buf + 44);
    hs_put_u32(reply_buf + 52, actual_sz);
    hs_put_u32(reply_buf + 56, (uint32_t)inner_err);

    *out_reply_size = rep_sz;
    return 1;
}

/* Complex 56-byte reply carrying 1 OOL descriptor (do_client_refresh 70004,
 * copy_event_for_service 70007, copy_matching_event_for_service 70008).
 * Layout: hdr(24) + body(4) + ool0(16) + NDR(8) + cnt(4). */
static int hs_build_ool_56_reply(const mach_msg_header_t *req,
                                 uint8_t *reply_buf, uint32_t max_reply_size,
                                 uint32_t *out_reply_size,
                                 const uint8_t *payload, uint32_t payload_sz) {
    const uint32_t rep_sz = 56;
    if (max_reply_size < rep_sz) return 0;

    uint64_t addr = 0;
    uint32_t actual_sz = 0;
    if (payload && payload_sz > 0) {
        if (hs_alloc_ool(payload, payload_sz, &addr) != 0) {
            return hs_build_retcode_reply(req, reply_buf, max_reply_size,
                                          out_reply_size, 5 /* KERN_FAILURE */);
        }
        actual_sz = payload_sz;
    }

    hs_memset(reply_buf, 0, rep_sz);
    hs_init_reply_hdr(reply_buf, req, rep_sz, 1);
    hs_put_u32(reply_buf + 24, 1); /* msgh_descriptor_count = 1 */
    hs_write_ool_desc(reply_buf + 28, addr, actual_sz);
    hs_fill_ndr(reply_buf + 44);
    hs_put_u32(reply_buf + 52, actual_sz);

    *out_reply_size = rep_sz;
    return 1;
}

static int hs_is_simpleroutine(int32_t msgh_id) {
    switch (msgh_id) {
    case HIDSYS_MIG_RELEASE_NOTIFICATION:
    case HIDSYS_MIG_DISPATCH_EVENT:
    case HIDSYS_MIG_REGISTER_RECORD_CHANGED:
    case HIDSYS_MIG_UNREGISTER_RECORD_CHANGED:
    case HIDSYS_MIG_DISPATCH_EVENT_VIRTUAL_SVC:
        return 1;
    default:
        return 0;
    }
}

int hidsys_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size) {
    if (!hdr || !out_reply_size || send_size < sizeof(mach_msg_header_t)) return 0;
    *out_reply_size = 0;

    int32_t id = hdr->msgh_id;
    int handled = 1;

    /* Simpleroutines or one-way messages with no reply port produce no reply. */
    if (hs_is_simpleroutine(id) || hdr->msgh_local_port == MACH_PORT_NULL) {
        if (g_ipc_trace) {
            hs_print_str("[hidsys] msg id=");
            hs_print_int(id);
            hs_print_str(" -> no-reply\n");
        }
        return 1;
    }

    switch (id) {
    case HIDSYS_MIG_OPEN: {
        struct ipc_port *conn = ipc_get_hidsys_conn_port();
        if (!conn) conn = ipc_get_hidsys_port();
        if (!conn) {
            handled = hs_build_retcode_reply(hdr, reply_buf, max_reply_size,
                                             out_reply_size, 5 /* KERN_FAILURE */);
        } else {
            handled = hs_build_port_reply(hdr, reply_buf, max_reply_size,
                                          out_reply_size, conn);
        }
        break;
    }

    case HIDSYS_MIG_COPY_MATCHING_SERVICES:
        handled = hs_build_copy_matching_services_reply(hdr, reply_buf,
                                                        max_reply_size, out_reply_size);
        break;

    case HIDSYS_MIG_DO_CLIENT_REFRESH:
        handled = hs_build_ool_56_reply(hdr, reply_buf, max_reply_size, out_reply_size,
                                        g_bplist_empty_array,
                                        (uint32_t)sizeof(g_bplist_empty_array));
        break;

    case HIDSYS_MIG_COPY_EVENT_FOR_SERVICE:
    case HIDSYS_MIG_COPY_MATCHING_EVENT_FOR_SVC:
        handled = hs_build_ool_56_reply(hdr, reply_buf, max_reply_size, out_reply_size,
                                        NULL, 0);
        break;

    case HIDSYS_MIG_COPY_PROPERTY:
    case HIDSYS_MIG_COPY_PROPERTY_FOR_SVC:
        /* Empty / unset property: report kIOReturnNotFound in out_err so
         * IOHIDEventSystemClientCopyProperty cleanly returns NULL. */
        handled = hs_build_copy_ool_reply(hdr, reply_buf, max_reply_size, out_reply_size,
                                          NULL, 0, HIDSYS_IORETURN_NOT_FOUND);
        break;

    case HIDSYS_MIG_COPY_PROPERTIES_FOR_SVC:
        handled = hs_build_copy_ool_reply(hdr, reply_buf, max_reply_size, out_reply_size,
                                          g_bplist_empty_dict,
                                          (uint32_t)sizeof(g_bplist_empty_dict), 0);
        break;

    case HIDSYS_MIG_SERVICE_CONFORMS_TO:
    case HIDSYS_MIG_SET_ELEMENT_VALUE_FOR_SVC:
    case HIDSYS_MIG_SET_PROPERTIES_FOR_SVC:
    case HIDSYS_MIG_SET_PROPERTIES:
        handled = hs_build_retcode_u32_reply(hdr, reply_buf, max_reply_size,
                                             out_reply_size, 0, 0);
        break;

    case HIDSYS_MIG_QUEUE_CREATE:
        /* No shared IODataQueue memory entry exists; fail gracefully. */
        handled = hs_build_retcode_reply(hdr, reply_buf, max_reply_size,
                                         out_reply_size, HIDSYS_KERN_NOT_SUPPORTED);
        break;

    case HIDSYS_MIG_CLEAR_SERVICE_CACHE:
    case HIDSYS_MIG_QUEUE_START:
    case HIDSYS_MIG_QUEUE_STOP:
    case HIDSYS_MIG_REGISTER_RECORD_REMOVED:
    case HIDSYS_MIG_REG_PROPERTY_CHANGED_NOTIF:
    case HIDSYS_MIG_UNREG_PROPERTY_CHANGED_NOTIF:
    case HIDSYS_MIG_REG_EVENT_FILTER_NOTIF:
    case HIDSYS_MIG_UNREG_EVENT_FILTER_NOTIF:
    case HIDSYS_MIG_REG_SERVICES_CHANGED_NOTIF:
    case HIDSYS_MIG_UNREG_SERVICES_CHANGED_NOTIF:
        handled = hs_build_retcode_reply(hdr, reply_buf, max_reply_size,
                                         out_reply_size, 0);
        break;

    case HIDSYS_MIG_CREATE_VIRTUAL_SERVICE:
        handled = hs_build_retcode_u64_reply(hdr, reply_buf, max_reply_size,
                                             out_reply_size, 0, 1);
        break;

    default:
        handled = hs_build_retcode_reply(hdr, reply_buf, max_reply_size,
                                         out_reply_size, HIDSYS_MIG_BAD_ID);
        break;
    }

    if (g_ipc_trace) {
        hs_print_str("[hidsys] msg id=");
        hs_print_int(id);
        hs_print_str(" -> reply size=");
        hs_print_uint(*out_reply_size);
        hs_print_str("\n");
    }

    return handled;
}
