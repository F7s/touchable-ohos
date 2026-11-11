// Reference-based ZIP extraction for the HarmonyOS data-pack import.
//
// The app opens the picked .zip with fileIo.openSync(uri) and hands the file descriptor to this code, so the
// archive is never copied into the app sandbox: entries are read straight from the file descriptor with pread()
// and written to the sandbox destination. zlib.decompressFile() cannot be used here because it only accepts a
// sandbox path (and requires a .zip suffix), while a picker URI has neither.
//
// Supported: stored (method 0) and deflate (method 8) entries, UTF-8 and CP437-ish names (bytes copied as-is),
// explicit directory entries, data descriptors (sizes are taken from the central directory). Not supported:
// encrypted entries and ZIP64 (returns a clear error).
//
// This file is also built as a host test binary (UF_HOST_TEST) so the extractor can be verified against the same
// archives the app consumes, without a device:
//   cc -O2 -DUF_HOST_TEST unzip_fd.c -lz -o /tmp/uf_test && /tmp/uf_test pack.zip /tmp/out
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <zlib.h>

#define UF_PATH_MAX 4096
#define UF_CHUNK (1u << 20)

enum {
    UF_OK = 0,
    UF_E_IO = -1,          // read/write/seek failure
    UF_E_FORMAT = -2,      // not a zip, damaged central directory
    UF_E_UNSUPPORTED = -3, // zip64, encryption, compression method
    UF_E_UNSAFE = -4,      // path traversal / absolute entry name
    UF_E_SPACE = -5,       // write failed, likely out of space
    UF_E_CRC = -6,         // decompressed data does not match the central directory
};

