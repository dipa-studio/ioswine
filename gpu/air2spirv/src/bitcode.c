/* bitcode.c - freestanding C reader for the LLVM bitcode container.
 *
 * See bitcode.h for the format and the sources. The reader is a
 * bit-level walker: it yields block-enter, block-end, record and EOF
 * events and decodes abbrev definitions internally. No libc, no
 * allocation; blob operands point into the caller's buffer.
 */
#include "bitcode.h"
#include "a2s_common.h"

/* ------------------------------------------------------------------ */
/* Bit reading. Bits are consumed least significant bit first within
 * each byte, which is the same as low-to-high across the little-endian
 * byte stream. A 64-bit prefetch buffer makes this fast enough for
 * megabyte bitcode blobs without per-bit loops. */

static void bc_refill(bc_reader_t *r)
{
    while (r->bitcnt <= 56 && r->bytepos < r->size) {
        r->bitbuf |= (uint64_t)r->data[r->bytepos++] << r->bitcnt;
        r->bitcnt += 8;
    }
}

static int bc_read_bits(bc_reader_t *r, uint32_t n, uint64_t *out)
{
    uint64_t v;

    if (n > 64)
        return BC_ERR_BAD_OP;
    if (n == 0) {
        *out = 0;
        return BC_OK;
    }
    bc_refill(r);
    if (r->bitcnt < n)
        return BC_ERR_TRUNCATED;
    if (n == 64) {
        v = r->bitbuf;
        r->bitbuf = 0;
    } else {
        v = r->bitbuf & ((1ULL << n) - 1);
        r->bitbuf >>= n;
    }
    r->bitcnt -= n;
    r->bitpos += n;
    *out = v;
    return BC_OK;
}

/* VBR of the given width: chunks of width bits, high bit set means
 * "more chunks follow", low width-1 bits are payload. */
static int bc_read_vbr(bc_reader_t *r, uint32_t width, uint64_t *out)
{
    uint64_t value = 0;
    uint32_t shift = 0;

    if (width < 2 || width > 32)
        return BC_ERR_BAD_VBR;
    for (;;) {
        uint64_t chunk;
        int rc = bc_read_bits(r, width, &chunk);
        if (rc != BC_OK)
            return rc;
        if (shift + width - 1 > 64)
            return BC_ERR_BAD_VBR;
        if ((chunk & (1ULL << (width - 1))) != 0) {
            value |= (chunk & ((1ULL << (width - 1)) - 1)) << shift;
            shift += width - 1;
        } else {
            value |= chunk << shift;
            break;
        }
    }
    *out = value;
    return BC_OK;
}

/* Drop bits until the position is a multiple of 32. */
static int bc_align32(bc_reader_t *r)
{
    uint32_t rem = (uint32_t)(r->bitpos % 32);
    uint64_t tmp;

    if (rem == 0)
        return BC_OK;
    return bc_read_bits(r, 32 - rem, &tmp);
}

/* Forget prefetched bits and re-sync the byte cursor to the current
 * bit position. Needed before direct byte access (blob operands). */
static void bc_sync_bytes(bc_reader_t *r)
{
    r->bitbuf = 0;
    r->bitcnt = 0;
    r->bytepos = (size_t)(r->bitpos / 8);
}

/* ------------------------------------------------------------------ */
/* Abbrev definitions. */

static int bc_decode_abbrev(bc_reader_t *r, bc_abbrev_t *ab)
{
    uint64_t numops;
    uint32_t i;
    int rc = bc_read_vbr(r, 5, &numops);
    if (rc != BC_OK)
        return rc;
    if (numops > BC_MAX_ABBREV_OPS)
        return BC_ERR_TOO_MANY_OPS;
    ab->op_count = (uint8_t)numops;
    for (i = 0; i < numops; i++) {
        uint64_t is_lit;
        bc_abbrev_op_t *op = &ab->ops[i];

        rc = bc_read_bits(r, 1, &is_lit);
        if (rc != BC_OK)
            return rc;
        if (is_lit != 0) {
            uint64_t val;
            rc = bc_read_vbr(r, 8, &val);
            if (rc != BC_OK)
                return rc;
            op->is_literal = 1;
            op->encoding = 0;
            op->value = val;
        } else {
            uint64_t enc;
            rc = bc_read_bits(r, 3, &enc);
            if (rc != BC_OK)
                return rc;
            if (enc < BC_OP_FIXED || enc > BC_OP_BLOB)
                return BC_ERR_BAD_OP;
            op->is_literal = 0;
            op->encoding = (uint8_t)enc;
            if (enc == BC_OP_FIXED || enc == BC_OP_VBR) {
                uint64_t width;
                rc = bc_read_vbr(r, 5, &width);
                if (rc != BC_OK)
                    return rc;
                if (width < 1 || width > 32)
                    return BC_ERR_BAD_OP;
                op->value = width;
            } else {
                op->value = 0;
            }
        }
    }
    return BC_OK;
}

