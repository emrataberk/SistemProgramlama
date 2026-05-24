#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_FILES 32
#define MAX_TOTAL_SIZE (200LL * 1024LL * 1024LL)
#define ORG_SIZE_FIELD 10
#define DEFAULT_ARCHIVE "a.sau"
#define COPY_BUF_SIZE 8192
#define MAX_NAME_LEN 255

typedef struct {
    char stored_name[MAX_NAME_LEN + 1];
    char source_path[PATH_MAX];
    mode_t perm;
    long long size;
} ArchiveEntry;

static int is_regular_file(const char *path, struct stat *st) {
    if (stat(path, st) != 0) return 0;
    return S_ISREG(st->st_mode);
}

static int has_sau_extension(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".sau") == 0;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int is_safe_stored_name(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;
    if (strlen(name) > MAX_NAME_LEN) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    if (strchr(name, ',') != NULL || strchr(name, '|') != NULL) return 0;
    return 1;
}

static int validate_ascii_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;

    unsigned char buf[COPY_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = buf[i];
            if (c == 0 || c > 127) {
                fclose(fp);
                return 0;
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
}

static int mkdir_p(const char *dir) {
    char tmp[PATH_MAX];
    size_t len;

    if (dir == NULL || dir[0] == '\0') return -1;
    len = strlen(dir);
    if (len >= sizeof(tmp)) return -1;

    strcpy(tmp, dir);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_exact_bytes(FILE *in, FILE *out, long long bytes) {
    unsigned char buf[COPY_BUF_SIZE];
    while (bytes > 0) {
        size_t want = bytes > (long long)sizeof(buf) ? sizeof(buf) : (size_t)bytes;
        size_t got = fread(buf, 1, want, in);
        if (got != want) return 0;
        if (fwrite(buf, 1, got, out) != got) return 0;
        bytes -= (long long)got;
    }
    return 1;
}

static int build_archive(int argc, char **argv) {
    ArchiveEntry entries[MAX_FILES];
    int file_count = 0;
    const char *out_name = DEFAULT_ARCHIVE;
    long long total_size = 0;

    if (argc < 3) {
        fprintf(stderr, "Kullanım: tarsau -b dosya1 [dosya2 ...] [-o arşiv.sau]\n");
        return 1;
    }

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-o parametresinden sonra arşiv dosya adı verilmelidir.\n");
                return 1;
            }
            out_name = argv[i + 1];
            ++i;
            continue;
        }

        if (file_count >= MAX_FILES) {
            fprintf(stderr, "Giriş dosyası sayısı en fazla 32 olabilir.\n");
            return 1;
        }

        const char *path = argv[i];
        struct stat st;
        if (!is_regular_file(path, &st)) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n", path);
            return 1;
        }

        const char *bn = base_name(path);
        if (!is_safe_stored_name(bn)) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n", path);
            return 1;
        }

        for (int j = 0; j < file_count; ++j) {
            if (strcmp(entries[j].stored_name, bn) == 0) {
                fprintf(stderr, "Aynı dosya adına sahip birden fazla giriş verilemez: %s\n", bn);
                return 1;
            }
        }

        if (!validate_ascii_text_file(path)) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n", path);
            return 1;
        }

        if ((long long)st.st_size < 0 || total_size + (long long)st.st_size > MAX_TOTAL_SIZE) {
            fprintf(stderr, "Giriş dosyalarının toplam boyutu 200 MB'ı geçemez.\n");
            return 1;
        }

        strncpy(entries[file_count].stored_name, bn, MAX_NAME_LEN);
        entries[file_count].stored_name[MAX_NAME_LEN] = '\0';
        strncpy(entries[file_count].source_path, path, PATH_MAX - 1);
        entries[file_count].source_path[PATH_MAX - 1] = '\0';
        entries[file_count].perm = st.st_mode & 0777;
        entries[file_count].size = (long long)st.st_size;
        total_size += (long long)st.st_size;
        ++file_count;
    }

    if (file_count == 0) {
        fprintf(stderr, "En az bir giriş dosyası verilmelidir.\n");
        return 1;
    }

    FILE *out = fopen(out_name, "wb");
    if (!out) {
        perror("Arşiv dosyası açılamadı");
        return 1;
    }

    size_t meta_cap = 1024;
    char *meta = malloc(meta_cap);
    if (!meta) {
        fclose(out);
        return 1;
    }
    meta[0] = '\0';
    size_t meta_len = 0;

    for (int i = 0; i < file_count; ++i) {
        char record[512];
        int written = snprintf(record, sizeof(record), "|%s,%04o,%lld",
                               entries[i].stored_name,
                               (unsigned int)entries[i].perm,
                               entries[i].size);
        if (written < 0 || written >= (int)sizeof(record)) {
            free(meta);
            fclose(out);
            fprintf(stderr, "Organizasyon kaydı oluşturulamadı.\n");
            return 1;
        }

        size_t rec_len = (size_t)written;
        if (meta_len + rec_len + 1 > meta_cap) {
            while (meta_len + rec_len + 1 > meta_cap) meta_cap *= 2;
            char *tmp = realloc(meta, meta_cap);
            if (!tmp) {
                free(meta);
                fclose(out);
                return 1;
            }
            meta = tmp;
        }
        memcpy(meta + meta_len, record, rec_len);
        meta_len += rec_len;
        meta[meta_len] = '\0';
    }

    if (meta_len + 2 > meta_cap) {
        char *tmp = realloc(meta, meta_cap + 2);
        if (!tmp) {
            free(meta);
            fclose(out);
            return 1;
        }
        meta = tmp;
        meta_cap += 2;
    }
    meta[meta_len++] = '|';
    meta[meta_len] = '\0';

    long long org_size = ORG_SIZE_FIELD + (long long)meta_len;
    if (org_size > 9999999999LL) {
        free(meta);
        fclose(out);
        fprintf(stderr, "Organizasyon bölümü çok büyük.\n");
        return 1;
    }

    char size_field[ORG_SIZE_FIELD + 1];
    long long org_tmp = org_size;
    size_field[ORG_SIZE_FIELD] = '\0';
    for (int i = ORG_SIZE_FIELD - 1; i >= 0; --i) {
        size_field[i] = (char)('0' + (org_tmp % 10));
        org_tmp /= 10;
    }
    if (fwrite(size_field, 1, ORG_SIZE_FIELD, out) != ORG_SIZE_FIELD ||
        fwrite(meta, 1, meta_len, out) != meta_len) {
        free(meta);
        fclose(out);
        perror("Arşiv yazılamadı");
        return 1;
    }
    free(meta);

    for (int i = 0; i < file_count; ++i) {
        FILE *in = fopen(entries[i].source_path, "rb");
        if (!in) {
            fclose(out);
            perror("Giriş dosyası açılamadı");
            return 1;
        }
        if (!copy_exact_bytes(in, out, entries[i].size)) {
            fclose(in);
            fclose(out);
            fprintf(stderr, "Dosya arşive yazılamadı: %s\n", entries[i].source_path);
            return 1;
        }
        fclose(in);
    }

    if (fclose(out) != 0) {
        perror("Arşiv kapatılamadı");
        return 1;
    }

    printf("Dosyalar birleştirildi.\n");
    return 0;
}

