#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_FILES 32
#define MAX_TOTAL_SIZE (200LL * 1024LL * 1024LL)
#define ORG_SIZE_FIELD 10
#define DEFAULT_ARCHIVE "a.sau"
#define BUF_SIZE 8192
#define MAX_NAME_LEN 255

typedef struct {
    char name[MAX_NAME_LEN + 1];
    char path[PATH_MAX];
    mode_t permission;
    long long size;
} ArchiveEntry;

static const char *get_file_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int is_regular_file(const char *path, struct stat *st) {
    if (stat(path, st) != 0) {
        return 0;
    }
    return S_ISREG(st->st_mode);
}

static int is_valid_archive_name(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".sau") == 0;
}

static int is_valid_stored_name(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;
    if (strlen(name) > MAX_NAME_LEN) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    if (strchr(name, ',') != NULL || strchr(name, '|') != NULL) return 0;
    return 1;
}

static int is_ascii_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    unsigned char buffer[BUF_SIZE];
    size_t read_count;
    while ((read_count = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        for (size_t i = 0; i < read_count; ++i) {
            if (buffer[i] == 0 || buffer[i] > 127) {
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

static int copy_file_content(FILE *archive, const char *path) {
    FILE *input = fopen(path, "rb");
    if (input == NULL) {
        return 0;
    }

    unsigned char buffer[BUF_SIZE];
    size_t read_count;
    while ((read_count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (fwrite(buffer, 1, read_count, archive) != read_count) {
            fclose(input);
            return 0;
        }
    }

    int ok = !ferror(input);
    fclose(input);
    return ok;
}

static int append_text(char **text, size_t *capacity, size_t *length, const char *part) {
    size_t part_len = strlen(part);
    if (*length + part_len + 1 > *capacity) {
        while (*length + part_len + 1 > *capacity) {
            *capacity *= 2;
        }
        char *new_text = realloc(*text, *capacity);
        if (new_text == NULL) {
            return 0;
        }
        *text = new_text;
    }

    memcpy(*text + *length, part, part_len);
    *length += part_len;
    (*text)[*length] = '\0';
    return 1;
}

static void make_size_field(char field[ORG_SIZE_FIELD + 1], long long value) {
    field[ORG_SIZE_FIELD] = '\0';
    for (int i = ORG_SIZE_FIELD - 1; i >= 0; --i) {
        field[i] = (char)('0' + (value % 10));
        value /= 10;
    }
}

static int create_archive(int argc, char **argv) {
    ArchiveEntry entries[MAX_FILES];
    int file_count = 0;
    long long total_size = 0;
    const char *archive_name = DEFAULT_ARCHIVE;

    if (argc < 3) {
        fprintf(stderr, "Kullanım: tarsau -b dosya1 [dosya2 ...] [-o arşiv.sau]\n");
        return 1;
    }

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-o parametresinden sonra arşiv dosyası verilmelidir.\n");
                return 1;
            }
            archive_name = argv[i + 1];
            ++i;
            continue;
        }

        if (file_count >= MAX_FILES) {
            fprintf(stderr, "Giriş dosyası sayısı en fazla 32 olabilir.\n");
            return 1;
        }

        struct stat st;
        if (!is_regular_file(argv[i], &st)) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n", argv[i]);
            return 1;
        }

        const char *stored_name = get_file_name(argv[i]);
        if (!is_valid_stored_name(stored_name) || !is_ascii_text_file(argv[i])) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n", argv[i]);
            return 1;
        }

        if (total_size + (long long)st.st_size > MAX_TOTAL_SIZE) {
            fprintf(stderr, "Giriş dosyalarının toplam boyutu 200 MB'ı geçemez.\n");
            return 1;
        }

        strncpy(entries[file_count].name, stored_name, MAX_NAME_LEN);
        entries[file_count].name[MAX_NAME_LEN] = '\0';
        strncpy(entries[file_count].path, argv[i], PATH_MAX - 1);
        entries[file_count].path[PATH_MAX - 1] = '\0';
        entries[file_count].permission = st.st_mode & 0777;
        entries[file_count].size = (long long)st.st_size;
        total_size += entries[file_count].size;
        ++file_count;
    }

    if (file_count == 0) {
        fprintf(stderr, "En az bir giriş dosyası verilmelidir.\n");
        return 1;
    }

    if (!is_valid_archive_name(archive_name)) {
        fprintf(stderr, "Arşiv dosya adı .sau uzantılı olmalıdır.\n");
        return 1;
    }

    size_t capacity = 1024;
    size_t length = 0;
    char *metadata = malloc(capacity);
    if (metadata == NULL) {
        return 1;
    }
    metadata[0] = '\0';

    for (int i = 0; i < file_count; ++i) {
        size_t record_capacity = strlen(entries[i].name) + 64;
        char *record = malloc(record_capacity);
        if (record == NULL) {
            free(metadata);
            return 1;
        }

        int n = snprintf(record, record_capacity, "|%s,%04o,%lld",
                         entries[i].name,
                         (unsigned int)entries[i].permission,
                         entries[i].size);
        if (n < 0 || (size_t)n >= record_capacity ||
            !append_text(&metadata, &capacity, &length, record)) {
            free(record);
            free(metadata);
            return 1;
        }
        free(record);
    }

    if (!append_text(&metadata, &capacity, &length, "|")) {
        free(metadata);
        return 1;
    }

    long long organization_size = ORG_SIZE_FIELD + (long long)length;
    char size_area[ORG_SIZE_FIELD + 1];
    make_size_field(size_area, organization_size);

    FILE *archive = fopen(archive_name, "wb");
    if (archive == NULL) {
        free(metadata);
        perror("Arşiv dosyası oluşturulamadı");
        return 1;
    }

    if (fwrite(size_area, 1, ORG_SIZE_FIELD, archive) != ORG_SIZE_FIELD ||
        fwrite(metadata, 1, length, archive) != length) {
        fclose(archive);
        free(metadata);
        fprintf(stderr, "Organizasyon bölümü yazılamadı.\n");
        return 1;
    }
    free(metadata);

    for (int i = 0; i < file_count; ++i) {
        if (!copy_file_content(archive, entries[i].path)) {
            fclose(archive);
            fprintf(stderr, "%s arşive eklenemedi.\n", entries[i].path);
            return 1;
        }
    }

    fclose(archive);
    printf("Dosyalar birleştirildi.\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Kullanım: tarsau -b dosya1 [dosya2 ...] [-o arşiv.sau]\n");
        return 1;
    }

    if (strcmp(argv[1], "-b") == 0) {
        return create_archive(argc, argv);
    }

    fprintf(stderr, "Bu aşamada yalnızca -b arşiv oluşturma işlemi desteklenir.\n");
    return 1;
}