/* Store a freshly decoded abbrev. Inside a BLOCKINFO block it goes to
 * the per-target BLOCKINFO table; otherwise it joins the current
 * block's scope with the next free id. */
static int bc_store_abbrev(bc_reader_t *r, const bc_abbrev_t *ab)
{
    if (r->depth > 0 &&
        r->stack[r->depth - 1].block_id == BC_BLOCKINFO_BLOCK_ID) {
        uint32_t id = BC_FIRST_APPLICATION_ABBREV;
        uint32_t i;
        for (i = 0; i < r->bi_count; i++) {
            if (r->bi_block_id[i] == r->bi_target)
                id++;
        }
        if (r->bi_count >= BC_MAX_BI_ABBREVS)
            return BC_ERR_BAD_ABBREV;
        a2s_memcpy(&r->bi_abbrevs[r->bi_count], ab, sizeof(*ab));
        r->bi_block_id[r->bi_count] = r->bi_target;
        r->bi_abbrev_id[r->bi_count] = id;
        r->bi_count++;
        return BC_OK;
    }
    if (r->depth == 0)
        return BC_ERR_BAD_ABBREV;
    if (r->abbrev_count >= BC_MAX_ABBREVS)
        return BC_ERR_BAD_ABBREV;
    a2s_memcpy(&r->abbrevs[r->abbrev_count], ab, sizeof(*ab));
    r->abbrev_depth[r->abbrev_count] = (uint8_t)r->depth;
    r->abbrev_id[r->abbrev_count] =
        r->stack[r->depth - 1].next_abbrev++;
    r->abbrev_count++;
    return BC_OK;
}

static const bc_abbrev_t *bc_lookup_abbrev(bc_reader_t *r, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < r->abbrev_count; i++) {
        if (r->abbrev_depth[i] == r->depth && r->abbrev_id[i] == id)
            return &r->abbrevs[i];
    }
    return NULL;
}

/* Seed the new block's abbrev scope with the BLOCKINFO abbrevs for
 * its block id, then set the next id to assign. */
static int bc_seed_abbrevs(bc_reader_t *r, uint32_t block_id)
{
    uint32_t i;
    uint32_t next = BC_FIRST_APPLICATION_ABBREV;

    for (i = 0; i < r->bi_count; i++) {
        if (r->bi_block_id[i] != block_id)
            continue;
        if (r->abbrev_count >= BC_MAX_ABBREVS)
            return BC_ERR_BAD_ABBREV;
        a2s_memcpy(&r->abbrevs[r->abbrev_count], &r->bi_abbrevs[i],
                   sizeof(bc_abbrev_t));
        r->abbrev_depth[r->abbrev_count] = (uint8_t)r->depth;
        r->abbrev_id[r->abbrev_count] = r->bi_abbrev_id[i];
        r->abbrev_count++;
        if (r->bi_abbrev_id[i] >= next)
            next = r->bi_abbrev_id[i] + 1;
    }
    r->stack[r->depth - 1].next_abbrev = next;
    return BC_OK;
}

/* ------------------------------------------------------------------ */
/* Record decoding. */

static int bc_add_op(bc_event_t *ev, uint64_t value)
{
    if (ev->op_count >= BC_MAX_OPERANDS)
        return BC_ERR_TOO_MANY_OPS;
    ev->ops[ev->op_count++] = value;
    return BC_OK;
}

/* Decode one scalar operand per an abbrev op (Fixed, VBR, Char6 or
 * literal). Array and Blob are handled by the caller. */
