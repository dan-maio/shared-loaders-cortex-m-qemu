#ifndef W25Q64_H_
#define W25Q64_H_

#include <cstdint>

class w25q64
{
public:
  w25q64();

  void
  power_up();

  bool
  read_id(uint8_t* mid, uint8_t* tid);

  void
  sector_erase(uint32_t addr);

  bool
  page_program(uint32_t addr, const uint8_t* data, uint16_t len);

  bool
  write(uint32_t addr, const uint8_t* data, uint32_t len);

  void
  read(uint32_t addr, uint8_t* data, uint32_t len);

private:
  void
  write_enable();

  uint8_t
  read_status();

  void
  wait_busy();

  void
  cs_low();

  void
  cs_high();
};

#endif
