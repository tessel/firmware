// Copyright 2014 Technical Machine. See the COPYRIGHT
// file at the top-level directory of this distribution.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

// Tessel-specific initialization functions (gpio) and board/system version.

#ifndef TESSEL_H_
#define TESSEL_H_

#include <stddef.h>

// dedicated pin names
#include "variant.h" 

int tessel_board_version();
char* tessel_board_uuid ();
extern const char* version_info;
extern uint8_t* update_from_flash;
extern size_t update_from_flash_len;

void tessel_gpio_init (int cc3k_leds);
void tessel_network_reset ();

int output_write (uint8_t *ptr, int len);


typedef struct {
	enum {
		TESSEL_CMD_NONE,
		TESSEL_CMD_HEADER,
		TESSEL_CMD_BODY,
		TESSEL_CMD_EXE
	} state;

	union {
		uint8_t header[5];
		struct {
			char type;
			uint32_t size;
		} __attribute__((packed));
	} __attribute__((packed));
	
	size_t header_index;

	size_t index;
	uint8_t* buf;
} tessel_cmd_buf_t;

void tessel_cmd_process (uint8_t cmd, uint8_t* buf, unsigned length);

int populate_fs (const uint8_t *file, size_t len);

int debugstack();


/**
 * SPI
 */

// SPI0 is peripherals, SPI1 is CC3K
typedef enum {
	TESSEL_SPI_0 = 0,
	TESSEL_SPI_1 = 1
} tessel_spi_t;


#endif /* TESSEL_H_ */
