/* air2spirv.h - translator: parsed AIR function description to SPIR-V.
 *
 * First path only: a trivial vertex+fragment pair. The translator takes
 * a parsed AIR function (entry point name, stage, interface) and emits
 * a valid SPIR-V module into a caller-provided word buffer. A future
 * stage fills the description struct from real AIR bitcode (function
 * name from the VALUE_SYMTAB, stage from the air.vertex / air.fragment
 * metadata, arguments from the function signature); the emitter itself
 * never sees bitcode and has no LLVM dependency.
 *
 * Vertex (passthrough): one vec4 input at the given location is loaded
 * and stored to gl_Position (BuiltIn Position). Fragment (solid
 * color): a constant vec4 color is stored to the vec4 output at
 * location 0. Both modules use OpCapability Shader, Logical addressing
 * with the GLSL450 memory model, and validate with spirv-val.
 *
 * No libc. Freestanding: -Wall -Wextra -Werror clean with the NDK
 * aarch64 clang -ffreestanding.
 */
#ifndef IOSWINE_AIR2SPIRV_H
#define IOSWINE_AIR2SPIRV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A2S_OK 0
#define A2S_ERR_BAD_ARG -1
#define A2S_ERR_OVERFLOW -2
#define A2S_ERR_BAD_STAGE -3

/* SPIR-V magic and version emitted in the module header. */
#define A2S_SPIRV_MAGIC 0x07230203u
#define A2S_SPIRV_VERSION 0x00010000u /* SPIR-V 1.0 */

typedef enum {
    A2S_STAGE_VERTEX = 0,
    A2S_STAGE_FRAGMENT = 1,
    A2S_STAGE_KERNEL = 2
} a2s_stage_t;

typedef struct {
    const char *entry_name; /* e.g. "main"; NUL-terminated */
    a2s_stage_t stage;
    uint32_t in_location;   /* vertex: location of the vec4 position input */
    float color[4];         /* fragment: solid output color, RGBA */
} a2s_function_t;

/* Emit one SPIR-V module for fn into words[0..capacity). On success
 * *out_count holds the word count (the module is out_count * 4 bytes).
 * The buffer must stay alive while the module is used. Fails with
 * A2S_ERR_BAD_ARG (null pointers, empty entry name, kernel stage: not
 * implemented in this first path), A2S_ERR_OVERFLOW (buffer too
 * small), or A2S_ERR_BAD_STAGE. */
int a2s_emit(const a2s_function_t *fn, uint32_t *words, uint32_t capacity,
             uint32_t *out_count);

/* One-line description of an a2s_* error code. */
const char *a2s_error_string(int err);

#ifdef __cplusplus
}
#endif

#endif /* IOSWINE_AIR2SPIRV_H */