static void uf_why(char *why, size_t n, const char *fmt, ...) {
    if (!why || n == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static int pread_all(int fd, void *buf, size_t n, uint64_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        put += (size_t)w;
    }
    return 0;
}

static int mkdir_p(const char *dir) {
    char tmp[UF_PATH_MAX];
    size_t len = strlen(dir);
    if (len == 0 || len >= sizeof tmp) return -1;
    memcpy(tmp, dir, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

typedef struct {
    uint64_t file_size;
    uint64_t cd_off;
    uint64_t cd_size;
    uint32_t count;
} uf_dir;

// Read the end-of-central-directory record and the central directory. `cd` is malloc'ed and must be freed.
static int uf_read_dir(int fd, uf_dir *d, unsigned char **cd, char *why, size_t why_n) {
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 22) {
        uf_why(why, why_n, "not a zip file");
        return UF_E_FORMAT;
    }
    d->file_size = (uint64_t)st.st_size;
    uint64_t tail_off = d->file_size > 65557 ? d->file_size - 65557 : 0;
    size_t tail_len = (size_t)(d->file_size - tail_off);
    unsigned char *tail = (unsigned char *)malloc(tail_len);
    if (!tail) return UF_E_IO;
    if (pread_all(fd, tail, tail_len, tail_off) != 0) {
        free(tail);
        uf_why(why, why_n, "cannot read the end of the archive");
        return UF_E_IO;
    }
    ssize_t eocd = -1;
    for (ssize_t i = (ssize_t)tail_len - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(tail);
        uf_why(why, why_n, "no zip end record (not a zip, or truncated)");
        return UF_E_FORMAT;
    }
    uint32_t count = rd16(tail + eocd + 10);
    uint64_t cd_size = rd32(tail + eocd + 12);
    uint64_t cd_off = rd32(tail + eocd + 16);
    free(tail);
    if (count == 0xFFFFu || cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
        uf_why(why, why_n, "ZIP64 archives are not supported");
        return UF_E_UNSUPPORTED;
    }
    if (cd_off + cd_size > d->file_size || cd_size > (64u << 20)) {
        uf_why(why, why_n, "central directory is outside the file");
        return UF_E_FORMAT;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)cd_size);
    if (!buf) return UF_E_IO;
    if (pread_all(fd, buf, (size_t)cd_size, cd_off) != 0) {
        free(buf);
        uf_why(why, why_n, "cannot read the central directory");
        return UF_E_IO;
    }
    d->cd_off = cd_off;
    d->cd_size = cd_size;
    d->count = count;
    *cd = buf;
    return UF_OK;
}

// Walk the central directory. Returns 0 and fills the entry; advances *pos. Returns 1 at the end.
// `name` points into `cd` (not NUL-terminated).
typedef struct {
    const unsigned char *name;
    uint32_t name_len;
    uint32_t flags;
    uint32_t method;
    uint32_t crc;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t local_off;
} uf_entry;
// Python's zipfile (and others) switch an entry to a ZIP64 extra field once a local-header offset or a size
// passes 2^31-1, which happens in a >2 GB pack even when the archive as a whole is not ZIP64: the base field is
// 0xFFFFFFFF and the real value is in extra id 0x0001, in the order uncompressed, compressed, offset, disk.
static void uf_zip64_extra(const unsigned char *extra, uint32_t extra_len, uf_entry *e) {
    uint32_t p = 0;
    while (p + 4 <= extra_len) {
        uint16_t id = rd16(extra + p);
        uint16_t sz = rd16(extra + p + 2);
        if (p + 4u + sz > extra_len) return;
        if (id == 0x0001) {
            const unsigned char *q = extra + p + 4;
            uint32_t left = sz;
            if (e->uncomp_size == 0xFFFFFFFFu && left >= 8) {
                e->uncomp_size = rd64(q); q += 8; left -= 8;
            }
            if (e->comp_size == 0xFFFFFFFFu && left >= 8) {
                e->comp_size = rd64(q); q += 8; left -= 8;
            }
            if (e->local_off == 0xFFFFFFFFu && left >= 8) {
                e->local_off = rd64(q); q += 8; left -= 8;
            }
            return;
        }
        p += 4u + sz;
    }
}


static int uf_next(const unsigned char *cd, uint64_t cd_size, uint64_t *pos, uf_entry *e) {
    if (*pos + 46 > cd_size) return 1;
    const unsigned char *p = cd + *pos;
    if (rd32(p) != 0x02014b50u) return 2;   // damaged central directory
    e->flags = rd16(p + 8);
    e->method = rd16(p + 10);
    e->crc = rd32(p + 16);
    e->comp_size = rd32(p + 20);
    e->uncomp_size = rd32(p + 24);
    e->name_len = rd16(p + 28);
    uint32_t extra_len = rd16(p + 30);
    uint32_t comment_len = rd16(p + 32);
    e->local_off = rd32(p + 42);
    if (*pos + 46 + e->name_len + extra_len + comment_len > cd_size) return 2;
    e->name = p + 46;
    uf_zip64_extra(p + 46 + e->name_len, extra_len, e);
    *pos += 46u + e->name_len + extra_len + comment_len;
    return 0;
}

// 0 = safe, -1 = unsafe. Converts '\' to '/', rejects absolute paths, drive letters and ".." components.
static int uf_safe_name(char *name) {
    for (char *p = name; *p; p++) {
        if (*p == '\\') *p = '/';
        if ((unsigned char)*p < 0x20) return -1;
    }
    if (name[0] == '/' || name[0] == 0) return -1;
    if (name[1] == ':') return -1;
    const char *seg = name;
    for (const char *p = name;; p++) {
        if (*p == '/' || *p == 0) {
            size_t n = (size_t)(p - seg);
            if (n == 0) return -1;   // double slash
            if (n == 2 && seg[0] == '.' && seg[1] == '.') return -1;
            if (n == 1 && seg[0] == '.') return -1;
            if (*p == 0) break;
            seg = p + 1;
        }
    }
    return 0;
}

// Sum of the uncompressed sizes in the central directory, so the caller can check free space first.
int uf_zip_info(int fd, uint64_t *unpacked, uint32_t *entries, char *why, size_t why_n) {
    uf_dir d;
    unsigned char *cd = NULL;
    int rc = uf_read_dir(fd, &d, &cd, why, why_n);
    if (rc != UF_OK) return rc;
    uint64_t total = 0;
    uint32_t n = 0;
    uint64_t pos = 0;
    uf_entry e;
    for (;;) {
        int r = uf_next(cd, d.cd_size, &pos, &e);
        if (r == 1) break;
        if (r == 2) {
            free(cd);
            uf_why(why, why_n, "damaged central directory");
            return UF_E_FORMAT;
        }
        n++;
        total += e.uncomp_size;
    }
    free(cd);
    if (unpacked) *unpacked = total;
    if (entries) *entries = n;
    if (why && why_n) why[0] = 0;
    return UF_OK;
}

// Extract every file in the archive to out_dir. *done is updated with __atomic_add_fetch so a poller on
// another thread can report progress. total is the value uf_zip_info() returned (0 = unknown).
int uf_extract(int fd, const char *out_dir, volatile uint64_t *done, uint64_t total, char *why, size_t why_n) {
    (void)total;   // progress readers use the central-directory sum from uf_zip_info() instead
    uf_dir d;
    unsigned char *cd = NULL;
    int rc = uf_read_dir(fd, &d, &cd, why, why_n);
    if (rc != UF_OK) return rc;
    uint64_t pos = 0;
    uf_entry e;
    unsigned char *in = (unsigned char *)malloc(UF_CHUNK);
    unsigned char *out = (unsigned char *)malloc(UF_CHUNK);
    char name[UF_PATH_MAX];
    char path[UF_PATH_MAX + 32];
    if (!in || !out) {
        free(in);
        free(out);
        free(cd);
        uf_why(why, why_n, "out of memory");
        return UF_E_IO;
    }
    int result = UF_OK;
    for (;;) {
        int r = uf_next(cd, d.cd_size, &pos, &e);
        if (r == 1) break;
        if (r == 2) {
            result = UF_E_FORMAT;
            uf_why(why, why_n, "damaged central directory");
            break;
        }
        if (e.name_len == 0 || e.name_len + 1 >= sizeof name) {
            result = UF_E_FORMAT;
            uf_why(why, why_n, "bad entry name length %u", e.name_len);
            break;
        }
        memcpy(name, e.name, e.name_len);
        name[e.name_len] = 0;
        int is_dir = name[e.name_len - 1] == '/';
        if (is_dir) name[e.name_len - 1] = 0;   // validate/create without the trailing slash
        if (uf_safe_name(name) != 0) {
            result = UF_E_UNSAFE;
            uf_why(why, why_n, "unsafe entry name: %s", name);
            break;
        }
        snprintf(path, sizeof path, "%s/%s", out_dir, name);
        if (is_dir) {
            if (mkdir_p(path) != 0) {
                result = UF_E_SPACE;
                uf_why(why, why_n, "cannot create %s: %s", path, strerror(errno));
                break;
            }
            continue;
        }
        if (e.flags & 1u) {
            result = UF_E_UNSUPPORTED;
            uf_why(why, why_n, "encrypted entry: %s", name);
            break;
        }
        if (e.method != 0 && e.method != 8) {
            result = UF_E_UNSUPPORTED;
            uf_why(why, why_n, "unsupported compression method %u: %s", e.method, name);
            break;
        }
        // The local header's name/extra lengths give the data offset (they may differ from the central ones).
        unsigned char lh[30];
        int prc = pread_all(fd, lh, sizeof lh, e.local_off);
        if (prc != 0 || rd32(lh) != 0x04034b50u) {
            result = UF_E_FORMAT;
            uf_why(why, why_n, "bad local header: %s off=%llu errno=%d sig=%02x%02x%02x%02x",
                   name, (unsigned long long)e.local_off, errno, lh[0], lh[1], lh[2], lh[3]);
            break;
        }
        uint64_t data_off = e.local_off + 30u + rd16(lh + 26) + rd16(lh + 28);
        if (data_off + e.comp_size > d.file_size) {
            result = UF_E_FORMAT;
            uf_why(why, why_n, "entry data outside the file: %s", name);
            break;
        }
        // Create the parent directory and the file.
        char dirpath[UF_PATH_MAX + 32];
        memcpy(dirpath, path, strlen(path) + 1);
        char *slash = strrchr(dirpath, '/');
        if (slash) {
            *slash = 0;
            if (mkdir_p(dirpath) != 0) {
                result = UF_E_SPACE;
                uf_why(why, why_n, "cannot create %s: %s", dirpath, strerror(errno));
                break;
            }
        }
        int ofd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) {
            result = UF_E_SPACE;
            uf_why(why, why_n, "cannot create %s: %s", path, strerror(errno));
            break;
        }
        uint32_t crc = crc32(0, Z_NULL, 0);
        if (e.method == 0) {
            uint64_t left = e.comp_size;
            uint64_t off = data_off;
            while (left > 0) {
                size_t want = left > UF_CHUNK ? UF_CHUNK : (size_t)left;
                if (pread_all(fd, in, want, off) != 0 || write_all(ofd, in, want) != 0) {
                    result = UF_E_SPACE;
                    uf_why(why, why_n, "write failed: %s: %s", path, strerror(errno));
                    break;
                }
                crc = crc32(crc, in, (uInt)want);
                off += want;
                left -= want;
                __atomic_add_fetch(done, want, __ATOMIC_RELAXED);
            }
        } else {
            z_stream zs;
            memset(&zs, 0, sizeof zs);
            if (inflateInit2(&zs, -15) != Z_OK) {
                close(ofd);
                result = UF_E_IO;
                uf_why(why, why_n, "zlib init failed: %s", name);
                break;
            }
            uint64_t left = e.comp_size;
            uint64_t off = data_off;
            int zrc = Z_OK;
            while (left > 0 && zrc != Z_STREAM_END) {
                size_t want = left > UF_CHUNK ? UF_CHUNK : (size_t)left;
                if (pread_all(fd, in, want, off) != 0) {
                    zrc = Z_ERRNO;
                    break;
                }
                off += want;
                left -= want;
                zs.next_in = in;
                zs.avail_in = (uInt)want;
                while (zs.avail_in > 0 && zrc != Z_STREAM_END) {
                    zs.next_out = out;
                    zs.avail_out = UF_CHUNK;
                    zrc = inflate(&zs, Z_NO_FLUSH);
                    if (zrc != Z_OK && zrc != Z_STREAM_END && zrc != Z_BUF_ERROR) break;
                    size_t got = UF_CHUNK - zs.avail_out;
                    if (got > 0) {
                        if (write_all(ofd, out, got) != 0) {
                            zrc = Z_ERRNO;
                            break;
                        }
                        crc = crc32(crc, out, (uInt)got);
                        __atomic_add_fetch(done, got, __ATOMIC_RELAXED);
                    }
                    if (zrc == Z_BUF_ERROR) break;
                }
                if (zrc == Z_ERRNO) break;
            }
            inflateEnd(&zs);
            if (zrc != Z_STREAM_END) {
                close(ofd);
                result = UF_E_FORMAT;
                uf_why(why, why_n, "deflate stream damaged: %s", name);
                break;
            }
        }
        close(ofd);
        if (result != UF_OK) break;
        // The central directory is authoritative for the sizes and the CRC.
        struct stat ost;
        if (stat(path, &ost) != 0 || (uint64_t)ost.st_size != e.uncomp_size) {
            result = UF_E_FORMAT;
            uf_why(why, why_n, "size mismatch: %s", name);
            break;
        }
        if (e.crc != 0 && crc != e.crc) {
            result = UF_E_CRC;
            uf_why(why, why_n, "CRC mismatch: %s", name);
            break;
        }
    }
    free(in);
    free(out);
    free(cd);
    if (result == UF_OK && why && why_n) why[0] = 0;
    return result;
}

#ifdef UF_HOST_TEST
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <zip> <out dir>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }
    char why[512] = {0};
    uint64_t unpacked = 0;
    uint32_t entries = 0;
    int rc = uf_zip_info(fd, &unpacked, &entries, why, sizeof why);
    printf("info rc=%d unpacked=%llu entries=%u why=%s\n", rc, (unsigned long long)unpacked, entries, why);
    if (rc != 0) return 1;
    volatile uint64_t done = 0;
    rc = uf_extract(fd, argv[2], &done, unpacked, why, sizeof why);
    printf("extract rc=%d done=%llu/%llu why=%s\n", rc, (unsigned long long)done, (unsigned long long)unpacked, why);
    close(fd);
    return rc == 0 ? 0 : 1;
}
#endif
