#include "spi-flash.hpp"
#include <cstdint>

#define REG32(a) (*(volatile uint32_t*)(a))

#define RCC_AHB1ENR     REG32(0x40023830u)
#define GPIOA_MODER     REG32(0x40020000u)
#define GPIOA_BSRR      REG32(0x40020018u)
#define GPIOA_IDR       REG32(0x40020010u)
#define GPIOA_PUPDR     REG32(0x4002000Cu)

#define CS   4u
#define SCK  5u
#define MISO 6u
#define MOSI 7u

static void cs_low(void)  { GPIOA_BSRR = (1u << (CS + 16)); }
static void cs_high(void) { GPIOA_BSRR = (1u << CS); }
static void sck_hi(void)  { GPIOA_BSRR = (1u << SCK); }
static void sck_lo(void)  { GPIOA_BSRR = (1u << (SCK + 16)); }
static void mosi_hi(void) { GPIOA_BSRR = (1u << MOSI); }
static void mosi_lo(void) { GPIOA_BSRR = (1u << (MOSI + 16)); }
static int  miso_read(void) { return (GPIOA_IDR >> MISO) & 1; }

static uint8_t xfer(uint8_t d) {
    uint8_t r = 0;
    for (int b = 7; b >= 0; b--) {
        uint32_t mosi_val = (1u << (MOSI + 16)) + (((uint32_t)(d >> b) & 1u) << MOSI);
        GPIOA_BSRR = mosi_val;
        GPIOA_BSRR = (1u << SCK);
        r = (r << 1) | ((GPIOA_IDR >> MISO) & 1);
        GPIOA_BSRR = (1u << (SCK + 16));
    }
    return r;
}

void spi_flash_init(void) {
    RCC_AHB1ENR |= (1u << 0);
    __asm__ volatile("dsb" ::: "memory");
    GPIOA_MODER = (GPIOA_MODER & ~((3u<<8)|(3u<<10)|(3u<<14)))
                | ((1u<<8)|(1u<<10)|(1u<<14));
    GPIOA_MODER &= ~(3u << (MISO * 2));
    GPIOA_PUPDR = (GPIOA_PUPDR & ~((3u<<8)|(3u<<10)|(3u<<12)|(3u<<14)))
                | ((1u<<8)|(1u<<10)|(1u<<12)|(1u<<14));
    cs_high(); sck_lo(); mosi_hi();
}

void spi_flash_get_sr(uint32_t* sr, uint32_t* cr1) { (void)sr; (void)cr1; }

static void write_enable(void) {
    cs_low(); xfer(0x06); cs_high();
}

static void wait_busy(void) {
    do {
        cs_low(); xfer(0x05); uint8_t s = xfer(0xFF); cs_high();
        if (!(s & 1)) break;
    } while (1);
}

int spi_flash_read_id(uint8_t* mid, uint8_t* tid) {
    cs_low();
    xfer(0x9F);
    *mid = xfer(0xFF);
    xfer(0xFF);
    *tid = xfer(0xFF);
    cs_high();
    return (*mid == 0xEF) ? 0 : -1;
}

void spi_flash_sector_erase(uint32_t addr) {
    write_enable();
    cs_low();
    xfer(0x20);
    xfer((addr >> 16) & 0xFF);
    xfer((addr >> 8) & 0xFF);
    xfer(addr & 0xFF);
    cs_high();
    wait_busy();
}

int spi_flash_page_program(uint32_t addr, const uint8_t* data, uint16_t len) {
    if (len > 256) return -1;
    write_enable();
    cs_low();
    xfer(0x02);
    xfer((addr >> 16) & 0xFF);
    xfer((addr >> 8) & 0xFF);
    xfer(addr & 0xFF);
    for (uint16_t i = 0; i < len; i++) xfer(data[i]);
    cs_high();
    wait_busy();
    return 0;
}

int spi_flash_write(uint32_t addr, const uint8_t* data, uint32_t len) {
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t page = addr + pos;
        uint32_t page_off = page & 0xFF;
        uint32_t chunk = 256 - page_off;
        if (chunk > len - pos) chunk = len - pos;
        if (spi_flash_page_program(page, data + pos, (uint16_t)chunk))
            return -1;
        pos += chunk;
    }
    return 0;
}

void spi_flash_read(uint32_t addr, uint8_t* data, uint16_t len) {
    cs_low();
    xfer(0x03);
    xfer((addr >> 16) & 0xFF);
    xfer((addr >> 8) & 0xFF);
    xfer(addr & 0xFF);
    for (uint16_t i = 0; i < len; i++) data[i] = xfer(0xFF);
    cs_high();
}
