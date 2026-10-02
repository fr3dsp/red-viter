#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#include "minizip/unzip.h"

#define SIG_EOCD 0x06054b50u
#define SIG_CDIR 0x02014b50u
#define SIG_LOCAL 0x04034b50u
#define IN_CHUNK 16384

typedef struct {
    FILE *fp;
    uint32_t cd_offset;
    uint32_t entry_count;
    uint32_t entry_index;
    uint32_t cur_record;
    uint32_t cur_local;
    unz_file_info info;
    int opened;
    int method;
    int stream_end;
    uint32_t comp_left;
    uint32_t raw_left;
    z_stream zs;
    unsigned char *inbuf;
} unz_lite;

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_record(unz_lite *u) {
    unsigned char h[46];
    if (fseek(u->fp, (long)u->cur_record, SEEK_SET) != 0) return UNZ_ERRNO;
    if (fread(h, 1, sizeof(h), u->fp) != sizeof(h)) return UNZ_ERRNO;
    if (rd32(h) != SIG_CDIR) return UNZ_BADZIPFILE;
    u->info.version = rd16(h + 4);
    u->info.version_needed = rd16(h + 6);
    u->info.flag = rd16(h + 8);
    u->info.compression_method = rd16(h + 10);
    u->info.dosDate = rd32(h + 12);
    u->info.crc = rd32(h + 16);
    u->info.compressed_size = rd32(h + 20);
    u->info.uncompressed_size = rd32(h + 24);
    u->info.size_filename = rd16(h + 28);
    u->info.size_file_extra = rd16(h + 30);
    u->info.size_file_comment = rd16(h + 32);
    u->info.disk_num_start = rd16(h + 34);
    u->info.internal_fa = rd16(h + 36);
    u->info.external_fa = rd32(h + 38);
    u->cur_local = rd32(h + 42);
    return UNZ_OK;
}

unzFile unzOpen(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 22) {
        fclose(fp);
        return NULL;
    }
    long scan = size < 65557 ? size : 65557;
    unsigned char *tail = malloc((size_t)scan);
    if (!tail) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, size - scan, SEEK_SET);
    if (fread(tail, 1, (size_t)scan, fp) != (size_t)scan) {
        free(tail);
        fclose(fp);
        return NULL;
    }
    long pos = -1;
    for (long i = scan - 22; i >= 0; i--) {
        if (rd32(tail + i) == SIG_EOCD) {
            pos = i;
            break;
        }
    }
    if (pos < 0) {
        free(tail);
        fclose(fp);
        return NULL;
    }
    unz_lite *u = calloc(1, sizeof(*u));
    if (!u) {
        free(tail);
        fclose(fp);
        return NULL;
    }
    u->fp = fp;
    u->entry_count = rd16(tail + pos + 10);
    u->cd_offset = rd32(tail + pos + 16);
    free(tail);
    return u;
}

int unzClose(unzFile file) {
    unz_lite *u = file;
    if (!u) return UNZ_PARAMERROR;
    if (u->opened) unzCloseCurrentFile(file);
    fclose(u->fp);
    free(u);
    return UNZ_OK;
}

int unzGoToFirstFile(unzFile file) {
    unz_lite *u = file;
    if (!u) return UNZ_PARAMERROR;
    if (u->entry_count == 0) return UNZ_END_OF_LIST_OF_FILE;
    u->entry_index = 0;
    u->cur_record = u->cd_offset;
    return load_record(u);
}

int unzGoToNextFile(unzFile file) {
    unz_lite *u = file;
    if (!u) return UNZ_PARAMERROR;
    if (u->entry_index + 1 >= u->entry_count) return UNZ_END_OF_LIST_OF_FILE;
    u->cur_record += 46 + (uint32_t)(u->info.size_filename + u->info.size_file_extra + u->info.size_file_comment);
    u->entry_index++;
    return load_record(u);
}

