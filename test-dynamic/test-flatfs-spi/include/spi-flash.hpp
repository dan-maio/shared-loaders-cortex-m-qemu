#ifndef SPI_FLASH_H_
#define SPI_FLASH_H_

#include <stdint.h>

void spi_flash_init(void);
int  spi_flash_read_id(uint8_t* mid, uint8_t* tid);
void spi_flash_sector_erase(uint32_t addr);
int  spi_flash_page_program(uint32_t addr, const uint8_t* data, uint16_t len);
int  spi_flash_write(uint32_t addr, const uint8_t* data, uint32_t len);
void spi_flash_read(uint32_t addr, uint8_t* data, uint16_t len);
void spi_flash_get_sr(uint32_t* sr, uint32_t* cr1);

#endif
