#include "launchd.h"
#include "xpcwire.h"
#include "linux.h"
#include <stdint.h>
#include <stddef.h>

extern int g_ipc_trace;

/* Freestanding string / memory helpers */
static size_t ld_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int ld_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int ld_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static void ld_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void ld_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

/* Freestanding logging helpers */
static void ld_print_str(const char *s) {
    if (!s) return;
    sys_write(2, s, ld_strlen(s));
}

static void ld_print_uint(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        while (val > 0) {
            buf[idx--] = '0' + (val % 10);
            val /= 10;
        }
    }
    ld_print_str(&buf[idx + 1]);
}

static void ld_print_hex(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        const char hex[] = "0123456789abcdef";
        while (val > 0) {
            buf[idx--] = hex[val & 0xf];
            val >>= 4;
        }
    }
    ld_print_str(&buf[idx + 1]);
}

/* Built-in service resolver hook (direct calls avoid arm64e PAC relocs in __DATA,__const) */
extern struct ipc_port *ipc_get_hidsys_port(void);
static struct ipc_port *ld_lookup_builtin(const char *name) {
    if (ld_strcmp(name, "com.apple.backboard.display.services") == 0) {
        return ipc_get_bks_display_port();
    }
    if (ld_strcmp(name, "com.apple.iohideventsystem") == 0) {
        return ipc_get_hidsys_port();
    }
    return NULL;
}

/* Dynamic registered services table */
#define MAX_REGISTERED_SERVICES 64
typedef struct {
    char name[128];
    struct ipc_port *port;
    int in_use;
} registered_service_t;

static registered_service_t g_registered_services[MAX_REGISTERED_SERVICES];

