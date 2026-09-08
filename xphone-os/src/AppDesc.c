// The image names itself.
//
// Every ESP-IDF app carries an esp_app_desc_t 32 bytes into its image. The
// web flasher's device report reads it from both OTA slots to answer "which
// firmware is on this device?" — without it, a field report can only say
// what the ROM bootloader knows. The Arduino core's precompiled libraries
// ship that descriptor as a WEAK symbol filled in by their own build
// ("arduino-lib-builder 8cabf2c, built Feb 11 2026"), which is the same
// string for every Flowe build and for any other Arduino firmware. A real
// user's report (2026-09-05) could not name the firmware in either slot
// because of it.
//
// This strong definition replaces it with the Flowe series, the exact git
// build, the compile date, and the project name "flowe". The bootloader
// reads the tail fields (eFuse block revision window, MMU page size) before
// it will boot an image, so they are taken from the same sdkconfig the
// libraries were built with — never zeroed.
//
// C, not C++: designated initializers in declaration order, no constructor
// tricks, and the section attribute lands exactly where sections.ld expects
// (`*(.rodata_desc .rodata_desc.*)` is the first thing in flash rodata).

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"

#include "generated/xphone_version.h"

#ifndef CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL
#define CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL 0
#endif
#ifndef CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL
#define CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL 199
#endif
#ifndef CONFIG_MMU_PAGE_SIZE
#define CONFIG_MMU_PAGE_SIZE 0x10000
#endif

// log2 of the MMU page size, the way esp_app_desc.c stores it.
#if CONFIG_MMU_PAGE_SIZE == 0x2000
#define XP_MMU_PAGE_LOG2 13
#elif CONFIG_MMU_PAGE_SIZE == 0x4000
#define XP_MMU_PAGE_LOG2 14
#elif CONFIG_MMU_PAGE_SIZE == 0x8000
#define XP_MMU_PAGE_LOG2 15
#else
#define XP_MMU_PAGE_LOG2 16
#endif

#define XP_STR2(x) #x
#define XP_STR(x) XP_STR2(x)
#define XP_IDF_VER \
  "v" XP_STR(ESP_IDF_VERSION_MAJOR) "." XP_STR(ESP_IDF_VERSION_MINOR) "." XP_STR(ESP_IDF_VERSION_PATCH)

// "0.6.6 (58c1576)" — the same two facts the boot line prints, in the 32
// bytes the descriptor allows. XPHONE_GIT_REV is `git describe --dirty`
// style; a long describe still fits because the series is short.
const esp_app_desc_t esp_app_desc __attribute__((used, section(".rodata_desc"))) = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
    .secure_version = 0,
    .reserv1 = {0, 0},
    .version = XPHONE_MARKETING " (" XPHONE_GIT_REV ")",
    .project_name = "flowe",
    .time = __TIME__,
    .date = __DATE__,
    .idf_ver = XP_IDF_VER,
    .app_elf_sha256 = {0},
    .min_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
    .max_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
    .mmu_page_size = XP_MMU_PAGE_LOG2,
    .reserv3 = {0, 0, 0},
    .reserv2 = {0},
};
