/* cg_hello: Render a 1024x1024 image with CoreGraphics and write cg_hello.png with ImageIO */

#include <stddef.h>
#include <stdint.h>

/* Hand-declared CoreFoundation types & prototypes */
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFData *CFMutableDataRef;
typedef long CFIndex;
typedef unsigned char Boolean;

#define kCFStringEncodingUTF8 0x08000100

CFURLRef CFURLCreateFromFileSystemRepresentation(
    CFAllocatorRef allocator,
    const unsigned char *buffer,
    CFIndex bufLen,
    Boolean isDirectory);

CFStringRef CFStringCreateWithCString(
    CFAllocatorRef alloc,
    const char *cStr,
    uint32_t encoding);

void CFRelease(CFTypeRef cf);
CFIndex CFArrayGetCount(CFArrayRef theArray);
const void *CFArrayGetValueAtIndex(CFArrayRef theArray, CFIndex idx);
Boolean CFStringGetCString(CFStringRef theString, char *buffer, CFIndex bufferSize, uint32_t encoding);

CFMutableDataRef CFDataCreateMutable(CFAllocatorRef allocator, CFIndex capacity);
CFIndex CFDataGetLength(CFMutableDataRef theData);
const uint8_t *CFDataGetBytePtr(CFMutableDataRef theData);

/* Hand-declared CoreGraphics types & prototypes */
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
typedef struct CGGradient *CGGradientRef;
typedef struct CGImage *CGImageRef;
typedef struct CGImageDestination *CGImageDestinationRef;

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
void CGContextSetRGBStrokeColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
void CGContextFillRect(CGContextRef c, CGRect rect);
void CGContextFillEllipseInRect(CGContextRef c, CGRect rect);
void CGContextAddArc(CGContextRef c, CGFloat x, CGFloat y, CGFloat radius, CGFloat startAngle, CGFloat endAngle, int clockwise);
void CGContextMoveToPoint(CGContextRef c, CGFloat x, CGFloat y);
void CGContextAddLineToPoint(CGContextRef c, CGFloat x, CGFloat y);
void CGContextStrokePath(CGContextRef c);
void CGContextSetLineWidth(CGContextRef c, CGFloat width);

CGGradientRef CGGradientCreateWithColorComponents(
    CGColorSpaceRef space,
    const CGFloat components[],
    const CGFloat locations[],
    size_t count);
void CGGradientRelease(CGGradientRef gradient);

void CGContextDrawLinearGradient(
    CGContextRef c,
    CGGradientRef gradient,
    CGPoint startPoint,
    CGPoint endPoint,
    uint32_t options);

CGImageRef CGBitmapContextCreateImage(CGContextRef c);
void CGImageRelease(CGImageRef image);

/* Hand-declared ImageIO prototypes */
CFArrayRef CGImageDestinationCopyTypeIdentifiers(void);
CGImageDestinationRef CGImageDestinationCreateWithURL(
    CFURLRef url,
    CFStringRef type,
    size_t count,
    CFTypeRef options);

CGImageDestinationRef CGImageDestinationCreateWithData(
    CFMutableDataRef data,
    CFStringRef type,
    size_t count,
    CFTypeRef options);

void CGImageDestinationAddImage(
    CGImageDestinationRef idst,
    CGImageRef image,
    CFTypeRef properties);

Boolean CGImageDestinationFinalize(CGImageDestinationRef idst);

/* Standard libSystem functions */
int printf(const char *fmt, ...);
int fflush(void *stream);
size_t strlen(const char *s);
int *__error(void);
char *getcwd(char *buf, size_t size);
int snprintf(char *str, size_t size, const char *format, ...);
int open(const char *path, int flags, ...);
long write(int fd, const void *buf, size_t count);
int close(int fd);

/* Helper to draw rounded rectangle */
static void add_rounded_rect(CGContextRef ctx, CGFloat x, CGFloat y, CGFloat w, CGFloat h, CGFloat r) {
    CGContextMoveToPoint(ctx, x + r, y);
    CGContextAddLineToPoint(ctx, x + w - r, y);
    CGContextAddArc(ctx, x + w - r, y + r, r, 4.71238898038469, 6.283185307179586, 0);
    CGContextAddLineToPoint(ctx, x + w, y + h - r);
    CGContextAddArc(ctx, x + w - r, y + h - r, r, 0.0, 1.5707963267948966, 0);
    CGContextAddLineToPoint(ctx, x + r, y + h);
    CGContextAddArc(ctx, x + r, y + h - r, r, 1.5707963267948966, 3.141592653589793, 0);
    CGContextAddLineToPoint(ctx, x, y + r);
    CGContextAddArc(ctx, x + r, y + r, r, 3.141592653589793, 4.71238898038469, 0);
}

static void fill_rounded_rect(CGContextRef ctx, CGFloat x, CGFloat y, CGFloat w, CGFloat h, CGFloat r) {
    add_rounded_rect(ctx, x, y, w, h, r);
    CGContextFillRect(ctx, (CGRect){{x, y}, {w, h}}); /* simple fallback if needed, but fill path is better */
}

