/*
 * cg_present: Render an animated CoreGraphics test pattern (1080x2400 BGRA)
 * and present ~10 frames/s for 10 s via libsystem_kernel's ioswine_present_frame().
 */

#include <stddef.h>
#include <stdint.h>

typedef double CGFloat;

typedef struct CGPoint {
    CGFloat x;
    CGFloat y;
} CGPoint;

typedef struct CGSize {
    CGFloat width;
    CGFloat height;
} CGSize;

typedef struct CGRect {
    CGPoint origin;
    CGSize size;
} CGRect;

typedef struct CGColorSpace *CGColorSpaceRef;
typedef struct CGContext *CGContextRef;

/* kCGBitmapByteOrder32Little (0x2000) | kCGImageAlphaPremultipliedFirst (2) = BGRA8888 premul */
#define CG_BITMAP_BGRA8888 0x2002u
/* kCGImageAlphaPremultipliedLast (1) = RGBA8888 premul fallback */
#define CG_BITMAP_RGBA8888 0x0001u

CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);
void CGColorSpaceRelease(CGColorSpaceRef space);

CGContextRef CGBitmapContextCreate(
    void *data,
    size_t width,
    size_t height,
    size_t bitsPerComponent,
    size_t bytesPerRow,
    CGColorSpaceRef space,
    uint32_t bitmapInfo);
void CGContextRelease(CGContextRef c);
void CGContextSetRGBFillColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
void CGContextFillRect(CGContextRef c, CGRect rect);

/* Standard libSystem functions */
int printf(const char *fmt, ...);
int fflush(void *stream);
void *malloc(size_t size);
void free(void *ptr);
int usleep(unsigned int usec);
void *dlsym(void *handle, const char *symbol);

__attribute__((weak_import))
extern int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h, uint32_t stride);

typedef int (*present_frame_fn_t)(const void *bgra, uint32_t w, uint32_t h, uint32_t stride);

static present_frame_fn_t resolve_present_fn(void) {
    if (ioswine_present_frame) return ioswine_present_frame;
    present_frame_fn_t fn = (present_frame_fn_t)dlsym((void *)(intptr_t)-2, "ioswine_present_frame");
    if (fn) return fn;
    /* loader/dyld.c loads our libsystem_kernel.dylib at fixed LSK_BASE (0x300000000). */
    fn = (present_frame_fn_t)dlsym((void *)0x300000000ull, "ioswine_present_frame");
    if (fn) return fn;
    return NULL;
}

static void fill_rect(CGContextRef ctx, CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
    CGContextFillRect(ctx, (CGRect){{x, y}, {w, h}});
}