int unzGetCurrentFileInfo(unzFile file, unz_file_info *info, char *name, unsigned long name_size, void *extra, unsigned long extra_size, char *comment, unsigned long comment_size) {
    unz_lite *u = file;
    if (!u) return UNZ_PARAMERROR;
    if (info) *info = u->info;
    if (name && name_size > 0) {
        unsigned long n = u->info.size_filename;
        if (n > name_size - 1) n = name_size - 1;
        if (fseek(u->fp, (long)(u->cur_record + 46), SEEK_SET) != 0) return UNZ_ERRNO;
        if (fread(name, 1, n, u->fp) != n) return UNZ_ERRNO;
        name[n] = 0;
    }
    if (extra && extra_size > 0) memset(extra, 0, extra_size);
    if (comment && comment_size > 0) comment[0] = 0;
    return UNZ_OK;
}

int unzOpenCurrentFile(unzFile file) {
    unz_lite *u = file;
    if (!u) return UNZ_PARAMERROR;
    if (u->opened) return UNZ_PARAMERROR;
    unsigned char h[30];
    if (fseek(u->fp, (long)u->cur_local, SEEK_SET) != 0) return UNZ_ERRNO;
    if (fread(h, 1, sizeof(h), u->fp) != sizeof(h)) return UNZ_ERRNO;
    if (rd32(h) != SIG_LOCAL) return UNZ_BADZIPFILE;
    uint32_t data_pos = u->cur_local + 30 + rd16(h + 26) + rd16(h + 28);
    if (fseek(u->fp, (long)data_pos, SEEK_SET) != 0) return UNZ_ERRNO;
    u->method = (int)u->info.compression_method;
    u->comp_left = (uint32_t)u->info.compressed_size;
    u->raw_left = (uint32_t)u->info.uncompressed_size;
    u->stream_end = 0;
    if (u->method == 8) {
        u->inbuf = malloc(IN_CHUNK);
        if (!u->inbuf) return UNZ_INTERNALERROR;
        memset(&u->zs, 0, sizeof(u->zs));
        if (inflateInit2(&u->zs, -MAX_WBITS) != Z_OK) {
            free(u->inbuf);
            u->inbuf = NULL;
            return UNZ_INTERNALERROR;
        }
    } else if (u->method != 0) {
        return UNZ_BADZIPFILE;
    }
    u->opened = 1;
    return UNZ_OK;
}

int unzReadCurrentFile(unzFile file, void *buf, unsigned len) {
    unz_lite *u = file;
    if (!u || !u->opened) return UNZ_PARAMERROR;
    if (len == 0) return 0;
    if (u->method == 0) {
        uint32_t n = len < u->raw_left ? len : u->raw_left;
        if (n == 0) return -1;
        size_t got = fread(buf, 1, n, u->fp);
        if (got == 0) return -1;
        u->raw_left -= (uint32_t)got;
        return (int)got;
    }
    u->zs.next_out = buf;
    u->zs.avail_out = len;
    while (u->zs.avail_out > 0 && !u->stream_end) {
        if (u->zs.avail_in == 0) {
            if (u->comp_left == 0) break;
            uint32_t n = u->comp_left < IN_CHUNK ? u->comp_left : IN_CHUNK;
            size_t got = fread(u->inbuf, 1, n, u->fp);
            if (got == 0) return -1;
            u->comp_left -= (uint32_t)got;
            u->zs.next_in = u->inbuf;
            u->zs.avail_in = (uInt)got;
        }
        uInt before_in = u->zs.avail_in;
        uInt before_out = u->zs.avail_out;
        int r = inflate(&u->zs, Z_NO_FLUSH);
        if (r == Z_STREAM_END) {
            u->stream_end = 1;
        } else if (r != Z_OK && r != Z_BUF_ERROR) {
            return -1;
        }
        if (before_in == u->zs.avail_in && before_out == u->zs.avail_out && !u->stream_end) return -1;
    }
    int produced = (int)(len - u->zs.avail_out);
    if (produced == 0) return -1;
    return produced;
}

int unzCloseCurrentFile(unzFile file) {
    unz_lite *u = file;
    if (!u || !u->opened) return UNZ_PARAMERROR;
    if (u->method == 8) {
        inflateEnd(&u->zs);
        free(u->inbuf);
        u->inbuf = NULL;
    }
    u->opened = 0;
    return UNZ_OK;
}