static int parse_nonnegative_ll(const char *s, long long *out) {
    if (!s || !*s) return 0;
    long long value = 0;
    for (const char *p = s; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return 0;
        int d = *p - '0';
        if (value > (LLONG_MAX - d) / 10) return 0;
        value = value * 10 + d;
    }
    *out = value;
    return 1;
}

static int parse_octal_mode(const char *s, mode_t *mode) {
    if (!s || !*s) return 0;
    unsigned int value = 0;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '7') return 0;
        value = (value * 8U) + (unsigned int)(*p - '0');
        if (value > 0777U) return 0;
    }
    *mode = (mode_t)value;
    return 1;
}

static int read_archive_entries(FILE *in, long long archive_size, ArchiveEntry *entries, int *file_count, long long *data_start) {
    char field[ORG_SIZE_FIELD + 1];
    if (fread(field, 1, ORG_SIZE_FIELD, in) != ORG_SIZE_FIELD) return 0;
    field[ORG_SIZE_FIELD] = '\0';
    for (int i = 0; i < ORG_SIZE_FIELD; ++i) {
        if (!isdigit((unsigned char)field[i])) return 0;
    }

    long long org_size;
    if (!parse_nonnegative_ll(field, &org_size)) return 0;
    if (org_size <= ORG_SIZE_FIELD || org_size > archive_size) return 0;

    long long meta_len_ll = org_size - ORG_SIZE_FIELD;
    if (meta_len_ll <= 0 || meta_len_ll > 1024LL * 1024LL) return 0;
    size_t meta_len = (size_t)meta_len_ll;
    char *meta = malloc(meta_len + 1);
    if (!meta) return 0;
    if (fread(meta, 1, meta_len, in) != meta_len) {
        free(meta);
        return 0;
    }
    meta[meta_len] = '\0';

    int count = 0;
    size_t pos = 0;
    if (meta_len < 3 || meta[0] != '|' || meta[meta_len - 1] != '|') {
        free(meta);
        return 0;
    }
    pos = 1;
    while (pos < meta_len) {
        size_t start = pos;
        while (pos < meta_len && meta[pos] != '|') ++pos;
        if (pos >= meta_len || pos == start) {
            free(meta);
            return 0;
        }
        meta[pos] = '\0';

        if (count >= MAX_FILES) {
            free(meta);
            return 0;
        }

        char *name = meta + start;
        char *perm_s = strchr(name, ',');
        if (!perm_s) { free(meta); return 0; }
        *perm_s++ = '\0';
        char *size_s = strchr(perm_s, ',');
        if (!size_s) { free(meta); return 0; }
        *size_s++ = '\0';
        if (strchr(size_s, ',') != NULL) { free(meta); return 0; }

        mode_t mode;
        long long fsize;
        if (!is_safe_stored_name(name) || !parse_octal_mode(perm_s, &mode) || !parse_nonnegative_ll(size_s, &fsize)) {
            free(meta);
            return 0;
        }
        for (int j = 0; j < count; ++j) {
            if (strcmp(entries[j].stored_name, name) == 0) {
                free(meta);
                return 0;
            }
        }

        strncpy(entries[count].stored_name, name, MAX_NAME_LEN);
        entries[count].stored_name[MAX_NAME_LEN] = '\0';
        entries[count].perm = mode;
        entries[count].size = fsize;
        entries[count].source_path[0] = '\0';
        ++count;
        ++pos;
    }

    long long total = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].size > archive_size || total > LLONG_MAX - entries[i].size) {
            free(meta);
            return 0;
        }
        total += entries[i].size;
    }
    if (org_size + total != archive_size) {
        free(meta);
        return 0;
    }

    free(meta);
    *file_count = count;
    *data_start = org_size;
    return count > 0;
}

