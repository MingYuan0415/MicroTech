/** @file ESP-IDF attribute compatibility macros for the simulator. */
#ifndef __SIM_ESP_ATTR_H__
#define __SIM_ESP_ATTR_H__

#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define SPI_FLASH_RW_ATTR
#define DRAM_DEFAULT_ATTR
#define ESP_ALIGN_ATTRIBUTE(x) __attribute__((aligned(x)))
#define ALWAYS_INLINE_ATTR inline __attribute__((always_inline))

#endif /* __SIM_ESP_ATTR_H__ */
