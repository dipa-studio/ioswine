/* bitcode.h - freestanding C reader for the LLVM bitcode container.
 *
 * Grounded in the official LLVM BitCodeFormat doc (llvm.org/docs/
 * BitCodeFormat.html), cross-checked against the current LLVM headers
 * (llvm/include/llvm/Bitcode/LLVMBitCodes.h and
 * llvm/include/llvm/Bitstream/BitCodeEnums.h). Where the doc and the
 * header disagree, the header wins; the disagreements are noted below.
 *
 *   Wrapper: a .air file or a metallib embedded blob starts with the
 *   wrapper magic 0x0b17c0de (bytes DE C0 17 0B), followed by four more
 *   little-endian u32 fields: version (always 0), offset of the
 *   bitcode stream, size of the bitcode stream, CPU type. The stream
 *   itself starts with the magic bytes 42 43 C0 DE ("BC\xc0\xde").
 *   A raw stream without a wrapper is also accepted.
 *
 *   Bitstream: bits are read least significant bit first within each
 *   byte. A VBR of width N emits values in N-1 bit chunks, all but the
 *   last chunk with the high bit set. Char6 maps 0-25 a-z, 26-51 A-Z,
 *   52-61 0-9, 62 '.', 63 '_'. Records are word aligned after
 *   ENTER_SUBBLOCK, END_BLOCK and Blob operands.
 *
 *   Record codes (builtin abbrev IDs): END_BLOCK 0, ENTER_SUBBLOCK 1,
 *   DEFINE_ABBREV 2, UNABBREV_RECORD 3, first user abbrev 4.
 *   ENTER_SUBBLOCK is [code, blockid vbr8, newabbrevlen vbr4,
 *   align-to-32, blocklen u32]. The abbrev ID width starts at 2 and is
 *   set by newabbrevlen on each block entry. blocklen is the subblock
 *   size in 32-bit words, counted from after the blocklen field.
 *   Abbrevs are scoped to the block they are defined in: a subblock
 *   starts with an empty abbrev set, except that abbrevs defined in a
 *   BLOCKINFO block for that block ID are seeded first (IDs from 4),
 *   and the block's own DEFINE_ABBREV records continue the numbering.
 *   Abbrev operand encodings: Fixed 1, VBR 2, Array 3, Char6 4,
 *   Blob 5. A Blob is a vbr6 length, align to 32 bits, then bytes,
 *   then tail padding to a multiple of 4 bytes. An Array is a vbr6
 *   element count followed by elements decoded by the next operand in
 *   the abbrev; an array (or a blob) may only be the last operand.
 *
 *   Block IDs: BLOCKINFO 0, MODULE 8, PARAMATTR 9, PARAMATTR_GROUP 10,
 *   CONSTANTS 11, FUNCTION 12, IDENTIFICATION 13, VALUE_SYMTAB 14,
 *   METADATA 15, METADATA_ATTACHMENT 16, TYPE (TYPE_BLOCK_ID_NEW) 17,
 *   STRTAB 23.
 *
 *   MODULE record codes: VERSION 1, TRIPLE 2, DATALAYOUT 3, ASM 4,
 *   SECTIONNAME 5, DEPLIB 6, GLOBALVAR 7, FUNCTION 8, ALIAS_OLD 9,
 *   GCNAME 11, COMDAT 12, VSTOFFSET 13, ALIAS 14, SOURCE_FILENAME 16,
 *   HASH 17, IFUNC 18. (The doc still describes ALIAS as code 9; the
 *   current header defines ALIAS_OLD 9 and ALIAS 14. A current writer
 *   emits 14.) In module version 2, GLOBALVAR/FUNCTION/ALIAS first
 *   two operands are strtab offset and size; the name is no longer
 *   inline.
 *
 *   FUNCTION record codes (useful subset): DECLAREBLOCKS 1,
 *   INST_BINOP 2, INST_CAST 3, INST_GEP_OLD 4, INST_SELECT 5,
 *   INST_EXTRACTELT 6, INST_INSERTELT 7, INST_SHUFFLEVEC 8,
 *   INST_CMP 9, INST_RET 10, INST_BR 11, INST_SWITCH 12,
 *   INST_INVOKE 13, INST_UNREACHABLE 15, INST_PHI 16, INST_ALLOCA 19,
 *   INST_LOAD 20, INST_STORE_OLD 24, INST_CALL 34, INST_GEP 43,
 *   INST_STORE 44.
 *
 * The reader never allocates. The caller provides the buffer and a
 * bc_reader_t (about 10 KiB; allocate statically). Blob operands point
 * into the caller's buffer.
 *
 * No libc. Freestanding: -Wall -Wextra -Werror clean with the NDK
 * aarch64 clang -ffreestanding.
 */
