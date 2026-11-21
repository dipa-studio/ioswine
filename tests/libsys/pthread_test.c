/* pthread_test: create 4 threads each summing 1..1000000 into its own slot,
 * join, check, print "pthread ok".
 */

typedef unsigned long pthread_t;
typedef void *pthread_attr_t;

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int puts(const char *s);
void exit(int status);

#define EXPECTED_SUM 500000500000ULL

static unsigned long long slot[4];

static void *worker(void *arg) {
    long idx = (long)arg;
    unsigned long long sum = 0;
    for (unsigned long long i = 1; i <= 1000000ULL; i++) {
        sum += i;
    }
    slot[idx] = sum;
    return 0;
}

int main(void) {
    pthread_t th[4];

    for (long i = 0; i < 4; i++) {
        if (pthread_create(&th[i], 0, worker, (void *)i) != 0) {
            puts("pthread_create failed");
            exit(1);
        }
    }

    for (int i = 0; i < 4; i++) {
        if (pthread_join(th[i], 0) != 0) {
            puts("pthread_join failed");
            exit(1);
        }
    }

    for (int i = 0; i < 4; i++) {
        if (slot[i] != EXPECTED_SUM) {
            puts("sum mismatch");
            exit(1);
        }
    }

    puts("pthread ok");
    return 0;
}
