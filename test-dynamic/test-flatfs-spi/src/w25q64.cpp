#include "w25q64.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"

#define CS_PORT   GPIOA
#define CS_PIN    GPIO_PIN_4

static SPI_HandleTypeDef spi_handle;

w25q64::w25q64()
{
}

void w25q64::cs_low()  { HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_RESET); }
void w25q64::cs_high() { HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET); }

void w25q64::power_up()
{
  __HAL_RCC_SPI1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  GPIO_InitTypeDef gpio;
  gpio.Pin = CS_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(CS_PORT, &gpio);
  cs_high();

  gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_6;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOA, &gpio);

  spi_handle.Instance = SPI1;
  spi_handle.Init.Mode = SPI_MODE_MASTER;
  spi_handle.Init.Direction = SPI_DIRECTION_2LINES;
  spi_handle.Init.DataSize = SPI_DATASIZE_8BIT;
  spi_handle.Init.CLKPolarity = SPI_POLARITY_LOW;
  spi_handle.Init.CLKPhase = SPI_PHASE_1EDGE;
  spi_handle.Init.NSS = SPI_NSS_SOFT;
  spi_handle.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  spi_handle.Init.FirstBit = SPI_FIRSTBIT_MSB;
  spi_handle.Init.TIMode = SPI_TIMODE_DISABLE;
  spi_handle.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  spi_handle.Init.CRCPolynomial = 10;
  HAL_SPI_Init(&spi_handle);
}

bool w25q64::read_id(uint8_t* mid, uint8_t* tid)
{
  uint8_t cmd = 0x9F;
  uint8_t rx[3];
  cs_low();
  HAL_SPI_TransmitReceive(&spi_handle, &cmd, &cmd, 1, 100);
  HAL_SPI_TransmitReceive(&spi_handle, rx, rx, 3, 100);
  cs_high();
  *mid = rx[0];
  *tid = rx[2];
  return rx[0] == 0xEF;
}

static void tx(uint8_t* buf, uint16_t len) {
  uint8_t dummy[32];
  while (len > 0) {
    uint16_t chunk = len > sizeof(dummy) ? sizeof(dummy) : len;
    HAL_SPI_TransmitReceive(&spi_handle, buf, dummy, chunk, 100);
    buf += chunk;
    len -= chunk;
  }
}

void w25q64::write_enable()
{
  uint8_t cmd = 0x06;
  cs_low(); tx(&cmd, 1); cs_high();
}

uint8_t w25q64::read_status()
{
  uint8_t cmd = 0x05;
  uint8_t val = 0;
  cs_low();
  HAL_SPI_TransmitReceive(&spi_handle, &cmd, &cmd, 1, 100);
  HAL_SPI_TransmitReceive(&spi_handle, &val, &val, 1, 100);
  cs_high();
  return val;
}

void w25q64::wait_busy()
{
  while (read_status() & 0x01);
}

void w25q64::sector_erase(uint32_t addr)
{
  write_enable();
  uint8_t cmd[4] = { 0x20, uint8_t((addr >> 16) & 0xFF), uint8_t((addr >> 8) & 0xFF), uint8_t(addr & 0xFF) };
  cs_low(); tx(cmd, 4); cs_high();
  wait_busy();
}

bool w25q64::page_program(uint32_t addr, const uint8_t* data, uint16_t len)
{
  if (len > 256) return false;
  write_enable();
  uint8_t hdr[4] = { 0x02, uint8_t((addr >> 16) & 0xFF), uint8_t((addr >> 8) & 0xFF), uint8_t(addr & 0xFF) };
  cs_low();
  tx(hdr, 4);
  tx((uint8_t*)data, len);
  cs_high();
  wait_busy();
  return true;
}

void w25q64::read(uint32_t addr, uint8_t* data, uint32_t len)
{
  uint8_t hdr[4] = { 0x03, uint8_t((addr >> 16) & 0xFF), uint8_t((addr >> 8) & 0xFF), uint8_t(addr & 0xFF) };
  cs_low(); tx(hdr, 4);
  uint32_t pos = 0;
  while (pos < len) {
    uint16_t chunk = 65535;
    if (chunk > len - pos) chunk = len - pos;
    HAL_SPI_Receive(&spi_handle, data + pos, chunk, 100);
    pos += chunk;
  }
  cs_high();
}

bool w25q64::write(uint32_t addr, const uint8_t* data, uint32_t len)
{
  uint32_t pos = 0;
  while (pos < len) {
    uint32_t page = addr + pos;
    uint32_t page_off = page & 0xFF;
    uint16_t chunk = 256 - page_off;
    if (chunk > len - pos) chunk = len - pos;
    if (!page_program(page, data + pos, chunk)) return false;
    pos += chunk;
  }
  return true;
}
