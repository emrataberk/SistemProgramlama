#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
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
    char source_path[PATH_MAX];
    mode_t permission;
    long long size;
} ArchiveEntry;

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int is_sau_file(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".sau") == 0;
}

static int is_regular_file(const char *path, struct stat *st) {
    if (stat(path, st) != 0) return 0;
    return S_ISREG(st->st_mode);
}

static int is_stored_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;
    if (strlen(name) > MAX_NAME_LEN) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    if (strchr(name, ',') != NULL || strchr(name, '|') != NULL) return 0;
    return 1;
}

static int is_ascii_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return 0;

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

    int ok = !ferror(fp);
    fclose(fp);
    return ok;
}

static int mkdir_if_needed(const char *dir) {
    if (dir == NULL || dir[0] == '\0' || strcmp(dir, ".") == 0) {
        return 0;
    }

    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        return 0;
    }
    return 1;
}

static int copy_n_bytes(FILE *input, FILE *output, long long byte_count) {
    unsigned char buffer[BUF_SIZE];
    while (byte_count > 0) {
        size_t wanted = byte_count > (long long)sizeof(buffer) ? sizeof(buffer) : (size_t)byte_count;
        size_t read_count = fread(buffer, 1, wanted, input);
        if (read_count != wanted) return 0;
        if (fwrite(buffer, 1, read_count, output) != read_count) return 0;
        byte_count -= (long long)read_count;
    }
    return 1;
}

static int write_size_field(FILE *fp, long long value) {
    char field[ORG_SIZE_FIELD + 1];
    snprintf(field, sizeof(field), "%010lld", value);
    return fwrite(field, 1, ORG_SIZE_FIELD, fp) == ORG_SIZE_FIELD;
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
                fprintf(stderr, "-o parametresinden sonra arşiv dosya adı verilmelidir.\n");
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

        const char *stored_name = base_name(argv[i]);
        if (!is_stored_name_valid(stored_name) || !is_ascii_text_file(argv[i])) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n", argv[i]);
            return 1;
        }

        if (total_size + (long long)st.st_size > MAX_TOTAL_SIZE) {
            fprintf(stderr, "Giriş dosyalarının toplam boyutu 200 MB'ı geçemez.\n");
            return 1;
        }

        strncpy(entries[file_count].name, stored_name, MAX_NAME_LEN);
        entries[file_count].name[MAX_NAME_LEN] = '\0';
        strncpy(entries[file_count].source_path, argv[i], PATH_MAX - 1);
        entries[file_count].source_path[PATH_MAX - 1] = '\0';
        entries[file_count].permission = st.st_mode & 0777;
        entries[file_count].size = (long long)st.st_size;
        total_size += entries[file_count].size;
        ++file_count;
    }

    if (file_count == 0) {
        fprintf(stderr, "En az bir giriş dosyası verilmelidir.\n");
        return 1;
    }

    if (!is_sau_file(archive_name)) {
        fprintf(stderr, "Arşiv dosya adı .sau uzantılı olmalıdır.\n");
        return 1;
    }

    size_t metadata_capacity = 1024;
    size_t metadata_length = 0;
    char *metadata = malloc(metadata_capacity);
    if (metadata == NULL) return 1;
    metadata[0] = '\0';

    for (int i = 0; i < file_count; ++i) {
        char record[512];
        int n = snprintf(record, sizeof(record), "|%s,%04o,%lld",
                         entries[i].name,
                         (unsigned int)entries[i].permission,
                         entries[i].size);
        if (n < 0 || n >= (int)sizeof(record)) {
            free(metadata);
            return 1;
        }

        size_t record_len = (size_t)n;
        if (metadata_length + record_len + 2 > metadata_capacity) {
            metadata_capacity *= 2;
            char *tmp = realloc(metadata, metadata_capacity);
            if (tmp == NULL) {
                free(metadata);
                return 1;
            }
            metadata = tmp;
        }
        memcpy(metadata + metadata_length, record, record_len);
        metadata_length += record_len;
        metadata[metadata_length] = '\0';
    }

    if (metadata_length + 2 > metadata_capacity) {
        char *tmp = realloc(metadata, metadata_capacity + 2);
        if (tmp == NULL) {
            free(metadata);
            return 1;
        }
        metadata = tmp;
    }
    metadata[metadata_length++] = '|';
    metadata[metadata_length] = '\0';

    FILE *archive = fopen(archive_name, "wb");
    if (archive == NULL) {
        free(metadata);
        perror("Arşiv dosyası oluşturulamadı");
        return 1;
    }

    long long organization_size = ORG_SIZE_FIELD + (long long)metadata_length;
    if (!write_size_field(archive, organization_size) ||
        fwrite(metadata, 1, metadata_length, archive) != metadata_length) {
        fclose(archive);
        free(metadata);
        fprintf(stderr, "Arşiv başlığı yazılamadı.\n");
        return 1;
    }
    free(metadata);

    for (int i = 0; i < file_count; ++i) {
        FILE *input = fopen(entries[i].source_path, "rb");
        if (input == NULL) {
            fclose(archive);
            return 1;
        }
        int ok = copy_n_bytes(input, archive, entries[i].size);
        fclose(input);
        if (!ok) {
            fclose(archive);
            fprintf(stderr, "%s arşive yazılamadı.\n", entries[i].source_path);
            return 1;
        }
    }

    fclose(archive);
    printf("Dosyalar birleştirildi.\n");
    return 0;
}

