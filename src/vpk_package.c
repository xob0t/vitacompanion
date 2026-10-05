#include "vpk_package.h"

#include <miniz_tinfl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VPK_IN_SIZE (128 * 1024)
/* tinfl needs a power-of-two circular output buffer when streaming. */
#define VPK_OUT_SIZE (256 * 1024)
#define VPK_PATH_MAX 512

#define ZIP_LOCAL_HEADER_SIG 0x04034B50
#define ZIP_CENTRAL_HEADER_SIG 0x02014B50
#define ZIP_END_SIG 0x06054B50
#define ZIP_DESCRIPTOR_SIG 0x08074B50
#define ZIP_LOCAL_HEADER_SIZE 30
#define ZIP_FLAG_ENCRYPTED 0x0001
#define ZIP_FLAG_DESCRIPTOR 0x0008
#define ZIP_METHOD_STORED 0
#define ZIP_METHOD_DEFLATED 8

typedef struct {
    tinfl_decompressor inflator;
    uint8_t in[VPK_IN_SIZE];
    uint8_t out[VPK_OUT_SIZE];
    char name[VPK_PATH_MAX];
    char path[VPK_PATH_MAX];
} vpk_work;

/* Archive input, buffered in work->in so it can be read sequentially. */
typedef struct {
    vpk_work *work;
    vpk_read_fn read;
    void *ctx;
    size_t pos;
    size_t length;
} zip_stream;

typedef struct {
    uint32_t flags;
    uint32_t method;
    uint32_t crc;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} zip_entry;

static uint32_t read_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static vpk_work *work_alloc(SceUID *block)
{
    SceSize size = (sizeof(vpk_work) + 0xFFF) & ~0xFFF;
    void *base = NULL;

    *block = sceKernelAllocMemBlock("vitacompanion_vpk",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (*block < 0)
        return NULL;

    if (sceKernelGetMemBlockBase(*block, &base) < 0)
    {
        sceKernelFreeMemBlock(*block);
        *block = -1;
        return NULL;
    }

    return base;
}

/*
 * Returns 1 when unread input is buffered, 0 at the end of the input. The
 * buffer is filled completely: a network reader can return a few KiB at a
 * time, and inflating such small pieces makes for many slow, small writes.
 */
static int stream_fill(zip_stream *stream)
{
    if (stream->pos < stream->length)
        return 1;

    stream->pos = 0;
    stream->length = 0;
    while (stream->length < VPK_IN_SIZE)
    {
        int result = stream->read(stream->ctx,
            stream->work->in + stream->length,
            VPK_IN_SIZE - (unsigned int)stream->length);

        if (result < 0)
            return VPK_ERROR_READ;
        if (result == 0)
            break;
        stream->length += (size_t)result;
    }

    return stream->length > 0;
}

/* Reads exactly size bytes into dst, or skips them when dst is NULL. */
static int stream_read(zip_stream *stream, void *dst, size_t size)
{
    uint8_t *out = dst;

    while (size > 0)
    {
        size_t chunk;
        int result = stream_fill(stream);

        if (result < 0)
            return result;
        if (result == 0)
            return VPK_ERROR_CORRUPT;

        chunk = stream->length - stream->pos;
        if (chunk > size)
            chunk = size;
        if (out)
        {
            memcpy(out, stream->work->in + stream->pos, chunk);
            out += chunk;
        }
        stream->pos += chunk;
        size -= chunk;
    }

    return 0;
}

static int write_all(SceUID fd, const void *buffer, SceSize size)
{
    SceSize done = 0;

    while (done < size)
    {
        int result = sceIoWrite(fd, (const uint8_t *)buffer + done,
            size - done);
        if (result <= 0)
            return VPK_ERROR_WRITE;
        done += (SceSize)result;
    }

    return 0;
}

/* Creates each parent of path, and path itself when include_last is set. */
static void make_dirs(char *path, bool include_last)
{
    char *p;

    for (p = path + 1; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            sceIoMkdir(path, 0777);
            *p = '/';
        }
    }

    if (include_last)
        sceIoMkdir(path, 0777);
}

