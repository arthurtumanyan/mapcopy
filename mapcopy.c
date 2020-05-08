#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/sendfile.h>
#include <ctype.h>
#include <pthread.h>

struct timeval start, end, diff;
int gen_flag = 0;
int stop_flag = 0;
int limit_flag = 0;
long bytes_rest = 0;
long limit = 0;
int use_sendfile = 0;
int parallel_flag = 0;
int verifiy_flag = 0;
int debug_flag = 0;

int parallels = 0;
int p = 0;
void * dst_base[4];
void * src_base[4];
off_t pa_offset, offset;
size_t length[4];
int fdin, fdout;
void *src, *dst;
struct stat statbuf;
mode_t mode = 0666;

typedef void Sigfunc(int);

struct thread_data {
    void *src;
    void *dst;
    size_t len;
    int id;
};

struct thread_data thread_data_array[4];

Sigfunc * signal(int signo, Sigfunc *func) {
    struct sigaction act, oact;
    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (signo == SIGALRM) {
        act.sa_flags |= SA_INTERRUPT; /* SunOS 4.x */
    } else {
        act.sa_flags |= SA_RESTART; /* SVR4, 44BSD */
    }
    if (sigaction(signo, &act, &oact) < 0)
        return (SIG_ERR);
    return (oact.sa_handler);
}

Sigfunc * Signal(int signo, Sigfunc *func) {
    Sigfunc *sigfunc;
    if ((sigfunc = signal(signo, func)) == SIG_ERR) {
        perror("signal");
    }
    return (sigfunc);
}

void usage_exit() {
    printf("\tUsage: mapcopy --source   | -s <filename> --destination | -d <filename> --sendfile | -x --debug | -D\n"
            "\t       mapcopy --source   | -s <filename> --destination | -d <filename> --parallel | -p --debug | -D\n"
            "\t       mapcopy --source   | -s <filename> --verify | -v\n"
            "\t       mapcopy --generate | -g            --destination | -d <filename> --size | -S <bytes> --debug | -D\n\n");
    exit(EXIT_SUCCESS);
}

int timeval_subtract(struct timeval *result, struct timeval *t2, struct timeval *t1) {
    long int diff = (t2->tv_usec + 1000000 * t2->tv_sec) - (t1->tv_usec + 1000000 * t1->tv_sec);
    result->tv_sec = diff / 1000000;
    result->tv_usec = diff % 1000000;

    return (diff < 0);
}

void print_output() {
    timeval_subtract(&diff, &end, &start);
    if (0 > bytes_rest) {
        bytes_rest = 0;
    }
    printf("Limit:  %ld bytes (%ld KB)\n", limit, limit / 1024);
    long processed_bytes = limit - bytes_rest;
    printf("Copied: %ld bytes (%ld KB)\n", processed_bytes, processed_bytes / 1024);
    printf("Rest:   %ld bytes (%ld KB)\n", bytes_rest, bytes_rest / 1024);
    printf("Data processing took %ld.%03ld seconds\n", diff.tv_sec, diff.tv_usec);
    // Avoiding division by zero
    if (diff.tv_sec) {
        long speed = processed_bytes / diff.tv_sec + 0.00000001;
        printf("Speed (approx): %ld  bytes/sec (%ld MBytes/sec)\n", speed, speed / 1024 / 1024);
    }
}

void signal_int(int sig) {
    gettimeofday(&end, NULL);
    if (debug_flag) {
        printf("Signal %d received\n", sig);
    }
    stop_flag++;
    if (gen_flag) {
        print_output();
    }
    exit(EXIT_FAILURE);
}

ssize_t do_sendfile(int out_fd, int in_fd, off_t offset, size_t count) {
    ssize_t bytes_sent;
    size_t total_bytes_sent = 0;
    while (total_bytes_sent < count) {
        if ((bytes_sent = sendfile(out_fd, in_fd, &offset,
                count - total_bytes_sent)) <= 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            printf("Error: %s\n", strerror(errno));
            return -1;
        }
        total_bytes_sent += bytes_sent;
    }
    return total_bytes_sent;
}

