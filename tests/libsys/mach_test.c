#include <stdint.h>
#include <stddef.h>

typedef uint32_t mach_port_t;
typedef uint32_t mach_port_name_t;
typedef uint32_t mach_port_type_t;
typedef uint32_t mach_msg_bits_t;
typedef uint32_t mach_msg_size_t;
typedef uint32_t mach_msg_id_t;
typedef int32_t  mach_msg_return_t;
typedef int      kern_return_t;
typedef unsigned long pthread_t;

#define MACH_PORT_NULL               ((mach_port_t)0)
#define MACH_PORT_DEAD               ((mach_port_t)~0U)

#define MACH_PORT_RIGHT_SEND         0
#define MACH_PORT_RIGHT_RECEIVE      1
#define MACH_PORT_RIGHT_SEND_ONCE    2
#define MACH_PORT_RIGHT_PORT_SET     3
#define MACH_PORT_RIGHT_DEAD_NAME    4

#define MACH_PORT_TYPE_SEND          0x00010000
#define MACH_PORT_TYPE_RECEIVE       0x00020000
#define MACH_PORT_TYPE_SEND_ONCE     0x00040000
#define MACH_PORT_TYPE_PORT_SET      0x00080000
#define MACH_PORT_TYPE_DEAD_NAME     0x00100000

#define MACH_MSG_TYPE_MOVE_RECEIVE   16
#define MACH_MSG_TYPE_MOVE_SEND      17
#define MACH_MSG_TYPE_MOVE_SEND_ONCE 18
#define MACH_MSG_TYPE_COPY_SEND      19
#define MACH_MSG_TYPE_MAKE_SEND      20
#define MACH_MSG_TYPE_MAKE_SEND_ONCE 21
#define MACH_MSG_TYPE_PORT_RECEIVE   16
#define MACH_MSG_TYPE_PORT_SEND      17
#define MACH_MSG_TYPE_PORT_SEND_ONCE 18

#define MACH_MSGH_BITS_COMPLEX       0x80000000
#define MACH_MSGH_BITS(remote, local) \
    ((mach_msg_bits_t)(remote) | ((mach_msg_bits_t)(local) << 8))

#define MACH_MSG_PORT_DESCRIPTOR     0
#define MACH_MSG_OOL_DESCRIPTOR      1

#define MACH_SEND_MSG                0x00000001
#define MACH_RCV_MSG                 0x00000002
#define MACH_RCV_LARGE               0x00000004
#define MACH_RCV_TIMEOUT             0x00000100
#define MACH_MSG_TIMEOUT_NONE        0

#define KERN_SUCCESS                 0
#define KERN_INVALID_NAME            15
#define MACH_MSG_SUCCESS             0x00000000
#define MACH_SEND_INVALID_DEST       0x10000003
#define MACH_RCV_TIMED_OUT           0x10004003
#define MACH_RCV_TOO_LARGE           0x10004004
#define MACH_RCV_PORT_CHANGED        0x10004006
#define MACH_RCV_PORT_DIED           0x10004009
#define MACH_NOTIFY_NO_SENDERS       0x46
#define MACH_NOTIFY_SEND_ONCE        0x47

typedef struct {
    mach_msg_bits_t       msgh_bits;
    mach_msg_size_t       msgh_size;
    mach_port_t           msgh_remote_port;
    mach_port_t           msgh_local_port;
    mach_port_name_t      msgh_voucher_port;
    mach_msg_id_t         msgh_id;
} mach_msg_header_t;

typedef struct {
    mach_msg_size_t       msgh_descriptor_count;
} mach_msg_body_t;

typedef struct {
    mach_port_t           name;
    mach_msg_size_t       pad1;
    uint16_t              pad2;
    uint8_t               disposition;
    uint8_t               type;
} mach_msg_port_descriptor_t;

typedef struct {
    uint64_t              address;
    uint8_t               deallocate;
    uint8_t               copy;
    uint8_t               pad1;
    uint8_t               type;
    uint32_t              size;
} mach_msg_ool_descriptor_t;

typedef struct {
    uint32_t              msgh_trailer_type;
    uint32_t              msgh_trailer_size;
    uint32_t              msgh_seqno;
    uint32_t              msgh_sender[2];
    uint32_t              msgh_audit[8];
    uint64_t              msgh_context;
    uint64_t              msgh_filter;
} mach_msg_max_trailer_t;

struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};

/* Prototypes from libSystem */
int printf(const char *fmt, ...);
void exit(int status);
int usleep(unsigned int usec);
void *memset(void *s, int c, size_t n);

int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);

extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_

kern_return_t mach_port_allocate(mach_port_t task, int right, mach_port_name_t *name);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_name_t name);
kern_return_t mach_port_destroy(mach_port_t task, mach_port_name_t name);
kern_return_t mach_port_insert_right(mach_port_t task, mach_port_name_t name, mach_port_t poly, int polyPoly);
kern_return_t mach_port_type(mach_port_t task, mach_port_name_t name, int *ptype);
kern_return_t mach_port_get_refs(mach_port_t task, mach_port_name_t name, int right, uint32_t *refs);
kern_return_t mach_port_insert_member(mach_port_t task, mach_port_name_t name, mach_port_name_t pset);
kern_return_t mach_port_extract_member(mach_port_t task, mach_port_name_t name, mach_port_name_t pset);
kern_return_t mach_port_request_notification(mach_port_t task, mach_port_name_t name, int id, uint32_t sync, mach_port_t notify, int poly, mach_port_t *previous);