static int extract_archive(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    const char *archive_name = argv[2];
    const char *dest_dir = (argc == 4) ? argv[3] : ".";

    if (!has_sau_extension(archive_name)) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    struct stat st;
    if (!is_regular_file(archive_name, &st) || st.st_size <= ORG_SIZE_FIELD) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    FILE *in = fopen(archive_name, "rb");
    if (!in) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    ArchiveEntry entries[MAX_FILES];
    int file_count = 0;
    long long data_start = 0;
    if (!read_archive_entries(in, (long long)st.st_size, entries, &file_count, &data_start)) {
        fclose(in);
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    if (strcmp(dest_dir, ".") != 0) {
        if (mkdir_p(dest_dir) != 0) {
            fclose(in);
            fprintf(stderr, "Dizin oluşturulamadı: %s\n", dest_dir);
            return 1;
        }
    }

    if (fseeko(in, (off_t)data_start, SEEK_SET) != 0) {
        fclose(in);
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    for (int i = 0; i < file_count; ++i) {
        char out_path[PATH_MAX];
        if (strcmp(dest_dir, ".") == 0) {
            if (snprintf(out_path, sizeof(out_path), "%s", entries[i].stored_name) >= (int)sizeof(out_path)) {
                fclose(in);
                fprintf(stderr, "Çıkış yolu çok uzun.\n");
                return 1;
            }
        } else {
            if (snprintf(out_path, sizeof(out_path), "%s/%s", dest_dir, entries[i].stored_name) >= (int)sizeof(out_path)) {
                fclose(in);
                fprintf(stderr, "Çıkış yolu çok uzun.\n");
                return 1;
            }
        }

        FILE *out = fopen(out_path, "wb");
        if (!out) {
            fclose(in);
            perror("Çıkış dosyası açılamadı");
            return 1;
        }
        if (!copy_exact_bytes(in, out, entries[i].size)) {
            fclose(out);
            fclose(in);
            printf("Arşiv dosyası uygunsuz veya bozuk!\n");
            return 1;
        }
        if (fclose(out) != 0) {
            fclose(in);
            perror("Çıkış dosyası kapatılamadı");
            return 1;
        }
        if (chmod(out_path, entries[i].perm) != 0) {
            fclose(in);
            perror("İzinler ayarlanamadı");
            return 1;
        }
    }

    fclose(in);
    if (strcmp(dest_dir, ".") == 0) {
        printf("Geçerli dizinde dosyalar açıldı.\n");
    } else {
        printf("%s dizininde dosyalar açıldı.\n", dest_dir);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Kullanım:\n  tarsau -b dosya1 [dosya2 ...] [-o arşiv.sau]\n  tarsau -a arşiv.sau [dizin]\n");
        return 1;
    }

    if (strcmp(argv[1], "-b") == 0) {
        return build_archive(argc, argv);
    }
    if (strcmp(argv[1], "-a") == 0) {
        return extract_archive(argc, argv);
    }

    fprintf(stderr, "Geçersiz parametre: %s\n", argv[1]);
    return 1;
}
