/* write_hello: write(1, "write hello\n", 12); return 0;
 * First M2 test program linked against libSystem.B.dylib.
 */

typedef long ssize_t;
typedef unsigned long size_t;

ssize_t write(int fd, const void *buf, size_t count);

int main(void) {
    write(1, "write hello\n", 12);
    return 0;
}