/* Minimal XPC Serializer: builds reply dictionary with key "error" = error_code (40 bytes) */
static uint32_t xpc_serialize_error_reply(uint8_t *buf, uint32_t max_size, int64_t error_code) {
    xw_builder b;
    if (xw_builder_init(&b, buf, max_size) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_add_int64(&b, "error", error_code) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* Minimal XPC Serializer: builds empty reply dictionary (success, error=0) (20 bytes) */
static uint32_t xpc_serialize_empty_reply(uint8_t *buf, uint32_t max_size) {
    xw_builder b;
    if (xw_builder_init(&b, buf, max_size) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* Serializer: builds complex lookup reply dictionary (96 bytes)
 * Keys: "error" (int64 0), "req_pid" (uint64 sys_getpid), "rec_execcnt" (uint64 1), "port" (mach_send 0xd000)
 */
static uint32_t xpc_serialize_lookup_dict(uint8_t *dst, uint32_t max_size) {
    xw_builder b;
    if (xw_builder_init(&b, dst, max_size) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_add_int64(&b, "error", 0) != XW_OK) return 0;
    if (xw_dict_add_uint64(&b, "req_pid", (uint64_t)sys0(SYS_getpid)) != XW_OK) return 0;
    if (xw_dict_add_uint64(&b, "rec_execcnt", 1) != XW_OK) return 0;
    if (xw_dict_add_mach_send(&b, "port", 0, MACH_MSG_TYPE_COPY_SEND) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* Process incoming launchd Mach message */
int launchd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    if (!hdr || send_size < sizeof(mach_msg_header_t)) return 0;
    if (max_reply_size < sizeof(mach_msg_header_t) + 40) return 0;

    xw_envelope env;
    if (xw_envelope_parse(hdr, send_size, &env) != XW_OK) return 0;

    uint32_t routine = env.id & 0xffffffU;

    char service_name[256];
    ld_memset(service_name, 0, sizeof(service_name));
    mach_port_name_t req_port = MACH_PORT_NULL;

    xw_reader r;
    if (xw_reader_init(&r, env.payload, env.payload_len, env.descs, env.ndescs) == XW_OK) {
        if (g_ipc_trace) {
            xw_dict_iter it;
            if (xw_dict_iter_init(&r, &it) == XW_OK) {
                const char *k = NULL;
                xw_value val;
                while (xw_dict_next(&it, &k, &val) == 1) {
                    ld_print_str("   [launchd] key: \"");
                    ld_print_str(k ? k : "");
                    ld_print_str("\" type=0x");
                    ld_print_hex(val.type);
                    ld_print_str("\n");
                }
            }
        }

        xw_value v;
        if (xw_dict_find(&r, "name", &v) == XW_OK) {
            const char *s = NULL;
            uint32_t slen = 0;
            if (xw_as_string(&v, &s, &slen) == XW_OK && s) {
                size_t copy_len = slen < sizeof(service_name) ? slen : sizeof(service_name) - 1;
                ld_memcpy(service_name, s, copy_len);
                service_name[copy_len] = '\0';
            }
        }
        if (xw_dict_find(&r, "port", &v) == XW_OK) {
            uint32_t desc_idx = XW_NO_DESC;
            if (xw_as_mach_send(&v, &desc_idx) == XW_OK && desc_idx < env.ndescs) {
                req_port = (mach_port_name_t)env.descs[desc_idx].obj;
            } else if (xw_as_mach_recv(&v, &desc_idx) == XW_OK && desc_idx < env.ndescs) {
                req_port = (mach_port_name_t)env.descs[desc_idx].obj;
            }
        }
    }

    mach_msg_header_t *rep_hdr = (mach_msg_header_t *)reply_buf;
    uint8_t *rep_body = (uint8_t *)(rep_hdr + 1);
    uint32_t max_body = max_reply_size - sizeof(mach_msg_header_t);
    uint32_t body_len = 0;

    if (routine == LAUNCHD_ROUTINE_REGISTER) {
        /* 0xcc bootstrap_register2: store send right in name table, reply error 0 */
        int64_t reply_err = 0;
        struct ipc_port *port_obj = NULL;
        if (req_port != MACH_PORT_NULL) {
            port_obj = ipc_port_from_name(req_port);
        }
        if (!port_obj) {
            reply_err = 17; /* KERN_INVALID_RIGHT */
        } else {
            int found_slot = -1;
            int free_slot = -1;
            for (int i = 0; i < MAX_REGISTERED_SERVICES; i++) {
                if (g_registered_services[i].in_use) {
                    if (ld_strcmp(g_registered_services[i].name, service_name) == 0) {
                        found_slot = i;
                        break;
                    }
                } else if (free_slot == -1) {
                    free_slot = i;
                }
            }
            if (found_slot != -1) {
                g_registered_services[found_slot].port = port_obj;
                ipc_port_retain_send(port_obj);
            } else if (free_slot != -1) {
                size_t nlen = ld_strlen(service_name);
                if (nlen >= sizeof(g_registered_services[free_slot].name))
                    nlen = sizeof(g_registered_services[free_slot].name) - 1;
                ld_memcpy(g_registered_services[free_slot].name, service_name, nlen);
                g_registered_services[free_slot].name[nlen] = '\0';
                g_registered_services[free_slot].port = port_obj;
                g_registered_services[free_slot].in_use = 1;
                ipc_port_retain_send(port_obj);
            } else {
                reply_err = 3; /* KERN_NO_SPACE */
            }
        }

        if (g_ipc_trace) {
            ld_print_str("[launchd] routine=0xcc register name=\"");
            ld_print_str(service_name);
            ld_print_str("\" port=0x");
            ld_print_hex(req_port);
            ld_print_str(" -> error=");
            ld_print_uint((uint64_t)reply_err);
            ld_print_str("\n");
        }

        body_len = xpc_serialize_error_reply(rep_body, max_body, reply_err);
    } else if (routine == LAUNCHD_ROUTINE_CHECKIN) {
        /* 0xce bootstrap_check_in2: reply error 124 for names not pre-declared */
        int64_t reply_err = 124;
        if (g_ipc_trace) {
            ld_print_str("[launchd] routine=0xce check_in name=\"");
            ld_print_str(service_name);
            ld_print_str("\" -> unknown service (error=124)\n");
        }
        body_len = xpc_serialize_error_reply(rep_body, max_body, reply_err);
    } else if (routine == LAUNCHD_ROUTINE_DOMAIN_INFO) {
        /* User sessions domain info (0x388): return empty dict (error=0, single-user system) */
        body_len = xpc_serialize_empty_reply(rep_body, max_body);
        if (g_ipc_trace) {
            ld_print_str("[launchd] routine=0x388 domain_info -> success (single-user)\n");
        }
    } else if (routine == LAUNCHD_ROUTINE_LOOKUP || routine == LAUNCHD_ROUTINE_XPC_LOOKUP) {
        struct ipc_port *found_port = NULL;

        /* 1. Check dynamically registered services */
        for (int i = 0; i < MAX_REGISTERED_SERVICES; i++) {
            if (g_registered_services[i].in_use &&
                ld_strcmp(g_registered_services[i].name, service_name) == 0) {
                found_port = g_registered_services[i].port;
                break;
            }
        }

        /* 2. Check built-in services hook */
        if (!found_port) {
            found_port = ld_lookup_builtin(service_name);
        }

        if (found_port) {
            /* Service found: build complex reply message carrying the send right */
            const uint32_t rep_total = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
                                       sizeof(mach_msg_port_descriptor_t) + 96; /* 136 bytes */
            if (max_reply_size < rep_total) return 0;

            rep_hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
            rep_hdr->msgh_size = rep_total;
            rep_hdr->msgh_remote_port = hdr->msgh_local_port;
            rep_hdr->msgh_local_port = MACH_PORT_NULL;
            rep_hdr->msgh_voucher_port = MACH_PORT_NULL;
            rep_hdr->msgh_id = 0x20000000;

            mach_msg_body_t *body = (mach_msg_body_t *)(rep_hdr + 1);
            body->msgh_descriptor_count = 1;

            uint8_t *desc_bytes = (uint8_t *)(body + 1);
            /* M6: store (slot index, generation), not a raw pointer: the
             * port may be destroyed while the reply is queued. The receive
             * side validates (index, gen) via ipc_desc_load_port. */
            ipc_desc_store_port(desc_bytes, found_port);
            *(uint16_t *)(desc_bytes + 8) = 0;
            desc_bytes[10] = MACH_MSG_TYPE_COPY_SEND;  /* 0x13 */
            desc_bytes[11] = MACH_MSG_PORT_DESCRIPTOR; /* 0 */

            uint8_t *dict_dst = desc_bytes + sizeof(mach_msg_port_descriptor_t);
            uint32_t dict_len = xpc_serialize_lookup_dict(dict_dst, max_reply_size - 40);
            if (dict_len == 0) return 0;

            ipc_port_retain_send(found_port);

            if (g_ipc_trace) {
                ld_print_str("[launchd] routine=0x");
                ld_print_hex(routine);
                ld_print_str(" look_up name=\"");
                ld_print_str(service_name);
                ld_print_str("\" -> found port (complex reply)\n");
            }

            *out_reply_size = rep_total;
            return 1;
        } else {
            /* Service unknown: return 124 (0x7c), which libxpc maps to BOOTSTRAP_UNKNOWN_SERVICE (1102) */
            int64_t reply_error = 124;
            if (g_ipc_trace) {
                ld_print_str("[launchd] routine=0x");
                ld_print_hex(routine);
                ld_print_str(" name=\"");
                ld_print_str(service_name);
                ld_print_str("\" -> unknown service (error=");
                ld_print_uint((uint64_t)reply_error);
                ld_print_str(")\n");
            }
            /* WebKit auxiliary processes (WebContent, Networking, GPU,
             * Model) are launched by the UI process through this lookup
             *. ioswine has no process
             * spawning and no ExtensionKit, so the lookup always fails
             * here and the WKWebView stays white. Say so on fd 2
             * unconditionally: g_ipc_trace is off on device, so without
             * this line the failure is silent. One line per requested
             * service; WebKit retries a failed launch, so the line can
             * repeat. */
            if (ld_starts_with(service_name, "com.apple.WebKit.")) {
                ld_print_str("[webkit] service \"");
                ld_print_str(service_name);
                ld_print_str("\" not launchable: ioswine has no process spawning, WKWebView stays white\n");
            }
            body_len = xpc_serialize_error_reply(rep_body, max_body, reply_error);
        }
    } else {
        /* Unknown routine: reply with generic unknown service error 124 so caller fails cleanly */
        int64_t reply_error = 124;
        if (g_ipc_trace) {
            ld_print_str("[launchd] unknown routine msg id: 0x");
            ld_print_hex(hdr->msgh_id);
            ld_print_str(" routine: 0x");
            ld_print_hex(routine);
            ld_print_str(" name=\"");
            ld_print_str(service_name);
            ld_print_str("\" (error=");
            ld_print_uint((uint64_t)reply_error);
            ld_print_str(")\n");
        }
        body_len = xpc_serialize_error_reply(rep_body, max_body, reply_error);
    }

    if (body_len == 0) return 0;

    rep_hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    rep_hdr->msgh_size = sizeof(mach_msg_header_t) + body_len;
    rep_hdr->msgh_remote_port = hdr->msgh_local_port;
    rep_hdr->msgh_local_port = MACH_PORT_NULL;
    rep_hdr->msgh_voucher_port = MACH_PORT_NULL;
    rep_hdr->msgh_id = 0x20000000; /* libxpc expected reply id */

    *out_reply_size = rep_hdr->msgh_size;
    return 1;
}
