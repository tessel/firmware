// Copyright 2014 Technical Machine, Inc. See the COPYRIGHT
// file at the top-level directory of this distribution.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#include "hw.h"
#include "tm.h"
#include "colony.h"

static const uint8_t tx_chan = 0;
static const uint8_t rx_chan = 1;

void async_spi_callback();
void default_complete_cb();
void hw_spi_transfer_step();
/// The event triggered by the timer callback
tm_event async_spi_event = TM_EVENT_INIT(async_spi_callback);

struct spi_async_status_t spi_async_status = {
  .txRef = LUA_NOREF,
  .rxRef = LUA_NOREF,
  .buffer_length = 0,
  .tx_Linked_List = 0,
  .rx_Linked_List = 0,
  .callback = &default_complete_cb,
  .cs_delay_us = 0,
};

void hw_spi_async_status_reset () {
  spi_async_status.tx_Linked_List = 0;
  spi_async_status.rx_Linked_List = 0;
  spi_async_status.buffer_length = 0;
  spi_async_status.txRef = LUA_NOREF;
  spi_async_status.rxRef = LUA_NOREF;
  spi_async_status.callback = &default_complete_cb;
}

void hw_spi_dma_counter (uint8_t channel){
  // Check counter terminal IR
  if(GPDMA_IntGetStatus(GPDMA_STAT_INTTC, channel)){
    // Clear terminate counter Interrupt pending
    GPDMA_ClearIntPending (GPDMA_STATCLR_INTTC, channel);
    // This interrupt status should be hit for both tx and rx
    uint8_t done_count = ((spi_async_status.txbuf != NULL) && (spi_async_status.rxbuf != NULL)) ? 2 : 1;
    
    if (++spi_async_status.transferCount == done_count) {
      // Trigger the callback
      (*spi_async_status.callback)();
    }
  }
  if (GPDMA_IntGetStatus(GPDMA_STAT_INTERR, channel)){
    // Clear error counter Interrupt pending
    GPDMA_ClearIntPending (GPDMA_STATCLR_INTERR, channel);
    // Register the error in our struct
    spi_async_status.transferError++;
    // Trigger the callback because there was an error
    (*spi_async_status.callback)();
  }
}

void default_complete_cb() {
  // Pull cs pin high if a cs pin is set
  if (spi_async_status.chip_select != -1) {
    hw_digital_write(spi_async_status.chip_select, 1);
  }
  // Set offset to next command in the txbuf
  spi_async_status.chunk_offset += spi_async_status.chunk_size;

  if ((spi_async_status.chunk_offset + spi_async_status.chunk_size) > spi_async_status.buffer_length) {
    spi_async_status.chunk_offset = 0;
    
    if (--spi_async_status.repeat == 0) {
      tm_event_trigger(&async_spi_event);
      return;
    }
  }
  
  if (spi_async_status.chip_select != -1) {
    hw_wait_us(spi_async_status.cs_delay_us);
  }
  
  hw_spi_transfer_step();
}

uint32_t hw_spi_dma_num_linked_lists (size_t buf_len) {
  // Get the number of complete packets (0xFFF bytes)
  uint32_t num_full_packets = buf_len/SPI_MAX_DMA_SIZE;
  // Get the number of bytes in final packets (if not a multiple of 0xFFF)
  uint32_t num_remaining_bytes = buf_len % SPI_MAX_DMA_SIZE;
  // Get the total number of packets including incomplete packets
  return num_full_packets + (num_remaining_bytes ? 1 : 0);
}

hw_GPDMA_Linked_List_Type * hw_spi_dma_packetize_setup (size_t buf_len) {

  // Generate an array of linked lists to send (will need to be freed)
  hw_GPDMA_Linked_List_Type * linked_list = calloc(hw_spi_dma_num_linked_lists(buf_len), sizeof(hw_GPDMA_Linked_List_Type));

  return linked_list;
}

