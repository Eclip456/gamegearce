#include "rom.h"

#include <fileioc.h>
#include <string.h>

#define FORMAT_VERSION 1
#define HEADER_SIZE (7 + 3 + 4 + 4 + ROM_TITLE_LEN)
#define PAGE_HEADER_SIZE 8

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void page_name(char *out, const char *prefix, uint8_t index)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(prefix);

    memcpy(out, prefix, len);
    out[len] = 'p';
    out[len + 1] = hex[index >> 4];
    out[len + 2] = hex[index & 15];
    out[len + 3] = '\0';
}

uint8_t rom_find_games(rom_info_t games[], uint8_t max_games)
{
    void *vat_ptr = NULL;
    const char *name;
    uint8_t count = 0;

    while (count < max_games && (name = ti_Detect(&vat_ptr, "GGCEHDR")))
    {
        rom_info_t *game = &games[count];
        uint8_t raw[HEADER_SIZE];
        uint8_t handle = ti_Open(name, "r");

        if (!handle)
            continue;
        if (ti_Read(raw, HEADER_SIZE, 1, handle) == 1 && raw[7] == FORMAT_VERSION)
        {
            strncpy(game->prefix, name, sizeof game->prefix - 1);
            game->prefix[sizeof game->prefix - 1] = '\0';
            game->version = raw[7];
            game->system = raw[8];
            game->page_count = raw[9] ? raw[9] : 256;
            game->rom_size = read_le32(&raw[10]);
            game->crc32 = read_le32(&raw[14]);
            memcpy(game->title, &raw[18], ROM_TITLE_LEN);
            game->title[ROM_TITLE_LEN - 1] = '\0';
            /* A prefix longer than 5 chars can't have page AppVars. */
            if (strlen(game->prefix) <= 5)
                count++;
        }
        ti_Close(handle);
    }
    return count;
}

rom_status_t rom_load(const rom_info_t *game, const uint8_t *pages[], rom_error_t *err)
{
    char name[9];

    memset(err, 0, sizeof *err);

    /*
     * Pass 1: move every page into Archive. Archiving can trigger a garbage
     * collect, which moves other archived variables, so no data pointers are
     * taken until every page is already in place.
     */
    for (uint16_t i = 0; i < game->page_count; i++)
    {
        uint8_t handle;
        rom_status_t status = ROM_OK;

        err->page = i;
        page_name(name, game->prefix, (uint8_t)i);
        handle = ti_Open(name, "r");
        if (!handle)
            return ROM_MISSING_PAGE;

        err->size = ti_GetSize(handle);
        err->archived = ti_IsArchived(handle);
        if (err->size != PAGE_HEADER_SIZE + ROM_PAGE_SIZE)
            status = ROM_BAD_SIZE;
        else if (!err->archived && !ti_SetArchiveStatus(true, handle))
            status = ROM_NO_MEMORY;
        ti_Close(handle);
        if (status != ROM_OK)
            return status;
    }

    /* Pass 2: nothing moves any more, so the pointers stay valid. */
    for (uint16_t i = 0; i < game->page_count; i++)
    {
        const uint8_t *data;
        uint8_t handle;

        err->page = i;
        page_name(name, game->prefix, (uint8_t)i);
        handle = ti_Open(name, "r");
        if (!handle)
            return ROM_MISSING_PAGE;
        err->size = ti_GetSize(handle);
        err->archived = ti_IsArchived(handle);
        data = ti_GetDataPtr(handle);
        ti_Close(handle);

        memcpy(err->head, data, sizeof err->head);
        if (memcmp(data, "GGPG", 4) || data[4] != (uint8_t)i || data[5] != FORMAT_VERSION)
            return ROM_BAD_HEADER;
        pages[i] = data + PAGE_HEADER_SIZE;
    }
    return ROM_OK;
}

uint32_t rom_crc32(const uint8_t *const pages[], uint16_t page_count)
{
    static uint32_t table[256];
    uint32_t crc = 0xFFFFFFFF;

    if (!table[1])
    {
        for (uint16_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (uint8_t k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    }

    for (uint16_t p = 0; p < page_count; p++)
    {
        const uint8_t *data = pages[p];
        for (uint16_t i = 0; i < ROM_PAGE_SIZE; i++)
            crc = table[(uint8_t)(crc ^ data[i])] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}
