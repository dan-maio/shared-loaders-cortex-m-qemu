/** @file flash-prog.c @brief See flash-prog.h. STM32F411 embedded-flash driver. */
#include "flash-prog.h"

/* --- FLASH peripheral registers (STM32F411, RM0383 section 3). --- */
#define FLASH_R_BASE 0x40023C00u
#define FLASH_ACR    (*(volatile uint32_t*)(FLASH_R_BASE + 0x00u))
#define FLASH_KEYR   (*(volatile uint32_t*)(FLASH_R_BASE + 0x04u))
#define FLASH_SR     (*(volatile uint32_t*)(FLASH_R_BASE + 0x0Cu))
#define FLASH_CR     (*(volatile uint32_t*)(FLASH_R_BASE + 0x10u))

#define FLASH_KEY1   0x45670123u
#define FLASH_KEY2   0xCDEF89ABu

/* FLASH_SR */
#define SR_BSY       (1u << 16)
/* error flags: OPERR(1) WRPERR(4) PGAERR(5) PGPERR(6) PGSERR(7) -> 0xF2 */
#define SR_ERR_FLAGS ((1u<<1)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7))

/* FLASH_CR */
#define CR_PG        (1u << 0)
#define CR_SER       (1u << 1)
#define CR_SNB_MASK  (0xFu << 3)
#define CR_PSIZE_MASK (3u << 8)
#define CR_PSIZE_X32 (2u << 8)
#define CR_STRT      (1u << 16)
#define CR_LOCK      (1u << 31)

/* FLASH_ACR cache controls */
#define ACR_ICEN     (1u << 9)
#define ACR_DCEN     (1u << 10)
#define ACR_ICRST    (1u << 11)
#define ACR_DCRST    (1u << 12)

/* Routines that touch the flash array (erase/program) and poll BSY must run from
 * RAM, not flash. noinline keeps each as a real RAM function (no call into flash). */
#define RAMFUNC __attribute__((section(".ramfunc"), noinline, used))

void flash_unlock(void) {
    if (FLASH_CR & CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

void flash_lock(void) {
    FLASH_CR |= CR_LOCK;
}

RAMFUNC int flash_erase_sector(uint32_t snb) {
    while (FLASH_SR & SR_BSY) { }
    FLASH_SR = SR_ERR_FLAGS;                 /* write-1-clear stale error flags  */
    uint32_t cr = FLASH_CR & ~(CR_SNB_MASK | CR_PSIZE_MASK);
    cr |= CR_SER | ((snb & 0xFu) << 3) | CR_PSIZE_X32;
    FLASH_CR = cr;
    FLASH_CR |= CR_STRT;                     /* start the sector erase           */
    while (FLASH_SR & SR_BSY) { }            /* (polled from RAM)                */
    FLASH_CR &= ~CR_SER;
    return (FLASH_SR & SR_ERR_FLAGS) ? -1 : 0;
}

RAMFUNC int flash_program(uint32_t addr, const void* src, uint32_t len) {
    const uint8_t* s = (const uint8_t*)src;  /* MUST be a RAM pointer            */
    volatile uint32_t* d = (volatile uint32_t*)(uintptr_t)addr;
    while (FLASH_SR & SR_BSY) { }
    FLASH_SR = SR_ERR_FLAGS;
    FLASH_CR = (FLASH_CR & ~CR_PSIZE_MASK) | CR_PSIZE_X32;
    FLASH_CR |= CR_PG;

    int rc = 0;
    uint32_t i = 0;
    while (i < len) {
        uint32_t w = 0;
        uint32_t got = 0;
        for (; got < 4u && (i + got) < len; ++got)   /* assemble up to 4 LE bytes */
            w |= (uint32_t)s[i + got] << (8u * got);
        for (uint32_t k = got; k < 4u; ++k)          /* pad tail with 0xFF        */
            w |= 0xFFu << (8u * k);
        *d = w;                                       /* program one word          */
        while (FLASH_SR & SR_BSY) { }
        if (FLASH_SR & SR_ERR_FLAGS) { rc = -1; break; }
        ++d;
        i += 4u;
    }

    FLASH_CR &= ~CR_PG;
    return rc;
}

void flash_cache_flush(void) {
    uint32_t acr = FLASH_ACR;
    FLASH_ACR = acr & ~(ACR_ICEN | ACR_DCEN);   /* disable caches               */
    FLASH_ACR |= (ACR_ICRST | ACR_DCRST);        /* assert resets (clears them)  */
    FLASH_ACR &= ~(ACR_ICRST | ACR_DCRST);       /* deassert resets              */
    FLASH_ACR = acr;                              /* restore original enables     */
}