void do_verify(char *filename) {
    unsigned len = 0;
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (fstat(fd, &statbuf) < 0) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    char * p = mmap(0, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (close(fd) == -1) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    long char_cnt = 0;

    for (len = 0; len < statbuf.st_size; len++) {
        if (isprint(p[len])) {
            char_cnt++;
        }
    }

    printf("Size:  %ld\nChars: %ld\n\n", statbuf.st_size, char_cnt);

    if (munmap(p, statbuf.st_size) == -1) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void *do_memcopy(void *arg) {
    struct thread_data *data;
    data = (struct thread_data *) arg;
    if (debug_flag) {
        printf(">> %d: src: %p dst: %p length: %ld\n", data->id, data->dst, data->src, data->len);
    }
    memcpy(data->dst, data->src, data->len);
    if (errno == 0) {
        if (debug_flag) {
            printf("Thread: %d: src: %p dst: %p length: %ld (%ld MBytes)\n", data->id, data->dst, data->src, data->len, (data->len / 1024 / 1024));
        }
    } else {
        printf("Thread %d: Error: %s\n", data->id, strerror(errno));
    }
    return NULL;
}

int main(int argc, char *argv[]) {

    static struct option long_options[] = {
        {"source", required_argument, 0, 's'},
        {"destination", required_argument, 0, 'd'},
        {"generate", optional_argument, 0, 'g'},
        {"size", required_argument, 0, 'S'},
        {"sendfile", optional_argument, 0, 'x'},
        {"parallel", required_argument, 0, 'p'},
        {"verify", optional_argument, 0, 'v'},
        {"debug", optional_argument, 0, 'D'},
        {0, 0, 0, 0}
    };

    int c;
    char *source = NULL;
    char *destination = NULL;
    pthread_t threads[4];
    int rc;

    while (1) {

        int option_index = 0;

        c = getopt_long(argc, argv, "gs:d:S:xp:vD",
                long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c) {
            case 's':
                source = optarg;
                break;

            case 'd':
                destination = optarg;
                break;

            case 'g':
                gen_flag++;
                break;

            case 'x':
                use_sendfile++;
                break;

            case 'v':
                verifiy_flag++;
                break;

            case 'D':
                debug_flag++;
                break;

            case 'p':
                parallel_flag++;
                parallels = atoi(optarg);
                if (0 > parallels || 4 < parallels) {
                    parallels = 4;
                    printf("Threads count adjusted to %d\n", parallels);
                }
                break;

            case 'S':
                limit = atol(optarg);
                if (0 > limit) {
                    printf("Negative value for output destination file not supported\n");
                    exit(EXIT_FAILURE);
                }
                bytes_rest = limit;
                limit_flag++;
                break;

            default:
                usage_exit();
        }
    }
    Signal(SIGINT, signal_int);

    if (verifiy_flag && source) {
        do_verify(source);
        exit(EXIT_SUCCESS);
    }

    if (!gen_flag && (source == NULL || destination == NULL)) {
        usage_exit();
    }

    if (!gen_flag && (0 == strcmp(source, destination))) {
        printf("Nothing to do\n");
        exit(EXIT_FAILURE);
    }

    if (gen_flag && source) {
        usage_exit();
    }

    if (parallel_flag && use_sendfile) {
        usage_exit();
    }

    if (gen_flag && parallel_flag) {
        usage_exit();
    }

    if (gen_flag && use_sendfile) {
        usage_exit();
    }

    if (gen_flag && !limit_flag) {
        printf("Destination file size not specified\n");
        usage_exit();
    }

    if (!gen_flag) {
        if ((fdin = open(source, O_RDONLY)) < 0) {
            printf("Can't open %s for reading (%s)\n", source, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if ((fdout = open(destination, O_RDWR | O_CREAT, mode)) < 0) {
        printf("Can't open %s for writing (%s)\n", destination, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!gen_flag) {
        if (fstat(fdin, &statbuf) < 0) {
            printf("Error: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (lseek(fdout, (!gen_flag) ? statbuf.st_size - 1 : limit - 1, SEEK_SET) == -1) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (write(fdout, "", 1) != 1) {
        printf("Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }


    if (!gen_flag && !use_sendfile) {
        if (!parallel_flag) {
            if ((src = mmap(0, statbuf.st_size, PROT_READ, MAP_SHARED, fdin, 0)) == MAP_FAILED) {
                printf("Unable to mmap: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        } else {
            for (p = 0; p < parallels; p++) {
                if (!p) {
                    offset = 0;
                } else {
                    offset = (p * (statbuf.st_size / parallels));
                }
                pa_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
                length[p] = (ssize_t) (statbuf.st_size - pa_offset);
                if ((src_base[p] = mmap(0, length[p], PROT_READ, MAP_SHARED, fdin, pa_offset)) == MAP_FAILED) {
                    printf("Unable to mmap: %s\n", strerror(errno));
                    exit(EXIT_FAILURE);
                }
                if (debug_flag) {
                    printf("Src [%d] address: %p\n", p, src_base[p]);
                }
            }
        }
    }
    printf("\n");
    if (!use_sendfile) {
        if (!parallel_flag) {
            if ((dst = mmap(0, (!gen_flag) ? statbuf.st_size : limit, PROT_READ | PROT_WRITE, MAP_SHARED, fdout, 0)) == MAP_FAILED) {
                printf("Unable to mmap: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        } else {
            for (p = 0; p < parallels; p++) {
                if (!p) {
                    offset = 0;
                } else {
                    offset = (p * (statbuf.st_size / parallels));
                }
                pa_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
                length[p] = (ssize_t) (statbuf.st_size - pa_offset);
                if ((dst_base[p] = mmap(0, length[p], PROT_READ | PROT_WRITE, MAP_SHARED, fdout, pa_offset)) == MAP_FAILED) {
                    printf("Unable to mmap: %s\n", strerror(errno));
                    exit(EXIT_FAILURE);
                }
                if (debug_flag) {
                    printf("Dst [%d] address: %p\n", p, dst_base[p]);
                }
            }
        }
    }
    printf("\n");
    if (!parallel_flag) {
        gettimeofday(&start, NULL);
    }
    if (!gen_flag && !stop_flag) {
        limit = statbuf.st_size;
        if (!use_sendfile) {
            if (!parallel_flag) {
                memcpy(dst, src, statbuf.st_size);
            } else {
                for (p = 0; p < parallels; p++) {
                    /* spawn the threads */
                    thread_data_array[p].id = p;
                    thread_data_array[p].src = src_base[p];
                    thread_data_array[p].dst = dst_base[p];
                    thread_data_array[p].len = length[p];

                    if (0 > (rc = pthread_create(&threads[p], NULL, do_memcopy, (void *) &thread_data_array[p]))) {
                        printf("Error: %s\n", strerror(errno));
                    } else {
                        if (debug_flag) {
                            printf("Spawning thread %d\n", p);
                            printf("Passing arguments: id = %d, src = %p, dst = %p, len = %ld (%ld MBytes)\n",
                                    thread_data_array[p].id,
                                    thread_data_array[p].src,
                                    thread_data_array[p].dst,
                                    thread_data_array[p].len,
                                    thread_data_array[p].len / 1024 / 1024);
                        }
                    }
                }
                /* wait for threads to finish */
                for (p = 0; p < parallels; p++) {
                    if (0 > (rc = pthread_join(threads[p], NULL))) {
                        printf("Error: %s\n", strerror(errno));
                    } else {
                        if (debug_flag) {
                            printf("Thread %d joined\n", p);
                        }
                    }
                }
                printf("\n");
            }
        } else {
            off_t offset = 0;
            if (lseek(fdout, 0, SEEK_SET) == -1) {
                printf("Error: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            int rc = do_sendfile(fdout, fdin, offset, statbuf.st_size);
            if (rc == -1) {
                printf("Error: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        }
    } else {
        char *buf = "7";
        while (bytes_rest-- && !stop_flag) {
            memcpy(dst++, buf, 1);
        }
    }
    if (!parallel_flag) {
        gettimeofday(&end, NULL);
        print_output();
    }
    if (!gen_flag && !use_sendfile) {
        if (!parallel_flag) {
            if ((munmap(src, statbuf.st_size)) == -1) {
                printf("Error: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            if ((munmap(dst, statbuf.st_size) == -1)) {
                printf("Error: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
            close(fdin);
        }
    }

    close(fdout);
    exit(EXIT_SUCCESS);
}



