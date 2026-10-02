#ifndef RV_UNZIP_LITE_H
#define RV_UNZIP_LITE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void *unzFile;

typedef struct {
    unsigned long version;
    unsigned long version_needed;
    unsigned long flag;
    unsigned long compression_method;
    unsigned long dosDate;
    unsigned long crc;
    unsigned long compressed_size;
    unsigned long uncompressed_size;
    unsigned long size_filename;
    unsigned long size_file_extra;
    unsigned long size_file_comment;
    unsigned long disk_num_start;
    unsigned long internal_fa;
    unsigned long external_fa;
} unz_file_info;

#define UNZ_OK 0
#define UNZ_END_OF_LIST_OF_FILE (-100)
#define UNZ_ERRNO (-1)
#define UNZ_PARAMERROR (-102)
#define UNZ_BADZIPFILE (-103)
#define UNZ_INTERNALERROR (-104)

unzFile unzOpen(const char *path);
int unzClose(unzFile file);
int unzGoToFirstFile(unzFile file);
int unzGoToNextFile(unzFile file);
int unzGetCurrentFileInfo(unzFile file, unz_file_info *info, char *name, unsigned long name_size, void *extra, unsigned long extra_size, char *comment, unsigned long comment_size);
int unzOpenCurrentFile(unzFile file);
int unzReadCurrentFile(unzFile file, void *buf, unsigned len);
int unzCloseCurrentFile(unzFile file);

#ifdef __cplusplus
}
#endif

#endif
