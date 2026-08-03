#ifndef SPI_DRV_H
#define SPI_DRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spi_init(void);
void spi_format(void);
int  spi_read(uint32_t addr, uint8_t* data, uint16_t len);
int  spi_write(uint32_t addr, const uint8_t* data, uint32_t len);
void spi_erase_sector(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