static int remove_tree_at(char *path, size_t capacity)
{
    SceIoStat stat;
    SceIoDirent entry;
    SceUID dfd;
    size_t length;
    int result = 0;

    if (sceIoGetstat(path, &stat) < 0)
        return 0;

    if (!SCE_S_ISDIR(stat.st_mode))
        return sceIoRemove(path);

    dfd = sceIoDopen(path);
    if (dfd < 0)
        return dfd;

    length = strlen(path);
    memset(&entry, 0, sizeof(entry));
    while (sceIoDread(dfd, &entry) > 0)
    {
        size_t name_length = strlen(entry.d_name);
        int child_result;

        if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
            continue;

        if (length + 1 + name_length >= capacity)
        {
            result = VPK_ERROR_BAD_ENTRY;
            continue;
        }

        path[length] = '/';
        memcpy(path + length + 1, entry.d_name, name_length + 1);
        if (SCE_S_ISDIR(entry.d_stat.st_mode))
            child_result = remove_tree_at(path, capacity);
        else
            child_result = sceIoRemove(path);
        path[length] = '\0';

        if (child_result < 0)
            result = child_result;
        memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(dfd);

    if (result < 0)
        return result;
    return sceIoRmdir(path);
}

int vpk_remove_tree(const char *path)
{
    char buffer[VPK_PATH_MAX];

    if (!path || strlen(path) >= sizeof(buffer))
        return VPK_ERROR_BAD_ENTRY;

    strcpy(buffer, path);
    return remove_tree_at(buffer, sizeof(buffer));
}

static int copy_stored(zip_stream *stream, SceUID out_fd,
    const zip_entry *entry, uint32_t *crc)
{
    uint32_t remaining = entry->compressed_size;

    if (entry->compressed_size != entry->uncompressed_size)
        return VPK_ERROR_CORRUPT;

    while (remaining > 0)
    {
        const uint8_t *data;
        size_t chunk;
        int result = stream_fill(stream);

        if (result < 0)
            return result;
        if (result == 0)
            return VPK_ERROR_CORRUPT;

        data = stream->work->in + stream->pos;
        chunk = stream->length - stream->pos;
        if (chunk > remaining)
            chunk = remaining;

        result = write_all(out_fd, data, (SceSize)chunk);
        if (result < 0)
            return result;
        *crc = vpk_crc32(*crc, data, chunk);
        stream->pos += chunk;
        remaining -= (uint32_t)chunk;
    }

    return 0;
}

/*
 * Inflates one entry. When the sizes follow the data in a descriptor, the
 * compressed size is unknown and the end of the deflate stream marks the end
 * of the entry.
 */
static int inflate_entry(zip_stream *stream, SceUID out_fd,
    const zip_entry *entry, uint32_t *crc, uint32_t *written)
{
    vpk_work *work = stream->work;
    bool sizes_known = !(entry->flags & ZIP_FLAG_DESCRIPTOR);
    uint32_t remaining_in = entry->compressed_size;
    size_t out_pos = 0;

    *written = 0;
    tinfl_init(&work->inflator);

    for (;;)
    {
        size_t in_bytes;
        size_t out_bytes;
        bool more_input;
        tinfl_status status;
        int result = stream_fill(stream);

        if (result < 0)
            return result;

        in_bytes = stream->length - stream->pos;
        if (sizes_known && in_bytes > remaining_in)
            in_bytes = remaining_in;
        more_input = sizes_known ? remaining_in > in_bytes : result > 0;

        out_bytes = VPK_OUT_SIZE - out_pos;
        status = tinfl_decompress(&work->inflator,
            work->in + stream->pos, &in_bytes, work->out,
            work->out + out_pos, &out_bytes,
            more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        stream->pos += in_bytes;
        if (sizes_known)
            remaining_in -= (uint32_t)in_bytes;

        if (out_bytes > 0)
        {
            if (sizes_known &&
                out_bytes > entry->uncompressed_size - *written)
                return VPK_ERROR_CORRUPT;

            result = write_all(out_fd, work->out + out_pos,
                (SceSize)out_bytes);
            if (result < 0)
                return result;
            *crc = vpk_crc32(*crc, work->out + out_pos, out_bytes);
            *written += (uint32_t)out_bytes;
            out_pos = (out_pos + out_bytes) & (VPK_OUT_SIZE - 1);
        }

        if (status == TINFL_STATUS_DONE)
            break;
        if (status < 0 ||
            (status == TINFL_STATUS_NEEDS_MORE_INPUT && !more_input))
            return VPK_ERROR_CORRUPT;
    }

    /* Skip anything stored after the end of the deflate stream. */
    if (sizes_known && remaining_in > 0)
        return stream_read(stream, NULL, remaining_in);

    return 0;
}

/* Reads the CRC and sizes that follow the data, signature optional. */
static int read_descriptor(zip_stream *stream, zip_entry *entry)
{
    uint8_t data[16];
    const uint8_t *fields = data;
    int result = stream_read(stream, data, 12);

    if (result < 0)
        return result;
    if (read_le32(data) == ZIP_DESCRIPTOR_SIG)
    {
        result = stream_read(stream, data + 12, 4);
        if (result < 0)
            return result;
        fields = data + 4;
    }

    entry->crc = read_le32(fields);
    entry->compressed_size = read_le32(fields + 4);
    entry->uncompressed_size = read_le32(fields + 8);
    return 0;
}

static int extract_file(zip_stream *stream, zip_entry *entry, bool is_dir)
{
    uint32_t crc = 0;
    uint32_t written = 0;
    int result;

    if (entry->method != ZIP_METHOD_STORED &&
        entry->method != ZIP_METHOD_DEFLATED)
        return VPK_ERROR_METHOD;

    if (is_dir)
    {
        if (entry->compressed_size != 0)
            return VPK_ERROR_CORRUPT;
    }
    else
    {
        SceUID out_fd;

        /* Without its size, the end of uncompressed data cannot be found. */
        if (entry->method == ZIP_METHOD_STORED &&
            (entry->flags & ZIP_FLAG_DESCRIPTOR))
            return VPK_ERROR_NO_SIZES;

        out_fd = sceIoOpen(stream->work->path,
            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (out_fd < 0)
            return VPK_ERROR_WRITE;

        if (entry->method == ZIP_METHOD_STORED)
        {
            result = copy_stored(stream, out_fd, entry, &crc);
            written = entry->compressed_size;
        }
        else
            result = inflate_entry(stream, out_fd, entry, &crc, &written);

        if (sceIoClose(out_fd) < 0 && result == 0)
            result = VPK_ERROR_WRITE;
        if (result < 0)
            return result;
    }

    if (entry->flags & ZIP_FLAG_DESCRIPTOR)
    {
        result = read_descriptor(stream, entry);
        if (result < 0)
            return result;
    }

    if (crc != entry->crc || written != entry->uncompressed_size)
        return VPK_ERROR_CORRUPT;
    return 0;
}

/*
 * Extracts entries in the order of their local headers, so the archive can
 * come from a stream. The central directory at the end is not needed.
 */
static int extract_entries(zip_stream *stream, const char *dest_dir)
{
    vpk_work *work = stream->work;
    uint8_t header[ZIP_LOCAL_HEADER_SIZE];
    size_t dest_length = strlen(dest_dir);
    bool first = true;
    int result;

    if (dest_length + 2 >= VPK_PATH_MAX)
        return VPK_ERROR_BAD_ENTRY;
    strcpy(work->path, dest_dir);
    make_dirs(work->path, true);

    for (;;)
    {
        zip_entry entry;
        uint32_t signature;
        uint32_t name_length;
        size_t path_length;
        bool is_dir;

        result = stream_read(stream, header, 4);
        if (result < 0)
            return first && result == VPK_ERROR_CORRUPT
                ? VPK_ERROR_NOT_ZIP : result;

        signature = read_le32(header);
        if (!first && (signature == ZIP_CENTRAL_HEADER_SIG ||
            signature == ZIP_END_SIG))
            return 0;
        if (signature != ZIP_LOCAL_HEADER_SIG)
            return first ? VPK_ERROR_NOT_ZIP : VPK_ERROR_CORRUPT;
        first = false;

        result = stream_read(stream, header + 4,
            ZIP_LOCAL_HEADER_SIZE - 4);
        if (result < 0)
            return result;

        entry.flags = read_le16(header + 6);
        entry.method = read_le16(header + 8);
        entry.crc = read_le32(header + 14);
        entry.compressed_size = read_le32(header + 18);
        entry.uncompressed_size = read_le32(header + 22);
        name_length = read_le16(header + 26);

        if (entry.flags & ZIP_FLAG_ENCRYPTED)
            return VPK_ERROR_ENCRYPTED;
        if (!(entry.flags & ZIP_FLAG_DESCRIPTOR) &&
            (entry.compressed_size == 0xFFFFFFFF ||
                entry.uncompressed_size == 0xFFFFFFFF))
            return VPK_ERROR_ZIP64;
        if (name_length == 0 ||
            dest_length + 1 + name_length >= VPK_PATH_MAX)
            return VPK_ERROR_BAD_ENTRY;

        result = stream_read(stream, work->name, name_length);
        if (result < 0)
            return result;
        work->name[name_length] = '\0';
        result = stream_read(stream, NULL, read_le16(header + 28));
        if (result < 0)
            return result;

        if (strlen(work->name) != name_length ||
            !vpk_entry_name_normalize(work->name))
            return VPK_ERROR_BAD_ENTRY;

        /* Length was checked against VPK_PATH_MAX above. */
        memcpy(work->path, dest_dir, dest_length);
        work->path[dest_length] = '/';
        memcpy(work->path + dest_length + 1, work->name, name_length + 1);
        path_length = dest_length + 1 + name_length;
        is_dir = work->path[path_length - 1] == '/';
        if (is_dir)
        {
            work->path[path_length - 1] = '\0';
            make_dirs(work->path, true);
        }
        else
            make_dirs(work->path, false);

        result = extract_file(stream, &entry, is_dir);
        if (result < 0)
            return result;
    }
}

int vpk_extract_from(vpk_read_fn read, void *ctx, const char *dest_dir)
{
    zip_stream stream;
    SceUID block;
    int result;

    memset(&stream, 0, sizeof(stream));
    stream.work = work_alloc(&block);
    if (!stream.work)
        return VPK_ERROR_NO_MEMORY;
    stream.read = read;
    stream.ctx = ctx;

    result = extract_entries(&stream, dest_dir);

    sceKernelFreeMemBlock(block);
    return result;
}

static int read_file(void *ctx, void *buffer, unsigned int size)
{
    return sceIoRead(*(SceUID *)ctx, buffer, size);
}

int vpk_extract(const char *vpk_path, const char *dest_dir)
{
    SceUID fd;
    int result;

    fd = sceIoOpen(vpk_path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return VPK_ERROR_OPEN;

    result = vpk_extract_from(read_file, &fd, dest_dir);

    sceIoClose(fd);
    return result;
}

/*
 * Reads the title ID from param.sfo and writes sce_sys/package/head.bin when
 * the VPK did not include one; the promoter refuses packages without it.
 */
int vpk_prepare_package(const char *pkg_dir,
    char title_id[VPK_TITLE_ID_LENGTH + 1])
{
    char sfo_title_id[16];
    char content_id[49];
    SceIoStat stat;
    SceUID block;
    SceUID fd;
    vpk_work *work;
    int size;
    int result = 0;

    work = work_alloc(&block);
    if (!work)
        return VPK_ERROR_NO_MEMORY;

    snprintf(work->path, VPK_PATH_MAX, "%s/sce_sys/param.sfo", pkg_dir);
    fd = sceIoOpen(work->path, SCE_O_RDONLY, 0);
    if (fd < 0)
    {
        result = VPK_ERROR_PARAM_SFO;
        goto exit;
    }
    size = sceIoRead(fd, work->in, VPK_IN_SIZE);
    sceIoClose(fd);

    if (size <= 0 ||
        vpk_sfo_get_string(work->in, (size_t)size, "TITLE_ID",
            sfo_title_id, sizeof(sfo_title_id)) < 0)
    {
        result = VPK_ERROR_PARAM_SFO;
        goto exit;
    }
    if (!vpk_title_id_is_valid(sfo_title_id))
    {
        result = VPK_ERROR_TITLE_ID;
        goto exit;
    }
    strcpy(title_id, sfo_title_id);

    if (vpk_sfo_get_string(work->in, (size_t)size, "CONTENT_ID",
            content_id, sizeof(content_id)) < 0)
        content_id[0] = '\0';

    snprintf(work->path, VPK_PATH_MAX, "%s/sce_sys/package/head.bin",
        pkg_dir);
    if (sceIoGetstat(work->path, &stat) >= 0)
        goto exit;

    vpk_make_head_bin(title_id, content_id, work->out);
    make_dirs(work->path, false);
    fd = sceIoOpen(work->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        0777);
    if (fd < 0)
    {
        result = VPK_ERROR_WRITE;
        goto exit;
    }
    result = write_all(fd, work->out, VPK_HEAD_BIN_SIZE);
    if (sceIoClose(fd) < 0 && result == 0)
        result = VPK_ERROR_WRITE;

exit:
    sceKernelFreeMemBlock(block);
    return result;
}

const char *vpk_error_string(int error)
{
    switch (error)
    {
    case VPK_ERROR_NO_MEMORY:
        return "out of memory";
    case VPK_ERROR_OPEN:
        return "cannot open the VPK";
    case VPK_ERROR_READ:
        return "cannot read the VPK";
    case VPK_ERROR_WRITE:
        return "cannot write extracted files";
    case VPK_ERROR_NOT_ZIP:
        return "not a ZIP/VPK file";
    case VPK_ERROR_ZIP64:
        return "ZIP64 archives are not supported";
    case VPK_ERROR_ENCRYPTED:
        return "encrypted archives are not supported";
    case VPK_ERROR_METHOD:
        return "unsupported compression method";
    case VPK_ERROR_BAD_ENTRY:
        return "unsafe or overlong path in archive";
    case VPK_ERROR_CORRUPT:
        return "archive is corrupt";
    case VPK_ERROR_PARAM_SFO:
        return "missing or invalid sce_sys/param.sfo";
    case VPK_ERROR_TITLE_ID:
        return "invalid TITLE_ID in param.sfo";
    case VPK_ERROR_NO_SIZES:
        return "uncompressed entry without sizes in its local header";
    default:
        return NULL;
    }
}