static int bc_decode_scalar(bc_reader_t *r, const bc_abbrev_op_t *op,
                            uint64_t *out)
{
    if (op->is_literal) {
        *out = op->value;
        return BC_OK;
    }
    if (op->encoding == BC_OP_FIXED)
        return bc_read_bits(r, (uint32_t)op->value, out);
    if (op->encoding == BC_OP_VBR)
        return bc_read_vbr(r, (uint32_t)op->value, out);
    if (op->encoding == BC_OP_CHAR6)
        return bc_read_bits(r, 6, out);
    return BC_ERR_BAD_OP;
}

/* Decode an abbreviated record (abbrev id already read). The first
 * field is the record code; the rest are operands. */
static int bc_decode_abbrev_record(bc_reader_t *r, uint32_t abbrev_id,
                                   bc_event_t *ev)
{
    const bc_abbrev_t *ab = bc_lookup_abbrev(r, abbrev_id);
    uint32_t oi;
    int rc;
    int first = 1;

    if (ab == NULL)
        return BC_ERR_BAD_ABBREV;
    ev->abbrev_id = abbrev_id;
    for (oi = 0; oi < ab->op_count; oi++) {
        const bc_abbrev_op_t *op = &ab->ops[oi];
        uint64_t value;

        if (op->is_literal == 0 && op->encoding == BC_OP_ARRAY) {
            const bc_abbrev_op_t *elem;
            uint64_t count;
            uint64_t k;

            /* Array must be followed by its element type, and must be
             * the last operand (apart from that element type). */
            if (oi + 1 >= ab->op_count)
                return BC_ERR_BAD_OP;
            if (oi + 2 != ab->op_count)
                return BC_ERR_BAD_OP;
            elem = &ab->ops[oi + 1];
            if (elem->is_literal == 0 &&
                (elem->encoding == BC_OP_ARRAY ||
                 elem->encoding == BC_OP_BLOB))
                return BC_ERR_BAD_OP;
            rc = bc_read_vbr(r, 6, &count);
            if (rc != BC_OK)
                return rc;
            if (first)
                return BC_ERR_BAD_OP; /* code cannot be an array */
            rc = bc_add_op(ev, count);
            if (rc != BC_OK)
                return rc;
            for (k = 0; k < count; k++) {
                rc = bc_decode_scalar(r, elem, &value);
                if (rc != BC_OK)
                    return rc;
                rc = bc_add_op(ev, value);
                if (rc != BC_OK)
                    return rc;
            }
            oi++; /* consumed the element type op */
            first = 0;
            continue;
        }
        if (op->is_literal == 0 && op->encoding == BC_OP_BLOB) {
            uint64_t len;
            const uint8_t *ptr;

            /* Blob must be the last operand. */
            if (oi + 1 != ab->op_count)
                return BC_ERR_BAD_OP;
            if (first)
                return BC_ERR_BAD_OP; /* code cannot be a blob */
            rc = bc_read_vbr(r, 6, &len);
            if (rc != BC_OK)
                return rc;
            if (len > 0xffffffffu)
                return BC_ERR_BAD_OP;
            rc = bc_align32(r);
            if (rc != BC_OK)
                return rc;
            bc_sync_bytes(r);
            if ((uint64_t)r->bytepos + len > r->size)
                return BC_ERR_TRUNCATED;
            ptr = r->data + r->bytepos;
            r->bitpos += len * 8;
            r->bytepos += (size_t)len;
            rc = bc_align32(r);
            if (rc != BC_OK)
                return rc;
            bc_sync_bytes(r);
            ev->blob = ptr;
            ev->blob_len = (uint32_t)len;
            ev->has_blob = 1;
            rc = bc_add_op(ev, len);
            if (rc != BC_OK)
                return rc;
            first = 0;
            continue;
        }
        rc = bc_decode_scalar(r, op, &value);
        if (rc != BC_OK)
            return rc;
        if (first) {
            ev->code = (uint32_t)value;
            first = 0;
        } else {
            rc = bc_add_op(ev, value);
            if (rc != BC_OK)
                return rc;
        }
    }
    if (first)
        return BC_ERR_BAD_OP; /* abbrev with no fields at all */
    return BC_OK;
}