void hw_spi_dma_packetize_step (hw_GPDMA_Linked_List_Type * linked_list, size_t buf_len, const uint8_t *source, uint8_t *destination, uint8_t txBool) {
  uint32_t base_control;

  uint32_t num_linked_lists = hw_spi_dma_num_linked_lists(buf_len);
  uint32_t num_remaining_bytes = buf_len % SPI_MAX_DMA_SIZE;

  // If we are transmitting
  if (txBool) {
    // Use dest AHB Master and source increment
    base_control =  ((1UL<<25)) | ((1UL<<26));
  }
  // If we are receiving
  else {
    // Use source AHB Master and dest increment
    base_control = ((1UL<<24)) | ((1UL<<27));
  }

    // For each packet
  for (uint32_t i = 0; i < num_linked_lists; i++) {
    // Set the source to the same as rx config source
    linked_list[i].Source = (uint32_t) source;
    // Set the detination to where our reading pointer is located
    linked_list[i].Destination = (uint32_t) destination;
    // Se tthe control to read 4 byte bursts with max buf len, and increment the destination counter
    linked_list[i].Control = base_control;

    // If this is the last packet
    if (i == num_linked_lists-1) {
      // Set this as the end of linked list
      linked_list[i].NextLLI = (uint32_t)0;
      // Set the interrupt control bit
      linked_list[i].Control |= ((1UL<<31));
      // If the packet has fewer then max bytes
      if (num_remaining_bytes) {
        // Set those as num to read with interrupt at the end!
        linked_list[i].Control = num_remaining_bytes | base_control | ((1UL<<31));
      }
    }
    // If it's not the last packet
    else {
      // Set the maximum number of bytes to read
      linked_list[i].Control |= SPI_MAX_DMA_SIZE;

      // Point next linked list item to subsequent packet
      linked_list[i].NextLLI = (uint32_t) &linked_list[i + 1];

      // If this is a transmit buffer
      if (txBool) {
        // Increment source address
        source += SPI_MAX_DMA_SIZE;
      }
      // If this is an receive buffer
      else {
        // Increment the destination address
        destination += SPI_MAX_DMA_SIZE;
      }
    }
  }
}

void hw_spi_dma_packetize_cleanup (hw_GPDMA_Linked_List_Type * linked_list) {
  free(linked_list);
}

void hw_spi_async_cancel_transfers () {
  hw_gpdma_cancel_transfer(0);
  hw_gpdma_cancel_transfer(1);
}

void hw_spi_async_cleanup () {
  if (tm_lua_state != 0) {
    // Unreference our buffers so they can be garbage collected
    luaL_unref(tm_lua_state, LUA_REGISTRYINDEX, spi_async_status.txRef);
    luaL_unref(tm_lua_state, LUA_REGISTRYINDEX, spi_async_status.rxRef);
  }
  
  // Free our linked list 
  hw_spi_dma_packetize_cleanup(spi_async_status.tx_Linked_List);
  hw_spi_dma_packetize_cleanup(spi_async_status.rx_Linked_List);

  // Clear our config struct
  hw_spi_async_status_reset();

  // If there are any current transfers, stop them
  hw_spi_async_cancel_transfers();

  // Unreference the event to free up the event queue
  tm_event_unref(&async_spi_event);
}

void async_spi_callback (void) {
  // Make sure the Lua state exists
  lua_State* L = tm_lua_state;
  if (!L) return;

  // Push the _colony_emit helper function onto the stack
  lua_getglobal(L, "_colony_emit");
  // The process message identifier
  lua_pushstring(L, "spi_async_complete");
  // push whether we got an error (1 or 0)
  lua_pushnumber(L, spi_async_status.transferError);
  // Clean up our vars so that we can do this again
  hw_spi_async_cleanup();
  // Call _colony_emit to run the JS callback
  tm_checked_call(L, 2);
}

