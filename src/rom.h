#ifndef ROM_H
#define ROM_H

#include <stdbool.h>
#include <stdint.h>

#define ROM_PAGE_SIZE 0x4000
#define ROM_MAX_PAGES 256
#define ROM_TITLE_LEN 32
#define ROM_MAX_GAMES 32

/* One converted game found on the calculator (see tools/gg2ce.py). */
typedef struct {
    char prefix[9];                 /* header AppVar name, 1-5 chars */
    char title[ROM_TITLE_LEN];
    uint8_t version;
    uint8_t system;
    uint16_t page_count;            /* 1-256 */
    uint32_t rom_size;
    uint32_t crc32;
} rom_info_t;

typedef enum {
    ROM_OK,
    ROM_MISSING_PAGE,
    ROM_BAD_PAGE,
    ROM_NO_MEMORY,
} rom_status_t;

/* Fills games[] with every converted ROM on the calculator; returns the count. */
uint8_t rom_find_games(rom_info_t games[], uint8_t max_games);

/*
 * Locates every page of a game, archiving pages that are still in RAM.
 * On success pages[i] points at the 16 KB of ROM page i. On failure
 * *bad_page is the page that is missing or corrupt.
 */
rom_status_t rom_load(const rom_info_t *game, const uint8_t *pages[], uint16_t *bad_page);

/* CRC-32 of all pages, to compare against game->crc32. */
uint32_t rom_crc32(const uint8_t *const pages[], uint16_t page_count);

#endif