static void draw_frame(CGContextRef ctx, uint32_t w, uint32_t h, int frame) {
    /* 1. Background */
    CGContextSetRGBFillColor(ctx, 0.05, 0.08, 0.18, 1.0);
    fill_rect(ctx, 0.0, 0.0, (CGFloat)w, (CGFloat)h);

    /* 2. Perimeter border rects */
    CGContextSetRGBFillColor(ctx, 0.25, 0.45, 0.70, 1.0);
    fill_rect(ctx, 0.0, 0.0, (CGFloat)w, 12.0);
    fill_rect(ctx, 0.0, (CGFloat)h - 12.0, (CGFloat)w, 12.0);
    fill_rect(ctx, 0.0, 0.0, 12.0, (CGFloat)h);
    fill_rect(ctx, (CGFloat)w - 12.0, 0.0, 12.0, (CGFloat)h);

    /* 3. Moving horizontal bar (ping-pong) */
    const CGFloat bar_h = 160.0;
    const int period = 50;
    int phase = frame % (period * 2);
    if (phase >= period) phase = period * 2 - phase;
    CGFloat bar_y = 24.0 + ((CGFloat)phase / (CGFloat)period) * ((CGFloat)h - bar_h - 48.0);
    CGContextSetRGBFillColor(ctx, 0.12, 0.85, 0.60, 1.0);
    fill_rect(ctx, 24.0, bar_y, (CGFloat)w - 48.0, bar_h);
    CGContextSetRGBFillColor(ctx, 0.95, 0.98, 1.0, 1.0);
    fill_rect(ctx, 24.0, bar_y + 65.0, (CGFloat)w - 48.0, 30.0);

    /* 4. Moving horizontal shuttle block */
    const CGFloat blk_sz = 140.0;
    int hphase = (frame * 2) % (period * 2);
    if (hphase >= period) hphase = period * 2 - hphase;
    CGFloat blk_x = 40.0 + ((CGFloat)hphase / (CGFloat)period) * ((CGFloat)w - blk_sz - 80.0);
    CGContextSetRGBFillColor(ctx, 0.96, 0.48, 0.12, 1.0);
    fill_rect(ctx, blk_x, (CGFloat)h * 0.5 - blk_sz * 0.5, blk_sz, blk_sz);

    /* 5. Frame counter as 8 binary bit-rects + progress bar */
    CGContextSetRGBFillColor(ctx, 0.12, 0.16, 0.28, 1.0);
    fill_rect(ctx, 60.0, (CGFloat)h - 320.0, (CGFloat)w - 120.0, 220.0);

    for (int bit = 0; bit < 8; bit++) {
        int set = (frame >> (7 - bit)) & 1;
        if (set) {
            CGContextSetRGBFillColor(ctx, 1.0, 0.90, 0.20, 1.0);
        } else {
            CGContextSetRGBFillColor(ctx, 0.22, 0.26, 0.36, 1.0);
        }
        CGFloat rx = 90.0 + (CGFloat)bit * 116.0;
        fill_rect(ctx, rx, (CGFloat)h - 220.0, 92.0, 92.0);
    }

    CGFloat prog_w = ((CGFloat)(frame % 100) / 99.0) * ((CGFloat)w - 180.0);
    if (prog_w < 12.0) prog_w = 12.0;
    CGContextSetRGBFillColor(ctx, 0.30, 0.80, 1.0, 1.0);
    fill_rect(ctx, 90.0, (CGFloat)h - 290.0, prog_w, 36.0);
}

static void rgba_to_bgra(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h) {
    size_t count = (size_t)w * h;
    for (size_t i = 0; i < count; i++) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        uint8_t a = src[i * 4 + 3];
        dst[i * 4 + 0] = b;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = r;
        dst[i * 4 + 3] = a;
    }
}

int main(void) {
    const uint32_t width = 1080;
    const uint32_t height = 2400;
    const uint32_t stride = width * 4u;
    const int total_frames = 100;

    present_frame_fn_t present_fn = resolve_present_fn();
    if (!present_fn) {
        printf("cg_present: ioswine_present_frame symbol unavailable\n");
        printf("cg_present: 0 frames presented\n");
        fflush(NULL);
        return 0;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) {
        printf("cg_present: CGColorSpaceCreateDeviceRGB failed\n");
        return 1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)stride * height);
    if (!buf) {
        printf("cg_present: malloc failed\n");
        CGColorSpaceRelease(cs);
        return 1;
    }

    int need_swizzle = 0;
    uint8_t *swiz_buf = NULL;
    CGContextRef ctx = CGBitmapContextCreate(buf, width, height, 8, stride, cs, CG_BITMAP_BGRA8888);
    if (!ctx) {
        ctx = CGBitmapContextCreate(buf, width, height, 8, stride, cs, CG_BITMAP_RGBA8888);
        if (!ctx) {
            printf("cg_present: CGBitmapContextCreate failed\n");
            free(buf);
            CGColorSpaceRelease(cs);
            return 1;
        }
        need_swizzle = 1;
        swiz_buf = (uint8_t *)malloc((size_t)stride * height);
        if (!swiz_buf) {
            printf("cg_present: malloc swiz_buf failed\n");
            CGContextRelease(ctx);
            free(buf);
            CGColorSpaceRelease(cs);
            return 1;
        }
    }

    int presented = 0;
    for (int f = 0; f < total_frames; f++) {
        draw_frame(ctx, width, height, f);
        const void *frame_ptr = buf;
        if (need_swizzle) {
            rgba_to_bgra(buf, swiz_buf, width, height);
            frame_ptr = swiz_buf;
        }
        if (present_fn(frame_ptr, width, height, stride) == 0) {
            presented++;
        }
        usleep(100000);
    }

    printf("cg_present: %d frames presented\n", presented);
    fflush(NULL);

    CGContextRelease(ctx);
    if (swiz_buf) free(swiz_buf);
    free(buf);
    CGColorSpaceRelease(cs);
    return 0;
}
