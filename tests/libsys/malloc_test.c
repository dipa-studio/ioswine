/* malloc_test: allocate 1000 blocks of varying sizes (16 B - 1 MiB),
 * write patterns, verify, free; print "malloc ok".
 */

typedef unsigned long size_t;

void *malloc(size_t size);
void free(void *ptr);
int puts(const char *s);
void exit(int status);

#define NUM_BLOCKS 1000
#define MIN_SIZE 16
#define MAX_SIZE (1024 * 1024)

static void *blocks[NUM_BLOCKS];
static size_t sizes[NUM_BLOCKS];

int main(void) {
    /* Allocate 1000 blocks of varying sizes (16 B - 1 MiB) */
    for (int i = 0; i < NUM_BLOCKS; i++) {
        size_t sz = MIN_SIZE + (size_t)(((unsigned long long)i * 104729ULL) % (MAX_SIZE - MIN_SIZE + 1));
        sizes[i] = sz;
        unsigned char *p = (unsigned char *)malloc(sz);
        if (!p) {
            puts("malloc failed");
            exit(1);
        }
        blocks[i] = p;

        /* Write pattern across the block */
        unsigned char pat = (unsigned char)(i & 0xff);
        p[0] = pat;
        p[sz / 4] = (unsigned char)(pat ^ 0x3c);
        p[sz / 2] = (unsigned char)(pat ^ 0x5a);
        p[3 * (sz / 4)] = (unsigned char)(pat ^ 0xc3);
        p[sz - 1] = (unsigned char)(pat ^ 0xa5);
    }

    /* Verify patterns */
    for (int i = 0; i < NUM_BLOCKS; i++) {
        unsigned char *p = (unsigned char *)blocks[i];
        size_t sz = sizes[i];
        unsigned char pat = (unsigned char)(i & 0xff);

        if (p[0] != pat ||
            p[sz / 4] != (unsigned char)(pat ^ 0x3c) ||
            p[sz / 2] != (unsigned char)(pat ^ 0x5a) ||
            p[3 * (sz / 4)] != (unsigned char)(pat ^ 0xc3) ||
            p[sz - 1] != (unsigned char)(pat ^ 0xa5)) {
            puts("malloc pattern verification failed");
            exit(1);
        }
    }

    /* Free all allocated blocks */
    for (int i = 0; i < NUM_BLOCKS; i++) {
        free(blocks[i]);
        blocks[i] = 0;
    }

    puts("malloc ok");
    return 0;
}