int hw_spi_transfer_setup (size_t port, size_t buffer_length, const uint8_t *txbuf, uint8_t *rxbuf, 
  uint32_t txref, uint32_t rxref, size_t chunk_size, uint32_t repeat, uint8_t chip_select, uint32_t cs_delay_us, void (*callback)())
{
  hw_spi_t *SPIx = find_spi(port);

  // Set the source and destination connection items based on port
  if (SPIx->port == LPC_SSP0){
    spi_async_status.tx_config.DestConn = SSP0_TX_CONN;
    spi_async_status.rx_config.SrcConn = SSP0_RX_CONN;
  } 
  else {
    spi_async_status.tx_config.DestConn = SSP1_TX_CONN;
    spi_async_status.rx_config.SrcConn = SSP1_RX_CONN;
  }

  // This allows C hooks to be provided as callbacks
  if (callback) {
    spi_async_status.callback = callback;
  }
  else {
    spi_async_status.callback = &default_complete_cb;
  }

  // Save the length that we're transferring
  spi_async_status.buffer_length = buffer_length;
  spi_async_status.txRef = txref;
  spi_async_status.rxRef = rxref;
  spi_async_status.transferCount = 0;
  spi_async_status.transferError = 0;
  spi_async_status.txbuf = txbuf;
  spi_async_status.rxbuf = rxbuf;
  spi_async_status.chunk_size = chunk_size;
  spi_async_status.repeat = repeat;
  spi_async_status.chunk_offset = 0;
  spi_async_status.chip_select = chip_select;
  spi_async_status.cs_delay_us = cs_delay_us;

  if (rxbuf != NULL) {
    // Destination connection - unused
    spi_async_status.rx_config.DestConn = 0;
    // Transfer type
    spi_async_status.rx_config.TransferType = p2m;

    // Configure the rx transfer on channel 1
    // TODO: Get next available channel
    hw_gpdma_transfer_config(rx_chan, &spi_async_status.rx_config);

    // Generate the linked list structure for receiving
    spi_async_status.rx_Linked_List = hw_spi_dma_packetize_setup(buffer_length); 
  }

  if (txbuf != NULL) {
     // Source Connection - unused
    spi_async_status.tx_config.SrcConn = 0;
    // Transfer type
    spi_async_status.tx_config.TransferType = m2p;

    // Configure the tx transfer on channel 0
    // TODO: Get next available channel
    hw_gpdma_transfer_config(tx_chan, &spi_async_status.tx_config);

    // Generate the linked list structure for transmission
    spi_async_status.tx_Linked_List = hw_spi_dma_packetize_setup(buffer_length); 
  }

  // if it's a slave pull down CS
  if (SPIx->is_slave) {
    scu_pinmux(0xF, 1u, PUP_DISABLE | PDN_ENABLE |  MD_EZI | MD_ZI | MD_EHS, FUNC2);  //  SSP0 SSEL0
  }

  // Tell the runtime to keep the event loop active until this event is done
  tm_event_ref(&async_spi_event);

  return 0;
}

void hw_spi_transfer_step() {
  // Pull cs pin low if a cs pin is set
  if (spi_async_status.chip_select != -1) {
    hw_digital_write(spi_async_status.chip_select, 0);
    hw_wait_us(spi_async_status.cs_delay_us);
  }
  spi_async_status.transferCount = 0;
  spi_async_status.transferError = 0;

  if (spi_async_status.rxbuf != NULL) {
    hw_spi_dma_packetize_step(spi_async_status.rx_Linked_List, spi_async_status.chunk_size, hw_gpdma_get_lli_conn_address(spi_async_status.rx_config.SrcConn), spi_async_status.rxbuf + spi_async_status.chunk_offset, 0);

    // Begin the reception
    hw_gpdma_transfer_begin(rx_chan, spi_async_status.rx_Linked_List); 
  }

  if (spi_async_status.txbuf != NULL) {
    hw_spi_dma_packetize_step(spi_async_status.tx_Linked_List, spi_async_status.chunk_size, spi_async_status.txbuf + spi_async_status.chunk_offset, hw_gpdma_get_lli_conn_address(spi_async_status.tx_config.DestConn), 1);

    // Begin the transmission
    hw_gpdma_transfer_begin(tx_chan, spi_async_status.tx_Linked_List);
  }
}

int hw_spi_transfer (size_t port, size_t buffer_length, const uint8_t *txbuf, uint8_t *rxbuf, uint32_t txref, uint32_t rxref, size_t chunk_size, uint32_t repeat, int8_t chip_select, uint32_t cs_delay_us, void (*callback)())
{
  // Make sure that cs pin is high to start
  if (chip_select != -1) {
    hw_digital_output(chip_select);
    hw_digital_write(chip_select, 1);
  }
  hw_spi_transfer_setup (port, buffer_length, txbuf, rxbuf, txref, rxref, chunk_size, repeat, chip_select, cs_delay_us, callback);
  if (repeat == 0 || chunk_size > buffer_length) {
    tm_event_trigger(&async_spi_event);
  }
  hw_spi_transfer_step();
  return 0;
}
