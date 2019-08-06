#include <pthread.h>
#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#    include "config.h"
#endif

#if HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "log.h"
#include "mfile.h"
#include "util.h"

const int              MAX_MMAP_SIZE  = 1 << 12;  // 4G
static int             curr_mmap_size = 0;
static pthread_mutex_t mmap_lock      = PTHREAD_MUTEX_INITIALIZER;

MFile* open_mfile(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        log_error("open mfile %s failed", path);
        return NULL;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        close(fd);
        return NULL;
    }
#if _XOPEN_SOURCE >= 600 || _POSIX_C_SOURCE >= 200112L
    posix_fadvise(fd, 0, sb.st_size, POSIX_FADV_SEQUENTIAL);
#endif

    pthread_mutex_lock(&mmap_lock);
    int mb = sb.st_size >> 20;
    while (curr_mmap_size + mb > MAX_MMAP_SIZE && mb > 100) {
        pthread_mutex_unlock(&mmap_lock);
        sleep(5);
        pthread_mutex_lock(&mmap_lock);
    }
    curr_mmap_size += mb;
    pthread_mutex_unlock(&mmap_lock);

    MFile* f = (MFile*)safe_malloc(sizeof(MFile));
    f->fd    = fd;
    f->size  = sb.st_size;

    if (f->size > 0) {
        f->addr = (char*)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (f->addr == MAP_FAILED) {
            log_error("mmap failed %s", path);
            close(fd);
            pthread_mutex_lock(&mmap_lock);
            curr_mmap_size -= mb;
            pthread_mutex_unlock(&mmap_lock);
            free(f);
            return NULL;
        }

        if (madvise(f->addr, sb.st_size, MADV_SEQUENTIAL) < 0) {
            log_error("Unable to madvise() region %p", f->addr);
        }
    }
    else {
        f->addr = NULL;
    }

    return f;
}

void close_mfile(MFile* f)
{
    if (f->addr) {
        madvise(f->addr, f->size, MADV_DONTNEED);
        munmap(f->addr, f->size);
    }
#if _XOPEN_SOURCE >= 600 || _POSIX_C_SOURCE >= 200112L
    posix_fadvise(f->fd, 0, f->size, POSIX_FADV_DONTNEED);
#endif
    close(f->fd);
    pthread_mutex_lock(&mmap_lock);
    curr_mmap_size -= f->size >> 20;
    pthread_mutex_unlock(&mmap_lock);
    free(f);
}