static int bc_decode_unabbrev_record(bc_reader_t *r, bc_event_t *ev)
{
    uint64_t code;
    uint64_t numops;
    uint64_t i;
    int rc = bc_read_vbr(r, 6, &code);
    if (rc != BC_OK)
        return rc;
    rc = bc_read_vbr(r, 6, &numops);
    if (rc != BC_OK)
        return rc;
    if (numops > BC_MAX_OPERANDS)
        return BC_ERR_TOO_MANY_OPS;
    ev->code = (uint32_t)code;
    ev->abbrev_id = 0;
    for (i = 0; i < numops; i++) {
        uint64_t value;
        rc = bc_read_vbr(r, 6, &value);
        if (rc != BC_OK)
            return rc;
        rc = bc_add_op(ev, value);
        if (rc != BC_OK)
            return rc;
    }
    return BC_OK;
}

/* ------------------------------------------------------------------ */
/* Public API. */

int bc_open(const uint8_t *data, uint32_t size, bc_reader_t *r)
{
    const uint8_t *stream;
    uint32_t stream_size;

    if (data == NULL || r == NULL || size < 4)
        return BC_ERR_TOO_SMALL;
    if (a2s_rd32le(data) == BC_WRAPPER_MAGIC) {
        uint64_t offset;
        uint64_t bsize;
        if (size < BC_WRAPPER_HEADER_SIZE)
            return BC_ERR_TOO_SMALL;
        offset = a2s_rd32le(data + 8);
        bsize = a2s_rd32le(data + 12);
        if (offset > size || bsize > size - (uint32_t)offset)
            return BC_ERR_TRUNCATED;
        stream = data + offset;
        stream_size = (uint32_t)bsize;
    } else if (a2s_rd32le(data) == BC_STREAM_MAGIC) {
        stream = data;
        stream_size = size;
    } else {
        return BC_ERR_BAD_MAGIC;
    }
    if (stream_size < 4)
        return BC_ERR_TOO_SMALL;
    if (a2s_rd32le(stream) != BC_STREAM_MAGIC)
        return BC_ERR_BAD_MAGIC;

    a2s_memset(r, 0, sizeof(*r));
    r->data = stream;
    r->size = stream_size;
    r->bitpos = 32; /* past the 4-byte magic */
    r->bytepos = 4;
    r->bitbuf = 0;
    r->bitcnt = 0;
    r->abbrev_width = 2; /* the stream starts with width 2 */
    r->depth = 0;
    r->abbrev_count = 0;
    r->bi_count = 0;
    r->bi_target = 0;
    r->stream_started = 1;
    return BC_OK;
}

/* True when the bytes from the current bit position to the end of the
 * stream are all zero (trailing padding after the last top-level
 * block). Only consulted at depth 0. */
static int bc_rest_is_zero(bc_reader_t *r)
{
    size_t pos = (size_t)(r->bitpos / 8);
    size_t i;
    for (i = pos; i < r->size; i++) {
        if (r->data[i] != 0)
            return 0;
    }
    return 1;
}