#ifndef IOSWINE_BITCODE_H
#define IOSWINE_BITCODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC_WRAPPER_MAGIC 0x0b17c0deu
#define BC_STREAM_MAGIC 0xdec04342u /* "BC\xc0\xde" as little-endian u32 */
#define BC_WRAPPER_HEADER_SIZE 20u

/* Return codes: 0 is success, anything else is negative. */
#define BC_OK 0
#define BC_ERR_TOO_SMALL -1
#define BC_ERR_BAD_MAGIC -2
#define BC_ERR_TRUNCATED -3
#define BC_ERR_BAD_ABBREV -4
#define BC_ERR_BAD_BLOCK -5
#define BC_ERR_TOO_DEEP -6
#define BC_ERR_NO_END_BLOCK -7
#define BC_ERR_TOO_MANY_OPS -8
#define BC_ERR_BAD_VBR -9
#define BC_ERR_BAD_OP -10

/* Bitstream record codes. */
#define BC_END_BLOCK 0u
#define BC_ENTER_SUBBLOCK 1u
#define BC_DEFINE_ABBREV 2u
#define BC_UNABBREV_RECORD 3u
#define BC_FIRST_APPLICATION_ABBREV 4u

/* Standard block IDs. */
#define BC_BLOCKINFO_BLOCK_ID 0u
#define BC_MODULE_BLOCK_ID 8u
#define BC_PARAMATTR_BLOCK_ID 9u
#define BC_PARAMATTR_GROUP_BLOCK_ID 10u
#define BC_CONSTANTS_BLOCK_ID 11u
#define BC_FUNCTION_BLOCK_ID 12u
#define BC_IDENTIFICATION_BLOCK_ID 13u
#define BC_VALUE_SYMTAB_BLOCK_ID 14u
#define BC_METADATA_BLOCK_ID 15u
#define BC_METADATA_ATTACHMENT_ID 16u
#define BC_TYPE_BLOCK_ID 17u
#define BC_STRTAB_BLOCK_ID 23u

/* BLOCKINFO record codes. */
#define BC_BI_SETBID 1u
#define BC_BI_BLOCKNAME 2u
#define BC_BI_SETRECORDNAME 3u

/* MODULE block record codes. */
#define BC_MOD_VERSION 1u
#define BC_MOD_TRIPLE 2u
#define BC_MOD_DATALAYOUT 3u
#define BC_MOD_ASM 4u
#define BC_MOD_SECTIONNAME 5u
#define BC_MOD_DEPLIB 6u
#define BC_MOD_GLOBALVAR 7u
#define BC_MOD_FUNCTION 8u
#define BC_MOD_ALIAS_OLD 9u
#define BC_MOD_GCNAME 11u
#define BC_MOD_COMDAT 12u
#define BC_MOD_VSTOFFSET 13u
#define BC_MOD_ALIAS 14u
#define BC_MOD_SOURCE_FILENAME 16u
#define BC_MOD_HASH 17u
#define BC_MOD_IFUNC 18u

/* FUNCTION block record codes (useful subset). */
#define BC_FUNC_DECLAREBLOCKS 1u
#define BC_FUNC_INST_BINOP 2u
#define BC_FUNC_INST_CAST 3u
#define BC_FUNC_INST_RET 10u
#define BC_FUNC_INST_BR 11u
#define BC_FUNC_INST_PHI 16u
#define BC_FUNC_INST_ALLOCA 19u
#define BC_FUNC_INST_LOAD 20u
#define BC_FUNC_INST_CALL 34u
#define BC_FUNC_INST_GEP 43u
#define BC_FUNC_INST_STORE 44u

/* STRTAB block record codes. */
#define BC_STRTAB_BLOB 1u

/* Abbrev operand encodings. */
#define BC_OP_FIXED 1u
#define BC_OP_VBR 2u
#define BC_OP_ARRAY 3u
#define BC_OP_CHAR6 4u
#define BC_OP_BLOB 5u