int main(void) {
    const int width = 1024;
    const int height = 1024;

    /* Step 1: Create RGB bitmap context */
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (!colorSpace) {
        printf("cg: failed to create color space\n");
        return 1;
    }

    /* 8 bits per component, 4 bytes per pixel (RGBA premultiplied = 1) */
    CGContextRef ctx = CGBitmapContextCreate(NULL, width, height, 8, width * 4, colorSpace, 1);
    if (!ctx) {
        printf("cg: failed to create bitmap context\n");
        return 1;
    }
    printf("cg: context\n");
    fflush(NULL);

    /* Step 2: Draw the image */

    /* 2a. Background: dark-blue to purple linear gradient */
    CGFloat gradientColors[] = {
        0.05, 0.05, 0.25, 1.0,  /* bottom: dark blue */
        0.42, 0.12, 0.55, 1.0   /* top: rich purple */
    };
    CGFloat gradientLocations[] = { 0.0, 1.0 };
    CGGradientRef bgGradient = CGGradientCreateWithColorComponents(colorSpace, gradientColors, gradientLocations, 2);
    if (bgGradient) {
        CGPoint startPoint = { 512.0, 0.0 };
        CGPoint endPoint = { 512.0, 1024.0 };
        CGContextDrawLinearGradient(ctx, bgGradient, startPoint, endPoint, 0);
        CGGradientRelease(bgGradient);
    }

    /* 2b. Big white circle outline */
    CGContextSetRGBStrokeColor(ctx, 1.0, 1.0, 1.0, 0.85);
    CGContextSetLineWidth(ctx, 8.0);
    CGContextAddArc(ctx, 512.0, 512.0, 440.0, 0.0, 6.283185307179586, 0);
    CGContextStrokePath(ctx);

    /* 2c. Inner decorative dashed / thinner circle */
    CGContextSetRGBStrokeColor(ctx, 1.0, 1.0, 1.0, 0.35);
    CGContextSetLineWidth(ctx, 2.0);
    CGContextAddArc(ctx, 512.0, 512.0, 420.0, 0.0, 6.283185307179586, 0);
    CGContextStrokePath(ctx);

    /* 2d. Phone outline (iOS device shape in center) */
    /* Phone body: 360 x 620, centered at (512, 512) -> x from 332 to 692, y from 202 to 822 */
    add_rounded_rect(ctx, 332.0, 202.0, 360.0, 620.0, 48.0);
    CGContextSetRGBStrokeColor(ctx, 0.85, 0.88, 0.95, 0.95);
    CGContextSetLineWidth(ctx, 6.0);
    CGContextStrokePath(ctx);

    /* Phone glass screen: slightly darker rounded rect fill */
    add_rounded_rect(ctx, 342.0, 212.0, 340.0, 600.0, 40.0);
    CGContextSetRGBFillColor(ctx, 0.08, 0.10, 0.15, 0.70);
    CGContextFillEllipseInRect(ctx, (CGRect){{342.0, 212.0}, {340.0, 600.0}}); /* subtle screen vignette */

    /* Phone Dynamic Island: black pill at top */
    CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 0.95);
    CGContextFillEllipseInRect(ctx, (CGRect){{462.0, 776.0}, {100.0, 28.0}});

    /* Phone Home Indicator bar: white pill at bottom */
    CGContextSetRGBStrokeColor(ctx, 1.0, 1.0, 1.0, 0.80);
    CGContextSetLineWidth(ctx, 5.0);
    CGContextMoveToPoint(ctx, 452.0, 230.0);
    CGContextAddLineToPoint(ctx, 572.0, 230.0);
    CGContextStrokePath(ctx);

    /* 2e. Android robot motif inside the screen */
    /* Android green: #3DDC84 -> (0.24, 0.86, 0.52) */
    CGFloat agR = 0.24, agG = 0.86, agB = 0.52;

    /* Android Head: dome from angle 0 to pi */
    CGContextSetRGBFillColor(ctx, agR, agG, agB, 1.0);
    CGContextMoveToPoint(ctx, 592.0, 540.0);
    CGContextAddArc(ctx, 512.0, 540.0, 80.0, 0.0, 3.141592653589793, 0);
    CGContextAddLineToPoint(ctx, 592.0, 540.0);
    CGContextStrokePath(ctx); /* stroke baseline */
    /* Fill head dome using arc and line */
    CGContextSetRGBFillColor(ctx, agR, agG, agB, 1.0);
    CGContextFillEllipseInRect(ctx, (CGRect){{432.0, 460.0}, {160.0, 160.0}});
    /* Mask/cover bottom half of ellipse to make dome */
    CGContextSetRGBFillColor(ctx, 0.08, 0.10, 0.15, 1.0);
    CGContextFillRect(ctx, (CGRect){{430.0, 450.0}, {164.0, 90.0}});

    /* Android Eyes: white circles */
    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
    CGContextFillEllipseInRect(ctx, (CGRect){{468.0, 566.0}, {16.0, 16.0}});
    CGContextFillEllipseInRect(ctx, (CGRect){{540.0, 566.0}, {16.0, 16.0}});

    /* Android Antennas */
    CGContextSetRGBStrokeColor(ctx, agR, agG, agB, 1.0);
    CGContextSetLineWidth(ctx, 6.0);
    /* Left antenna */
    CGContextMoveToPoint(ctx, 468.0, 606.0);
    CGContextAddLineToPoint(ctx, 442.0, 646.0);
    CGContextStrokePath(ctx);
    /* Right antenna */
    CGContextMoveToPoint(ctx, 556.0, 606.0);
    CGContextAddLineToPoint(ctx, 582.0, 646.0);
    CGContextStrokePath(ctx);

    /* Android Body: main torso rect */
    CGContextSetRGBFillColor(ctx, agR, agG, agB, 1.0);
    CGContextFillRect(ctx, (CGRect){{432.0, 390.0}, {160.0, 140.0}});

    /* Torso bottom rounding */
    CGContextFillEllipseInRect(ctx, (CGRect){{432.0, 366.0}, {48.0, 48.0}});
    CGContextFillEllipseInRect(ctx, (CGRect){{544.0, 366.0}, {48.0, 48.0}});
    CGContextFillRect(ctx, (CGRect){{456.0, 366.0}, {112.0, 24.0}});

    /* Android Left Arm */
    CGContextSetLineWidth(ctx, 22.0);
    CGContextMoveToPoint(ctx, 408.0, 420.0);
    CGContextAddLineToPoint(ctx, 408.0, 490.0);
    CGContextStrokePath(ctx);

    /* Android Right Arm */
    CGContextMoveToPoint(ctx, 616.0, 420.0);
    CGContextAddLineToPoint(ctx, 616.0, 490.0);
    CGContextStrokePath(ctx);

    /* Android Left Leg */
    CGContextSetLineWidth(ctx, 24.0);
    CGContextMoveToPoint(ctx, 472.0, 330.0);
    CGContextAddLineToPoint(ctx, 472.0, 370.0);
    CGContextStrokePath(ctx);

    /* Android Right Leg */
    CGContextMoveToPoint(ctx, 552.0, 330.0);
    CGContextAddLineToPoint(ctx, 552.0, 370.0);
    CGContextStrokePath(ctx);

    /* Apple logo accent in the center of the Android body: small white apple bite outline */
    CGContextSetRGBStrokeColor(ctx, 1.0, 1.0, 1.0, 0.95);
    CGContextSetLineWidth(ctx, 4.0);
    CGContextAddArc(ctx, 512.0, 460.0, 26.0, 0.0, 6.283185307179586, 0);
    CGContextStrokePath(ctx);
    /* Small leaf above the circle */
    CGContextMoveToPoint(ctx, 512.0, 488.0);
    CGContextAddLineToPoint(ctx, 524.0, 502.0);
    CGContextStrokePath(ctx);

    printf("cg: drawn\n");
    fflush(NULL);

    /* Step 3: Create image and write PNG */
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    if (!image) {
        printf("cg: failed to create image\n");
        return 1;
    }

    char filepath[1024];
    if (getcwd(filepath, sizeof(filepath))) {
        size_t len = strlen(filepath);
        if (len + 14 < sizeof(filepath)) {
            if (filepath[len - 1] != '/') {
                filepath[len] = '/';
                filepath[len + 1] = '\0';
                len++;
            }
            snprintf(filepath + len, sizeof(filepath) - len, "cg_hello.png");
        }
    } else {
        snprintf(filepath, sizeof(filepath), "cg_hello.png");
    }

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const unsigned char *)filepath, (CFIndex)strlen(filepath), 0);
    if (!url) {
        printf("cg: failed to create file URL\n");
        return 1;
    }

    CFStringRef uti = CFStringCreateWithCString(NULL, "public.png", kCFStringEncodingUTF8);
    if (!uti) {
        printf("cg: failed to create UTI string\n");
        return 1;
    }

    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, uti, 1, NULL);
    if (!dest) {
        printf("cg: failed to create image destination, path='%s', errno=%d\n", filepath, *__error());
        return 1;
    }

    CGImageDestinationAddImage(dest, image, NULL);
    if (!CGImageDestinationFinalize(dest)) {
        printf("cg: failed to finalize image destination\n");
        return 1;
    }

    printf("cg: png written\n");
    printf("cg ok %dx%d\n", width, height);
    fflush(NULL);

    /* Cleanup */
    CFRelease(dest);
    CFRelease(uti);
    CFRelease(url);
    CGImageRelease(image);
    CGColorSpaceRelease(colorSpace);
    CGContextRelease(ctx);

    return 0;
}