int bc_next(bc_reader_t *r, bc_event_t *ev)
{
    int rc;

    if (r == NULL || ev == NULL)
        return BC_ERR_BAD_BLOCK;
    if (!r->stream_started)
        return BC_ERR_BAD_MAGIC;

    for (;;) {
        uint64_t code64;
        uint32_t code;

        if (r->depth == 0) {
            if (r->bitpos >= (uint64_t)r->size * 8 || bc_rest_is_zero(r)) {
                a2s_memset(ev, 0, sizeof(*ev));
                ev->kind = BC_EV_EOF;
                return BC_OK;
            }
        } else {
            if (r->bitpos >= r->stack[r->depth - 1].end_bitpos)
                return BC_ERR_NO_END_BLOCK;
        }

        rc = bc_read_bits(r, r->abbrev_width, &code64);
        if (rc != BC_OK)
            return rc;
        code = (uint32_t)code64;

        if (code == BC_END_BLOCK) {
            uint32_t end_block_id;
            uint64_t end_bitpos;
            uint32_t saved_width;
            if (r->depth == 0)
                return BC_ERR_BAD_BLOCK;
            end_block_id = r->stack[r->depth - 1].block_id;
            end_bitpos = r->stack[r->depth - 1].end_bitpos;
            saved_width = r->stack[r->depth - 1].saved_abbrev_width;
            rc = bc_align32(r);
            if (rc != BC_OK)
                return rc;
            if (r->bitpos > end_bitpos)
                return BC_ERR_BAD_BLOCK;
            /* Skip any trailing padding inside the block. */
            r->bitpos = end_bitpos;
            bc_sync_bytes(r);
            r->depth--;
            r->abbrev_width = saved_width;
            while (r->abbrev_count > 0 &&
                   r->abbrev_depth[r->abbrev_count - 1] > r->depth)
                r->abbrev_count--;
            a2s_memset(ev, 0, sizeof(*ev));
            ev->kind = BC_EV_BLOCK_END;
            ev->block_id = end_block_id;
            return BC_OK;
        }

        if (code == BC_ENTER_SUBBLOCK) {
            uint64_t blockid;
            uint64_t newwidth;
            uint64_t blocklen;
            bc_block_frame_t *frame;

            rc = bc_read_vbr(r, 8, &blockid);
            if (rc != BC_OK)
                return rc;
            rc = bc_read_vbr(r, 4, &newwidth);
            if (rc != BC_OK)
                return rc;
            if (newwidth < 1 || newwidth > 32)
                return BC_ERR_BAD_BLOCK;
            rc = bc_align32(r);
            if (rc != BC_OK)
                return rc;
            rc = bc_read_bits(r, 32, &blocklen);
            if (rc != BC_OK)
                return rc;
            if (r->depth >= BC_MAX_DEPTH)
                return BC_ERR_TOO_DEEP;
            if (blocklen > ((uint64_t)r->size * 8 - r->bitpos) / 32)
                return BC_ERR_TRUNCATED;
            frame = &r->stack[r->depth];
            frame->block_id = (uint32_t)blockid;
            frame->end_bitpos = r->bitpos + blocklen * 32;
            frame->saved_abbrev_width = r->abbrev_width;
            frame->next_abbrev = BC_FIRST_APPLICATION_ABBREV;
            r->depth++;
            r->abbrev_width = (uint32_t)newwidth;
            rc = bc_seed_abbrevs(r, (uint32_t)blockid);
            if (rc != BC_OK)
                return rc;
            a2s_memset(ev, 0, sizeof(*ev));
            ev->kind = BC_EV_BLOCK_ENTER;
            ev->block_id = (uint32_t)blockid;
            ev->block_len_words = (uint32_t)blocklen;
            return BC_OK;
        }

        if (code == BC_DEFINE_ABBREV) {
            bc_abbrev_t ab;
            rc = bc_decode_abbrev(r, &ab);
            if (rc != BC_OK)
                return rc;
            rc = bc_store_abbrev(r, &ab);
            if (rc != BC_OK)
                return rc;
            continue; /* abbrev definitions are not events */
        }

        a2s_memset(ev, 0, sizeof(*ev));
        ev->kind = BC_EV_RECORD;
        if (code == BC_UNABBREV_RECORD) {
            rc = bc_decode_unabbrev_record(r, ev);
            if (rc != BC_OK)
                return rc;
        } else {
            rc = bc_decode_abbrev_record(r, code, ev);
            if (rc != BC_OK)
                return rc;
        }

        /* BLOCKINFO SETBID selects the target block for the abbrevs
         * that follow in the BLOCKINFO block. */
        if (r->depth > 0 &&
            r->stack[r->depth - 1].block_id == BC_BLOCKINFO_BLOCK_ID &&
            ev->code == BC_BI_SETBID && ev->op_count >= 1)
            r->bi_target = (uint32_t)ev->ops[0];
        return BC_OK;
    }
}

const char *bc_error_string(int err)
{
    if (err == BC_OK)
        return "ok";
    if (err == BC_ERR_TOO_SMALL)
        return "buffer too small for the bitcode header";
    if (err == BC_ERR_BAD_MAGIC)
        return "bad magic, not LLVM bitcode";
    if (err == BC_ERR_TRUNCATED)
        return "bitcode stream is truncated";
    if (err == BC_ERR_BAD_ABBREV)
        return "unknown or misplaced abbrev";
    if (err == BC_ERR_BAD_BLOCK)
        return "bad block header or block end";
    if (err == BC_ERR_TOO_DEEP)
        return "block nesting too deep";
    if (err == BC_ERR_NO_END_BLOCK)
        return "block ended without END_BLOCK";
    if (err == BC_ERR_TOO_MANY_OPS)
        return "record or abbrev has too many operands";
    if (err == BC_ERR_BAD_VBR)
        return "bad VBR encoding";
    if (err == BC_ERR_BAD_OP)
        return "bad abbrev operand";
    return "unknown error";
}
