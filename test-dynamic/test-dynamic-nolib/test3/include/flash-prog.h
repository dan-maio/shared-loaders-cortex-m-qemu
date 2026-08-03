/**
 * @file flash-prog.h
 * @brief Minimal STM32F411 embedded-flash program/erase driver for the flat_fs
 *        filesystem region (sectors 6 and 7).
 *
 * The erase/program routines run FROM SRAM (placed in the .ramfunc section by the
 * linker script and copied to RAM by Reset_Handler). This is mandatory on the
 * single-bank F411: while a sector erase or word program is in progress, any
 * instruction fetch from flash stalls, so the routine that initiates the op AND
 * polls the BSY flag must not itself live in flash.
 *
 * Constraints / assumptions:
 *   - Programming uses PSIZE = x32 (word). Valid only at Vdd 2.7..3.6V, which is
 *     the WEACT-F411CE's 3.3V supply -- so no external Vpp is needed.
 *   - @p src passed to flash_program() MUST point into RAM (never flash): the
 *     RAM-resident loop reads it while a flash op is in progress.
 *   - Interrupts whose vectors/handlers are fetched from flash must be DISABLED
 *     across an erase (a long op): an ISR fetch would stall until the op ends.
 *     The provisioning path runs before SysTick/board_init for this reason.
 */
#ifndef FLASH_PROG_H_
#define FLASH_PROG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief flat_fs region sector numbers on the STM32F411RE (sectors 6 and 7). */
#define FLASH_FS_SECTOR_LO 6u
#define FLASH_FS_SECTOR_HI 7u

/** @brief Unlock FLASH->CR for program/erase (KEY1/KEY2). No flash array access. */
void flash_unlock(void);
/** @brief Re-lock FLASH->CR. */
void flash_lock(void);

/**
 * @brief Erase one flash sector by its sector number (SNB). RAM-resident.
 * @param snb Sector number (e.g. FLASH_FS_SECTOR_LO / _HI).
 * @return 0 on success, -1 if a flash error flag was set.
 */
int flash_erase_sector(uint32_t snb);

/**
 * @brief Program @p len bytes from RAM @p src to flash @p addr. RAM-resident.
 *        @p addr must be word-aligned; a trailing partial word is padded with
 *        0xFF (consistent with erased flash, and ignored by readers using size).
 * @return 0 on success, -1 on a flash program error.
 */
int flash_program(uint32_t addr, const void* src, uint32_t len);

/** @brief Reset the flash data+instruction caches so later reads see new bytes. */
void flash_cache_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_PROG_H_ */