mach_msg_return_t mach_msg(mach_msg_header_t *msg, int option, uint32_t send_size, uint32_t rcv_size,
                           mach_port_name_t rcv_name, uint32_t timeout, mach_port_name_t notify);

mach_port_t mig_get_reply_port(void);
void mig_dealloc_reply_port(mach_port_t port);

uint64_t mach_absolute_time(void);
kern_return_t mach_timebase_info(struct mach_timebase_info *info);

kern_return_t vm_allocate(mach_port_t task, uint64_t *address, uint64_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, uint64_t address, uint64_t size);

mach_port_name_t mk_timer_create(void);
kern_return_t mk_timer_destroy(mach_port_name_t name);
kern_return_t mk_timer_arm(mach_port_name_t name, uint64_t expire_time);
kern_return_t mk_timer_cancel(mach_port_name_t name, uint64_t *result_time);

/* Test 4 Server */
static void *rpc_server(void *arg) {
    mach_port_t srv_port = (mach_port_t)(uintptr_t)arg;
    struct {
        mach_msg_header_t hdr;
        uint32_t req_val;
        mach_msg_max_trailer_t trailer;
    } rcv;
    struct {
        mach_msg_header_t hdr;
        uint32_t resp_val;
    } snd;

    for (int i = 0; i < 1000; i++) {
        mach_msg_return_t mr = mach_msg(&rcv.hdr, MACH_RCV_MSG, 0, sizeof(rcv), srv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (mr != MACH_MSG_SUCCESS) {
            printf("FAIL 4 server rcv mr=0x%x\n", mr);
            exit(1);
        }
        snd.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        snd.hdr.msgh_size = sizeof(snd);
        snd.hdr.msgh_remote_port = rcv.hdr.msgh_remote_port;
        snd.hdr.msgh_local_port = MACH_PORT_NULL;
        snd.hdr.msgh_voucher_port = MACH_PORT_NULL;
        snd.hdr.msgh_id = rcv.hdr.msgh_id + 100;
        snd.resp_val = rcv.req_val * 2;
        mr = mach_msg(&snd.hdr, MACH_SEND_MSG, sizeof(snd), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (mr != MACH_MSG_SUCCESS) {
            printf("FAIL 4 server snd mr=0x%x\n", mr);
            exit(1);
        }
    }
    return NULL;
}

/* Test 11 Worker */
static volatile int g_t11_blocked = 0;
static volatile mach_msg_return_t g_t11_res = 0;

static void *t11_worker(void *arg) {
    mach_port_t p = (mach_port_t)(uintptr_t)arg;
    char buf[128];
    g_t11_blocked = 1;
    g_t11_res = mach_msg((mach_msg_header_t *)buf, MACH_RCV_MSG, 0, sizeof(buf), p, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    return NULL;
}

int main(void) {
    kern_return_t kr;
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);

    /* 1. allocate receive, insert MAKE_SEND, mach_port_type = RECEIVE|SEND, get_refs send=1 */
    mach_port_t p1 = 0;
    int type = 0;
    uint32_t refs = 0;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p1);
    if (kr != KERN_SUCCESS) { printf("FAIL 1 allocate kr=0x%x\n", kr); return 1; }
    kr = mach_port_insert_right(mach_task_self(), p1, p1, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) { printf("FAIL 1 insert_right kr=0x%x\n", kr); return 1; }
    kr = mach_port_type(mach_task_self(), p1, &type);
    if (kr != KERN_SUCCESS || type != (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND)) {
        printf("FAIL 1 type=0x%x expected=0x%x\n", type, MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND);
        return 1;
    }
    kr = mach_port_get_refs(mach_task_self(), p1, MACH_PORT_RIGHT_SEND, &refs);
    if (kr != KERN_SUCCESS || refs != 1) {
        printf("FAIL 1 get_refs refs=%u expected=1\n", refs);
        return 1;
    }
    printf("ok 1 allocate_and_rights\n");

    /* 2. send a 64-byte simple message to it, receive it: id, body bytes, msgh_size, trailer type/size (seqno trailer) */
    struct {
        mach_msg_header_t hdr;
        char body[40]; /* 24 + 40 = 64 bytes */
        mach_msg_max_trailer_t trailer;
    } msg2;
    memset(&msg2, 0, sizeof(msg2));
    msg2.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg2.hdr.msgh_size = 64;
    msg2.hdr.msgh_remote_port = p1;
    msg2.hdr.msgh_local_port = MACH_PORT_NULL;
    msg2.hdr.msgh_id = 0x1234;
    for (int i = 0; i < 40; i++) msg2.body[i] = (char)(i + 0x20);

    kr = mach_msg(&msg2.hdr, MACH_SEND_MSG, 64, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 2 send kr=0x%x\n", kr); return 1; }

    memset(&msg2, 0, sizeof(msg2));
    kr = mach_msg(&msg2.hdr, MACH_RCV_MSG | (1 << 24), 0, sizeof(msg2), p1, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 2 receive kr=0x%x\n", kr); return 1; }
    if (msg2.hdr.msgh_id != 0x1234) { printf("FAIL 2 id=0x%x\n", msg2.hdr.msgh_id); return 1; }
    if (msg2.hdr.msgh_size != 64) { printf("FAIL 2 size=%u\n", msg2.hdr.msgh_size); return 1; }
    for (int i = 0; i < 40; i++) {
        if (msg2.body[i] != (char)(i + 0x20)) { printf("FAIL 2 body mismatch at %d\n", i); return 1; }
    }
    uint32_t *t = (uint32_t *)((uint8_t *)&msg2.hdr + 64);
    if (t[0] != 0 || t[1] != 12 || t[2] != 1) {
        printf("FAIL 2 trailer type=%u size=%u seqno=%u\n", t[0], t[1], t[2]);
        return 1;
    }
    printf("ok 2 send_receive_simple\n");

    /* 3. receive with timeout 50 ms on an empty port → MACH_RCV_TIMED_OUT, elapsed 40..500 ms */
    mach_port_t empty_port = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &empty_port);
    uint64_t t0 = mach_absolute_time();
    char dummy_buf[128];
    kr = mach_msg((mach_msg_header_t *)dummy_buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(dummy_buf), empty_port, 50, MACH_PORT_NULL);
    uint64_t t1 = mach_absolute_time();
    uint64_t elapsed_ms = ((t1 - t0) * tb.numer) / (tb.denom * 1000000ULL);
    if (kr != MACH_RCV_TIMED_OUT) { printf("FAIL 3 kr=0x%x expected TIMED_OUT\n", kr); return 1; }
    if (elapsed_ms < 40 || elapsed_ms > 500) { printf("FAIL 3 elapsed=%llu ms\n", (unsigned long long)elapsed_ms); return 1; }
    mach_port_destroy(mach_task_self(), empty_port);
    printf("ok 3 receive_timeout\n");

    /* 4. MIG-style RPC between two threads (pthread_create): 1000 round trips */
    mach_port_t srv_port = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &srv_port);
    mach_port_insert_right(mach_task_self(), srv_port, srv_port, MACH_MSG_TYPE_MAKE_SEND);
    pthread_t th;
    pthread_create(&th, NULL, rpc_server, (void *)(uintptr_t)srv_port);

    mach_port_t reply_port = mig_get_reply_port();
    struct {
        mach_msg_header_t hdr;
        uint32_t val;
        mach_msg_max_trailer_t trailer;
    } rpc_msg;

    for (int i = 0; i < 1000; i++) {
        rpc_msg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
        rpc_msg.hdr.msgh_size = sizeof(mach_msg_header_t) + sizeof(uint32_t);
        rpc_msg.hdr.msgh_remote_port = srv_port;
        rpc_msg.hdr.msgh_local_port = reply_port;
        rpc_msg.hdr.msgh_voucher_port = MACH_PORT_NULL;
        rpc_msg.hdr.msgh_id = 1000 + i;
        rpc_msg.val = i;

        kr = mach_msg(&rpc_msg.hdr, MACH_SEND_MSG | MACH_RCV_MSG,
                      rpc_msg.hdr.msgh_size, sizeof(rpc_msg), reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != MACH_MSG_SUCCESS) {
            printf("FAIL 4 rpc roundtrip %d kr=0x%x\n", i, kr);
            return 1;
        }
        if (rpc_msg.hdr.msgh_id != (uint32_t)(1000 + i + 100) || rpc_msg.val != (uint32_t)(i * 2)) {
            printf("FAIL 4 rpc reply mismatch at %d: id=%u val=%u\n", i, rpc_msg.hdr.msgh_id, rpc_msg.val);
            return 1;
        }
    }
    pthread_join(th, NULL);
    mach_port_destroy(mach_task_self(), srv_port);
    printf("ok 4 mig_rpc_threads\n");

    /* 5. port set: two ports in a set, send to the second, receive on the set returns it with msgh_local_port = that port */
    mach_port_t pset = 0, ps1 = 0, ps2 = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &pset);
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &ps1);
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &ps2);
    mach_port_insert_right(mach_task_self(), ps1, ps1, MACH_MSG_TYPE_MAKE_SEND);
    mach_port_insert_right(mach_task_self(), ps2, ps2, MACH_MSG_TYPE_MAKE_SEND);
    mach_port_insert_member(mach_task_self(), ps1, pset);
    mach_port_insert_member(mach_task_self(), ps2, pset);

    struct {
        mach_msg_header_t hdr;
        char pad[32];
        mach_msg_max_trailer_t trailer;
    } pset_msg;
    memset(&pset_msg, 0, sizeof(pset_msg));
    pset_msg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    pset_msg.hdr.msgh_size = sizeof(mach_msg_header_t) + 32;
    pset_msg.hdr.msgh_remote_port = ps2;
    pset_msg.hdr.msgh_local_port = MACH_PORT_NULL;
    pset_msg.hdr.msgh_id = 555;

    kr = mach_msg(&pset_msg.hdr, MACH_SEND_MSG, pset_msg.hdr.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 5 send kr=0x%x\n", kr); return 1; }

    memset(&pset_msg, 0, sizeof(pset_msg));
    kr = mach_msg(&pset_msg.hdr, MACH_RCV_MSG, 0, sizeof(pset_msg), pset, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 5 rcv kr=0x%x\n", kr); return 1; }
    if (pset_msg.hdr.msgh_local_port != ps2) {
        printf("FAIL 5 local_port=0x%x expected=0x%x\n", pset_msg.hdr.msgh_local_port, ps2);
        return 1;
    }
    mach_port_destroy(mach_task_self(), pset);
    mach_port_destroy(mach_task_self(), ps1);
    mach_port_destroy(mach_task_self(), ps2);
    printf("ok 5 port_set\n");

    /* 6. complex message: one port desc + one OOL desc of 100000 bytes */
    mach_port_t rcv_port = 0, third_port = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rcv_port);
    mach_port_insert_right(mach_task_self(), rcv_port, rcv_port, MACH_MSG_TYPE_MAKE_SEND);
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &third_port);

    uint64_t ool_addr = 0;
    vm_allocate(mach_task_self(), &ool_addr, 100000, 1 /* VM_FLAGS_ANYWHERE */);
    uint8_t *ool_buf = (uint8_t *)ool_addr;
    for (int i = 0; i < 100000; i++) ool_buf[i] = (uint8_t)(i ^ 0x5a);

    struct {
        mach_msg_header_t hdr;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t port_desc;
        mach_msg_ool_descriptor_t ool_desc;
        mach_msg_max_trailer_t trailer;
    } cmsg;
    memset(&cmsg, 0, sizeof(cmsg));
    cmsg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    cmsg.hdr.msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) + sizeof(mach_msg_port_descriptor_t) + sizeof(mach_msg_ool_descriptor_t);
    cmsg.hdr.msgh_remote_port = rcv_port;
    cmsg.hdr.msgh_local_port = MACH_PORT_NULL;
    cmsg.hdr.msgh_id = 666;
    cmsg.body.msgh_descriptor_count = 2;

    cmsg.port_desc.name = third_port;
    cmsg.port_desc.pad1 = 0;
    cmsg.port_desc.pad2 = 0;
    cmsg.port_desc.disposition = MACH_MSG_TYPE_MAKE_SEND;
    cmsg.port_desc.type = MACH_MSG_PORT_DESCRIPTOR;

    cmsg.ool_desc.address = ool_addr;
    cmsg.ool_desc.deallocate = 1;
    cmsg.ool_desc.copy = 0;
    cmsg.ool_desc.pad1 = 0;
    cmsg.ool_desc.type = MACH_MSG_OOL_DESCRIPTOR;
    cmsg.ool_desc.size = 100000;

    kr = mach_msg(&cmsg.hdr, MACH_SEND_MSG, cmsg.hdr.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 6 send kr=0x%x\n", kr); return 1; }

    memset(&cmsg, 0, sizeof(cmsg));
    kr = mach_msg(&cmsg.hdr, MACH_RCV_MSG, 0, sizeof(cmsg), rcv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 6 rcv kr=0x%x\n", kr); return 1; }
    if (cmsg.body.msgh_descriptor_count != 2) { printf("FAIL 6 desc_count=%u\n", cmsg.body.msgh_descriptor_count); return 1; }
    if (cmsg.port_desc.type != MACH_MSG_PORT_DESCRIPTOR || cmsg.port_desc.disposition != MACH_MSG_TYPE_PORT_SEND) {
        printf("FAIL 6 port desc type=%u disp=%u\n", cmsg.port_desc.type, cmsg.port_desc.disposition);
        return 1;
    }
    if (cmsg.ool_desc.type != MACH_MSG_OOL_DESCRIPTOR || cmsg.ool_desc.size != 100000) {
        printf("FAIL 6 ool desc type=%u size=%u\n", cmsg.ool_desc.type, cmsg.ool_desc.size);
        return 1;
    }
    if ((cmsg.ool_desc.address & 0x3fff) != 0) {
        printf("FAIL 6 ool addr not 16K aligned: 0x%llx\n", (unsigned long long)cmsg.ool_desc.address);
        return 1;
    }
    if (cmsg.ool_desc.address == ool_addr) {
        printf("FAIL 6 ool addr same as sent addr: 0x%llx\n", (unsigned long long)cmsg.ool_desc.address);
        return 1;
    }
    uint8_t *rcv_ool = (uint8_t *)cmsg.ool_desc.address;
    for (int i = 0; i < 100000; i++) {
        if (rcv_ool[i] != (uint8_t)(i ^ 0x5a)) {
            printf("FAIL 6 pattern mismatch at %d\n", i);
            return 1;
        }
    }
    vm_deallocate(mach_task_self(), cmsg.ool_desc.address, 100000);
    mach_port_destroy(mach_task_self(), rcv_port);
    mach_port_destroy(mach_task_self(), third_port);
    printf("ok 6 complex_msg_ool\n");

    /* 7. MOVE_RECEIVE in a descriptor */
    mach_port_t move_port = 0, dest_port = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &move_port);
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &dest_port);
    mach_port_insert_right(mach_task_self(), dest_port, dest_port, MACH_MSG_TYPE_MAKE_SEND);

    struct {
        mach_msg_header_t hdr;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t port_desc;
        mach_msg_max_trailer_t trailer;
    } mmsg;
    memset(&mmsg, 0, sizeof(mmsg));
    mmsg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    mmsg.hdr.msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) + sizeof(mach_msg_port_descriptor_t);
    mmsg.hdr.msgh_remote_port = dest_port;
    mmsg.hdr.msgh_local_port = MACH_PORT_NULL;
    mmsg.hdr.msgh_id = 777;
    mmsg.body.msgh_descriptor_count = 1;

    mmsg.port_desc.name = move_port;
    mmsg.port_desc.pad1 = 0;
    mmsg.port_desc.pad2 = 0;
    mmsg.port_desc.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
    mmsg.port_desc.type = MACH_MSG_PORT_DESCRIPTOR;

    kr = mach_msg(&mmsg.hdr, MACH_SEND_MSG, mmsg.hdr.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 7 send kr=0x%x\n", kr); return 1; }

    int old_type = 0;
    kr = mach_port_type(mach_task_self(), move_port, &old_type);
    if (kr != KERN_INVALID_NAME) {
        printf("FAIL 7 old name still valid: kr=0x%x type=0x%x\n", kr, old_type);
        return 1;
    }

    memset(&mmsg, 0, sizeof(mmsg));
    kr = mach_msg(&mmsg.hdr, MACH_RCV_MSG, 0, sizeof(mmsg), dest_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 7 rcv kr=0x%x\n", kr); return 1; }
    if (mmsg.port_desc.disposition != MACH_MSG_TYPE_PORT_RECEIVE) {
        printf("FAIL 7 received disp=%u\n", mmsg.port_desc.disposition);
        return 1;
    }
    mach_port_t new_name = mmsg.port_desc.name;
    int new_type = 0;
    kr = mach_port_type(mach_task_self(), new_name, &new_type);
    if (kr != KERN_SUCCESS || !(new_type & MACH_PORT_TYPE_RECEIVE)) {
        printf("FAIL 7 new name not receive right: kr=0x%x type=0x%x\n", kr, new_type);
        return 1;
    }
    mach_port_destroy(mach_task_self(), dest_port);
    mach_port_destroy(mach_task_self(), new_name);
    printf("ok 7 move_receive\n");

    /* 8. MACH_RCV_LARGE with a 32-byte buffer → TOO_LARGE + needed size, then receive succeeds */
    mach_port_t lrg_port = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &lrg_port);
    mach_port_insert_right(mach_task_self(), lrg_port, lrg_port, MACH_MSG_TYPE_MAKE_SEND);

    struct {
        mach_msg_header_t hdr;
        char payload[40];
    } send_lrg;
    send_lrg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    send_lrg.hdr.msgh_size = sizeof(send_lrg);
    send_lrg.hdr.msgh_remote_port = lrg_port;
    send_lrg.hdr.msgh_local_port = MACH_PORT_NULL;
    send_lrg.hdr.msgh_id = 888;
    memset(send_lrg.payload, 'X', 40);

    kr = mach_msg(&send_lrg.hdr, MACH_SEND_MSG, sizeof(send_lrg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 8 send kr=0x%x\n", kr); return 1; }

    char small_buf[32];
    memset(small_buf, 0, sizeof(small_buf));
    kr = mach_msg((mach_msg_header_t *)small_buf, MACH_RCV_MSG | MACH_RCV_LARGE, 0, 32, lrg_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_RCV_TOO_LARGE) {
        printf("FAIL 8 rcv small kr=0x%x expected TOO_LARGE\n", kr);
        return 1;
    }
    mach_msg_header_t *sh = (mach_msg_header_t *)small_buf;
    uint32_t needed = sh->msgh_size;
    if (needed < 64 + 8) {
        printf("FAIL 8 needed size=%u too small\n", needed);
        return 1;
    }

    char big_buf[256];
    memset(big_buf, 0, sizeof(big_buf));
    kr = mach_msg((mach_msg_header_t *)big_buf, MACH_RCV_MSG, 0, sizeof(big_buf), lrg_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 8 rcv big kr=0x%x\n", kr);
        return 1;
    }
    mach_msg_header_t *bh = (mach_msg_header_t *)big_buf;
    if (bh->msgh_id != 888 || bh->msgh_size != 64) {
        printf("FAIL 8 big buf id=%u size=%u\n", bh->msgh_id, bh->msgh_size);
        return 1;
    }
    mach_port_destroy(mach_task_self(), lrg_port);
    printf("ok 8 rcv_large\n");

    /* 9. mk_timer */
    mach_port_t timer_port = mk_timer_create();
    if (timer_port == MACH_PORT_NULL) { printf("FAIL 9 create\n"); return 1; }

    uint64_t now_ticks = mach_absolute_time();
    uint64_t ticks_30ms = (30ULL * 1000000ULL * tb.denom) / tb.numer;
    kr = mk_timer_arm(timer_port, now_ticks + ticks_30ms);
    if (kr != KERN_SUCCESS) { printf("FAIL 9 arm kr=0x%x\n", kr); return 1; }

    uint64_t t_start = mach_absolute_time();
    struct {
        mach_msg_header_t hdr;
        char pad[32];
        mach_msg_max_trailer_t trailer;
    } tmsg;
    memset(&tmsg, 0, sizeof(tmsg));
    kr = mach_msg(&tmsg.hdr, MACH_RCV_MSG, 0, sizeof(tmsg), timer_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    uint64_t t_end = mach_absolute_time();
    uint64_t timer_elapsed = ((t_end - t_start) * tb.numer) / (tb.denom * 1000000ULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 9 receive kr=0x%x\n", kr); return 1; }
    if (timer_elapsed < 20 || timer_elapsed > 300) { printf("FAIL 9 elapsed=%llu ms\n", (unsigned long long)timer_elapsed); return 1; }

    /* Re-arm for 1000 ms, then cancel */
    now_ticks = mach_absolute_time();
    uint64_t ticks_1000ms = (1000ULL * 1000000ULL * tb.denom) / tb.numer;
    mk_timer_arm(timer_port, now_ticks + ticks_1000ms);
    uint64_t cancel_res = 0;
    kr = mk_timer_cancel(timer_port, &cancel_res);
    if (kr != KERN_SUCCESS) { printf("FAIL 9 cancel kr=0x%x\n", kr); return 1; }

    memset(&tmsg, 0, sizeof(tmsg));
    kr = mach_msg(&tmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(tmsg), timer_port, 60, MACH_PORT_NULL);
    if (kr != MACH_RCV_TIMED_OUT) {
        printf("FAIL 9 message arrived after cancel kr=0x%x\n", kr);
        return 1;
    }
    mk_timer_destroy(timer_port);
    printf("ok 9 mk_timer\n");

    /* 10. no-senders notification */
    mach_port_t np = 0, nport = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &np);
    mach_port_insert_right(mach_task_self(), np, np, MACH_MSG_TYPE_MAKE_SEND);

    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &nport);
    mach_port_t prev_notify = 0;
    kr = mach_port_request_notification(mach_task_self(), np, MACH_NOTIFY_NO_SENDERS, 1, nport, MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev_notify);
    if (kr != KERN_SUCCESS) { printf("FAIL 10 request_notif kr=0x%x\n", kr); return 1; }

    kr = mach_port_deallocate(mach_task_self(), np);
    if (kr != KERN_SUCCESS) { printf("FAIL 10 deallocate kr=0x%x\n", kr); return 1; }

    struct {
        mach_msg_header_t hdr;
        char pad[32];
        mach_msg_max_trailer_t trailer;
    } notif_msg;
    memset(&notif_msg, 0, sizeof(notif_msg));
    kr = mach_msg(&notif_msg.hdr, MACH_RCV_MSG, 0, sizeof(notif_msg), nport, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) { printf("FAIL 10 rcv kr=0x%x\n", kr); return 1; }
    if (notif_msg.hdr.msgh_id != MACH_NOTIFY_NO_SENDERS) {
        printf("FAIL 10 id=0x%x expected 0x46\n", notif_msg.hdr.msgh_id);
        return 1;
    }
    mach_port_destroy(mach_task_self(), np);
    mach_port_destroy(mach_task_self(), nport);
    printf("ok 10 no_senders\n");

    /* 11. destroying a receive right while another thread blocks in receive on it */
    mach_port_t dport = 0;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &dport);
    mach_port_insert_right(mach_task_self(), dport, dport, MACH_MSG_TYPE_MAKE_SEND);
    pthread_t th11;
    g_t11_blocked = 0;
    pthread_create(&th11, NULL, t11_worker, (void *)(uintptr_t)dport);

    while (!g_t11_blocked) usleep(1000);
    usleep(50000); /* wait 50ms to ensure worker is in mach_msg receive */

    mach_port_destroy(mach_task_self(), dport);
    pthread_join(th11, NULL);

    if (g_t11_res != MACH_RCV_PORT_DIED && g_t11_res != MACH_RCV_PORT_CHANGED) {
        printf("FAIL 11 worker res=0x%x expected PORT_DIED(0x10004009)\n", g_t11_res);
        return 1;
    }

    /* Sending to dead name */
    mach_msg_header_t shdr;
    shdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    shdr.msgh_size = sizeof(shdr);
    shdr.msgh_remote_port = dport;
    shdr.msgh_local_port = MACH_PORT_NULL;
    shdr.msgh_voucher_port = MACH_PORT_NULL;
    shdr.msgh_id = 999;
    kr = mach_msg(&shdr, MACH_SEND_MSG, sizeof(shdr), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_SEND_INVALID_DEST) {
        printf("FAIL 11 send to dead port kr=0x%x expected INVALID_DEST(0x10000003)\n", kr);
        return 1;
    }
    printf("ok 11 destroy_while_receiving\n");

    /* 12. Send-once notification when message cannot be delivered (dead port / no receiver) */
    mach_port_t rport = 0;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rport);
    if (kr != KERN_SUCCESS) { printf("FAIL 12 allocate rport kr=0x%x\n", kr); return 1; }

    /* Case A: Send to dead port (dport) with MAKE_SEND_ONCE reply port */
    mach_msg_header_t smsg;
    smsg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    smsg.msgh_size = sizeof(smsg);
    smsg.msgh_remote_port = dport;
    smsg.msgh_local_port = rport;
    smsg.msgh_voucher_port = MACH_PORT_NULL;
    smsg.msgh_id = 1234;
    kr = mach_msg(&smsg, MACH_SEND_MSG, sizeof(smsg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_SEND_INVALID_DEST) {
        printf("FAIL 12 send to dead port kr=0x%x expected INVALID_DEST\n", kr);
        return 1;
    }

    struct {
        mach_msg_header_t hdr;
        mach_msg_max_trailer_t trailer;
    } rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    kr = mach_msg(&rmsg.hdr, MACH_RCV_MSG, 0, sizeof(rmsg), rport, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 12 rcv dead port notif kr=0x%x\n", kr);
        return 1;
    }
    if (rmsg.hdr.msgh_id != MACH_NOTIFY_SEND_ONCE) {
        printf("FAIL 12 notif id=0x%x expected 0x47\n", rmsg.hdr.msgh_id);
        return 1;
    }

    /* Case B: Send to non-existent port 0xdead03 (no receiver) with MAKE_SEND_ONCE reply port */
    smsg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    smsg.msgh_size = sizeof(smsg);
    smsg.msgh_remote_port = 0xdead03;
    smsg.msgh_local_port = rport;
    smsg.msgh_voucher_port = MACH_PORT_NULL;
    smsg.msgh_id = 0x400000cf;
    kr = mach_msg(&smsg, MACH_SEND_MSG, sizeof(smsg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_SEND_INVALID_DEST) {
        printf("FAIL 12 send to 0xdead03 kr=0x%x expected INVALID_DEST\n", kr);
        return 1;
    }

    memset(&rmsg, 0, sizeof(rmsg));
    kr = mach_msg(&rmsg.hdr, MACH_RCV_MSG, 0, sizeof(rmsg), rport, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 12 rcv 0xdead03 notif kr=0x%x\n", kr);
        return 1;
    }
    if (rmsg.hdr.msgh_id != MACH_NOTIFY_SEND_ONCE) {
        printf("FAIL 12 0xdead03 notif id=0x%x expected 0x47\n", rmsg.hdr.msgh_id);
        return 1;
    }

    mach_port_destroy(mach_task_self(), rport);

    /* (Case C removed: on XNU a receive on an idle reply port with no senders blocks; it does not return
     * MACH_RCV_PORT_DIED. The old emulation did, which stranded libdispatch sync waiters.) */

    printf("ok 12 send_once_notification\n");

    /* 13. bootstrap_look_up: launchd shim returns BOOTSTRAP_UNKNOWN_SERVICE (1102) */
    extern mach_port_t bootstrap_port;
    extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
    mach_port_t sp = MACH_PORT_NULL;
    kr = bootstrap_look_up(bootstrap_port, "com.apple.ioswine.no-such-service", &sp);
    if (kr != 1102) {
        printf("FAIL 13 bootstrap_look_up kr=%d expected 1102\n", kr);
        return 1;
    }
    if (sp != MACH_PORT_NULL) {
        printf("FAIL 13 bootstrap_look_up sp=0x%x expected NULL\n", sp);
        return 1;
    }
    printf("ok 13 bootstrap_look_up\n");

    /* 14. bootstrap_check_in2: unknown name returns 124 */
    extern kern_return_t bootstrap_check_in2(mach_port_t bp, const char *name, mach_port_t *port, uint64_t flags);
    mach_port_t cin_port = MACH_PORT_NULL;
    kr = bootstrap_check_in2(bootstrap_port, "com.example.nonexistent.service", &cin_port, 0);
    if (kr != 124) {
        printf("FAIL 14 bootstrap_check_in2 kr=%d expected 124\n", kr);
        return 1;
    }
    printf("ok 14 bootstrap_check_in2 unknown -> 124\n");

    /* 15. bootstrap_register2 + bootstrap_look_up round trip */
    extern kern_return_t bootstrap_register2(mach_port_t bp, const char *name, mach_port_t port, uint64_t flags);
    mach_port_t reg_recv = MACH_PORT_NULL;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reg_recv);
    if (kr != KERN_SUCCESS) {
        printf("FAIL 15 allocate reg_recv kr=%d\n", kr);
        return 1;
    }
    kr = mach_port_insert_right(mach_task_self(), reg_recv, reg_recv, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("FAIL 15 insert_right MAKE_SEND kr=%d\n", kr);
        return 1;
    }

    kr = bootstrap_register2(bootstrap_port, "org.ioswine.test.service", reg_recv, 0);
    if (kr != KERN_SUCCESS) {
        printf("FAIL 15 bootstrap_register2 kr=%d\n", kr);
        return 1;
    }

    mach_port_t looked_up = MACH_PORT_NULL;
    kr = bootstrap_look_up(bootstrap_port, "org.ioswine.test.service", &looked_up);
    if (kr != KERN_SUCCESS) {
        printf("FAIL 15 bootstrap_look_up registered kr=%d\n", kr);
        return 1;
    }
    if (looked_up == MACH_PORT_NULL) {
        printf("FAIL 15 looked_up port is NULL\n");
        return 1;
    }

    /* Send message through looked_up port and receive on reg_recv */
    struct {
        mach_msg_header_t hdr;
        uint32_t val;
    } rt_send;
    rt_send.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    rt_send.hdr.msgh_size = sizeof(rt_send);
    rt_send.hdr.msgh_remote_port = looked_up;
    rt_send.hdr.msgh_local_port = MACH_PORT_NULL;
    rt_send.hdr.msgh_voucher_port = MACH_PORT_NULL;
    rt_send.hdr.msgh_id = 0x5555;
    rt_send.val = 0x12345678;

    kr = mach_msg(&rt_send.hdr, MACH_SEND_MSG, sizeof(rt_send), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 15 send through looked_up kr=0x%x\n", kr);
        return 1;
    }

    struct {
        mach_msg_header_t hdr;
        uint32_t val;
        mach_msg_max_trailer_t trailer;
    } rt_rcv;
    memset(&rt_rcv, 0, sizeof(rt_rcv));
    kr = mach_msg(&rt_rcv.hdr, MACH_RCV_MSG, 0, sizeof(rt_rcv), reg_recv, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 15 rcv on reg_recv kr=0x%x\n", kr);
        return 1;
    }
    if (rt_rcv.hdr.msgh_id != 0x5555 || rt_rcv.val != 0x12345678) {
        printf("FAIL 15 rcv data mismatch id=0x%x val=0x%x\n", rt_rcv.hdr.msgh_id, rt_rcv.val);
        return 1;
    }

    mach_port_deallocate(mach_task_self(), looked_up);
    mach_port_destroy(mach_task_self(), reg_recv);
    printf("ok 15 register2 + look_up round trip\n");

    /* 16. Built-in com.apple.backboard.display.services MIG RPC */
    mach_port_t bks_port = MACH_PORT_NULL;
    kr = bootstrap_look_up(bootstrap_port, "com.apple.backboard.display.services", &bks_port);
    if (kr != KERN_SUCCESS) {
        printf("FAIL 16 bootstrap_look_up bks kr=%d\n", kr);
        return 1;
    }
    if (bks_port == MACH_PORT_NULL) {
        printf("FAIL 16 bks_port is NULL\n");
        return 1;
    }

    mach_port_t bks_reply_port = mig_get_reply_port();

    typedef struct {
        mach_msg_header_t Head;
        uint8_t           NDR[8];
        kern_return_t     RetCode;
        float             width;
        float             height;
        float             scale;
        float             rotation;
        uint32_t          flagE;
        uint32_t          flagF;
        mach_msg_max_trailer_t trailer;
    } bks_rep_msg_t;

    bks_rep_msg_t bks_rep;
    memset(&bks_rep, 0, sizeof(bks_rep));

    bks_rep.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    bks_rep.Head.msgh_size = sizeof(mach_msg_header_t);
    bks_rep.Head.msgh_remote_port = bks_port;
    bks_rep.Head.msgh_local_port = bks_reply_port;
    bks_rep.Head.msgh_voucher_port = MACH_PORT_NULL;
    bks_rep.Head.msgh_id = 6001012;

    kr = mach_msg(&bks_rep.Head, MACH_SEND_MSG | MACH_RCV_MSG,
                  sizeof(mach_msg_header_t), sizeof(bks_rep), bks_reply_port,
                  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 16 mach_msg MIG 6001012 kr=0x%x\n", kr);
        return 1;
    }

    if (bks_rep.Head.msgh_id != 6001112) {
        printf("FAIL 16 bks reply id=%d expected 6001112\n", bks_rep.Head.msgh_id);
        return 1;
    }
    if (bks_rep.Head.msgh_size != 60) {
        printf("FAIL 16 bks reply size=%d expected 60\n", bks_rep.Head.msgh_size);
        return 1;
    }
    if (bks_rep.RetCode != 0) {
        printf("FAIL 16 bks reply RetCode=%d expected 0\n", bks_rep.RetCode);
        return 1;
    }
    if (bks_rep.width <= 0.0f || bks_rep.height <= 0.0f || bks_rep.scale <= 0.0f) {
        printf("FAIL 16 bks reply dimensions invalid\n");
        return 1;
    }

    /* Test unknown MIG id on display services port */
    memset(&bks_rep, 0, sizeof(bks_rep));
    bks_rep.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    bks_rep.Head.msgh_size = sizeof(mach_msg_header_t);
    bks_rep.Head.msgh_remote_port = bks_port;
    bks_rep.Head.msgh_local_port = bks_reply_port;
    bks_rep.Head.msgh_voucher_port = MACH_PORT_NULL;
    bks_rep.Head.msgh_id = 6001999;
    kr = mach_msg(&bks_rep.Head, MACH_SEND_MSG | MACH_RCV_MSG,
                  sizeof(mach_msg_header_t), sizeof(bks_rep), bks_reply_port,
                  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("FAIL 16 unknown MIG mach_msg kr=0x%x\n", kr);
        return 1;
    }
    if (bks_rep.Head.msgh_id != 6002099) {
        printf("FAIL 16 unknown MIG id=%d expected 6002099\n", bks_rep.Head.msgh_id);
        return 1;
    }
    if (bks_rep.Head.msgh_size != 36) {
        printf("FAIL 16 unknown MIG size=%d expected 36\n", bks_rep.Head.msgh_size);
        return 1;
    }
    if (bks_rep.RetCode != -301) { /* MIG_BAD_ID */
        printf("FAIL 16 unknown MIG RetCode=%d expected -301\n", bks_rep.RetCode);
        return 1;
    }

    mach_port_deallocate(mach_task_self(), bks_port);
    mig_dealloc_reply_port(bks_reply_port);
    printf("ok 16 backboardd display service MIG 6001012 + MIG_BAD_ID\n");

    printf("mach ok\n");
    return 0;
}