/* Reader limits. */
#define BC_MAX_OPERANDS 32u
#define BC_MAX_ABBREV_OPS 8u
#define BC_MAX_ABBREVS 64u
#define BC_MAX_BI_ABBREVS 32u
#define BC_MAX_DEPTH 32u

typedef enum {
    BC_EV_BLOCK_ENTER,
    BC_EV_BLOCK_END,
    BC_EV_RECORD,
    BC_EV_EOF
} bc_event_kind_t;

typedef struct {
    bc_event_kind_t kind;
    uint32_t block_id;        /* BLOCK_ENTER / BLOCK_END */
    uint32_t block_len_words; /* BLOCK_ENTER */
    uint32_t code;            /* RECORD: record code */
    uint32_t abbrev_id;       /* RECORD: 0 = unabbreviated */
    uint32_t op_count;        /* RECORD: number of operands in ops[] */
    uint64_t ops[BC_MAX_OPERANDS]; /* RECORD: operands after the code.
        * Array operands contribute their element count first, then the
        * elements in order. A Blob operand contributes its byte length
        * here, and its bytes in blob/blob_len. Char6 array elements
        * appear as their 6-bit values. */
    const uint8_t *blob;      /* RECORD: blob bytes, when has_blob */
    uint32_t blob_len;
    uint8_t has_blob;         /* RECORD: last operand was a blob */
} bc_event_t;

typedef struct {
    uint8_t is_literal; /* 1 = literal value, 0 = encoding */
    uint8_t encoding;   /* BC_OP_* when not a literal */
    uint64_t value;     /* literal value, or Fixed/VBR bit width */
} bc_abbrev_op_t;

typedef struct {
    uint8_t op_count;
    bc_abbrev_op_t ops[BC_MAX_ABBREV_OPS];
} bc_abbrev_t;

typedef struct {
    uint32_t block_id;
    uint64_t end_bitpos;      /* bit position where this block ends */
    uint32_t saved_abbrev_width;
    uint32_t next_abbrev;     /* next abbrev id to assign in this block */
} bc_block_frame_t;

typedef struct {
    const uint8_t *data; /* bitcode stream, not owned */
    uint32_t size;       /* stream size in bytes, magic included */
    uint64_t bitpos;     /* bits consumed from the start of data */
    size_t bytepos;      /* next byte to load into the bit buffer */
    uint64_t bitbuf;     /* prefetched bits, low bit first */
    uint32_t bitcnt;     /* bits available in bitbuf */
    uint32_t abbrev_width; /* current block's abbrev id width */
    uint32_t depth;
    bc_block_frame_t stack[BC_MAX_DEPTH];
    /* Working abbrev table. Entries carry their owning depth; on block
     * exit, entries deeper than the new depth are dropped by truncation
     * (they were appended after the enclosing scope's entries). */
    bc_abbrev_t abbrevs[BC_MAX_ABBREVS];
    uint8_t abbrev_depth[BC_MAX_ABBREVS];
    uint32_t abbrev_id[BC_MAX_ABBREVS];
    uint32_t abbrev_count;
    /* BLOCKINFO abbrevs, stored per target block id. */
    bc_abbrev_t bi_abbrevs[BC_MAX_BI_ABBREVS];
    uint32_t bi_block_id[BC_MAX_BI_ABBREVS];
    uint32_t bi_abbrev_id[BC_MAX_BI_ABBREVS];
    uint32_t bi_count;
    uint32_t bi_target;   /* current SETBID target block id */
    uint8_t stream_started; /* set once bc_open positioned past the magic */
} bc_reader_t;

/* Open a bitcode buffer. Accepts a wrapper (magic 0x0b17c0de, stream at
 * the wrapper's offset) or a raw stream (magic "BC\xc0\xde"). On
 * success the reader is positioned at the first record. */
int bc_open(const uint8_t *data, uint32_t size, bc_reader_t *r);

/* Read the next event: a block enter, a block end, a record, or EOF.
 * DEFINE_ABBREV records are consumed internally. Returns BC_OK and
 * fills ev, or a negative error. After BC_EV_EOF the reader is done.
 * A record's code is in ev->code and its operands (after the code)
 * in ev->ops[0..op_count). */
int bc_next(bc_reader_t *r, bc_event_t *ev);

/* One-line description of a bc_* error code. */
const char *bc_error_string(int err);

#ifdef __cplusplus
}
#endif

#endif /* IOSWINE_BITCODE_H */