static int parse_long_long(const char *text, long long *value) {
    if (text == NULL || text[0] == '\0') return 0;
    long long result = 0;
    for (const char *p = text; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return 0;
        result = result * 10 + (*p - '0');
    }
    *value = result;
    return 1;
}

static int read_archive_header(FILE *archive, long long archive_size,
                               ArchiveEntry *entries, int *file_count,
                               long long *data_start) {
    char field[ORG_SIZE_FIELD + 1];
    if (fread(field, 1, ORG_SIZE_FIELD, archive) != ORG_SIZE_FIELD) return 0;
    field[ORG_SIZE_FIELD] = '\0';

    for (int i = 0; i < ORG_SIZE_FIELD; ++i) {
        if (!isdigit((unsigned char)field[i])) return 0;
    }

    long long organization_size;
    if (!parse_long_long(field, &organization_size)) return 0;
    if (organization_size <= ORG_SIZE_FIELD || organization_size > archive_size) return 0;

    size_t metadata_length = (size_t)(organization_size - ORG_SIZE_FIELD);
    char *metadata = malloc(metadata_length + 1);
    if (metadata == NULL) return 0;

    if (fread(metadata, 1, metadata_length, archive) != metadata_length) {
        free(metadata);
        return 0;
    }
    metadata[metadata_length] = '\0';

    if (metadata_length < 3 || metadata[0] != '|' || metadata[metadata_length - 1] != '|') {
        free(metadata);
        return 0;
    }

    int count = 0;
    long long total_data_size = 0;
    char *saveptr = NULL;
    char *record = strtok_r(metadata, "|", &saveptr);
    while (record != NULL) {
        if (count >= MAX_FILES) {
            free(metadata);
            return 0;
        }

        char *perm_text = strchr(record, ',');
        if (perm_text == NULL) {
            free(metadata);
            return 0;
        }
        *perm_text++ = '\0';

        char *size_text = strchr(perm_text, ',');
        if (size_text == NULL) {
            free(metadata);
            return 0;
        }
        *size_text++ = '\0';

        if (!is_stored_name_valid(record)) {
            free(metadata);
            return 0;
        }

        char *endptr = NULL;
        long permission = strtol(perm_text, &endptr, 8);
        if (*endptr != '\0' || permission < 0 || permission > 0777) {
            free(metadata);
            return 0;
        }

        long long file_size;
        if (!parse_long_long(size_text, &file_size)) {
            free(metadata);
            return 0;
        }

        strncpy(entries[count].name, record, MAX_NAME_LEN);
        entries[count].name[MAX_NAME_LEN] = '\0';
        entries[count].permission = (mode_t)permission;
        entries[count].size = file_size;
        entries[count].source_path[0] = '\0';
        total_data_size += file_size;
        ++count;

        record = strtok_r(NULL, "|", &saveptr);
    }

    free(metadata);

    if (count == 0) return 0;
    if (organization_size + total_data_size != archive_size) return 0;

    *file_count = count;
    *data_start = organization_size;
    return 1;
}

static int extract_archive(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    const char *archive_name = argv[2];
    const char *target_dir = (argc == 4) ? argv[3] : ".";

    if (!is_sau_file(archive_name)) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    struct stat st;
    if (!is_regular_file(archive_name, &st)) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    FILE *archive = fopen(archive_name, "rb");
    if (archive == NULL) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    ArchiveEntry entries[MAX_FILES];
    int file_count = 0;
    long long data_start = 0;
    if (!read_archive_header(archive, (long long)st.st_size, entries, &file_count, &data_start)) {
        fclose(archive);
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    if (!mkdir_if_needed(target_dir)) {
        fclose(archive);
        fprintf(stderr, "Dizin oluşturulamadı: %s\n", target_dir);
        return 1;
    }

    if (fseeko(archive, (off_t)data_start, SEEK_SET) != 0) {
        fclose(archive);
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 1;
    }

    for (int i = 0; i < file_count; ++i) {
        char output_path[PATH_MAX];
        int written;
        if (strcmp(target_dir, ".") == 0) {
            written = snprintf(output_path, sizeof(output_path), "%s", entries[i].name);
        } else {
            written = snprintf(output_path, sizeof(output_path), "%s/%s", target_dir, entries[i].name);
        }
        if (written < 0 || written >= (int)sizeof(output_path)) {
            fclose(archive);
            fprintf(stderr, "Çıkış yolu çok uzun.\n");
            return 1;
        }

        FILE *output = fopen(output_path, "wb");
        if (output == NULL) {
            fclose(archive);
            perror("Çıkış dosyası açılamadı");
            return 1;
        }

        if (!copy_n_bytes(archive, output, entries[i].size)) {
            fclose(output);
            fclose(archive);
            printf("Arşiv dosyası uygunsuz veya bozuk!\n");
            return 1;
        }

        fclose(output);
        chmod(output_path, entries[i].permission);
    }

    fclose(archive);

    if (strcmp(target_dir, ".") == 0) {
        printf("Geçerli dizinde dosyalar açıldı.\n");
    } else {
        printf("%s dizininde dosyalar açıldı.\n", target_dir);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Kullanım:\n  tarsau -b dosya1 [dosya2 ...] [-o arşiv.sau]\n  tarsau -a arşiv.sau [dizin]\n");
        return 1;
    }

    if (strcmp(argv[1], "-b") == 0) {
        return create_archive(argc, argv);
    }

    if (strcmp(argv[1], "-a") == 0) {
        return extract_archive(argc, argv);
    }

    fprintf(stderr, "Geçersiz parametre: %s\n", argv[1]);
    return 1;
}
