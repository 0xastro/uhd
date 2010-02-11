//
// Copyright 2010 Ettus Research LLC
//
/* -*- c++ -*- */
/*
 * Copyright 2007,2008,2009 Free Software Foundation, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app_common_v2.h"
#include "buffer_pool.h"
#include "memcpy_wa.h"
#include "ethernet.h"
#include "nonstdio.h"
#include "print_rmon_regs.h"
#include "db.h"
#include "clocks.h"
#include "u2_init.h"
#include <string.h>
#include <stddef.h>
#include "usrp2_fw_common.h"

volatile bool link_is_up = false;	// eth handler sets this
int cpu_tx_buf_dest_port = PORT_ETH;

// If this is non-zero, this dbsm could be writing to the ethernet
dbsm_t *ac_could_be_sending_to_eth;

static unsigned char exp_seqno __attribute__((unused)) = 0;

static bool
set_time(const op_set_time_t *p)
{
  //printf("Setting time: secs %u, ticks %u\n", p->time_secs, p->time_ticks);
  //sr_time64->secs = p->time_secs; //set below...
  sr_time64->ticks = p->time_ticks;
  switch (p->type){
  case OP_SET_TIME_TYPE_NOW:
    sr_time64->imm = 1;
    break;
  case OP_SET_TIME_TYPE_PPS:
    sr_time64->imm = 0;
    break;
  }
  sr_time64->secs = p->time_secs; //set this last to latch the regs
  return true;
}

static inline bool
config_clock_cmd(const op_config_clock_t *p)
{
  //handle the 10 mhz ref source
  clocks_mimo_config(p->flags & MC_REF_CLK_MASK);

  //handle the pps config
  uint32_t pps_flags = 0;
  if (p->flags & MC_PPS_POLARITY_NEG) pps_flags |= 0x00 << 0;
  if (p->flags & MC_PPS_POLARITY_POS) pps_flags |= 0x01 << 0;
  if (p->flags & MC_PPS_SOURCE_SMA)   pps_flags |= 0x00 << 1;
  if (p->flags & MC_PPS_SOURCE_MIMO)  pps_flags |= 0x01 << 1;
  sr_time64->flags = pps_flags;

  return true;
}

static size_t
op_id_cmd(const op_generic_t *p,
	  void *reply_payload, size_t reply_payload_space)
{
  op_id_reply_t *r = (op_id_reply_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))	// no room
    return 0;

  // Build reply subpacket

  r->opcode = OP_ID_REPLY;
  r->len = sizeof(op_id_reply_t);
  r->rid = p->rid;
  r->addr = *ethernet_mac_addr();
  r->hw_rev = (u2_hw_rev_major << 8) | u2_hw_rev_minor;
  // r->fpga_md5sum = ;	// FIXME
  // r->sw_md5sum = ;	// FIXME

  return r->len;
}


static size_t
config_tx_v2_cmd(const op_config_tx_v2_t *p,
		 void *reply_payload, size_t reply_payload_space)
{
  op_config_tx_reply_v2_t *r = (op_config_tx_reply_v2_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))
    return 0;					// no room

  struct tune_result	tune_result;
  memset(&tune_result, 0, sizeof(tune_result));

  bool ok = true;
  
  if (p->valid & CFGV_GAIN){
    //ok &= db_set_gain(tx_dboard, p->gain);
  }

  if (p->valid & CFGV_FREQ){
    bool was_streaming = is_streaming();
    if (was_streaming)
      stop_rx_cmd();
    
    u2_fxpt_freq_t f = u2_fxpt_freq_from_hilo(p->freq_hi, p->freq_lo);
    bool tune_ok = false;//db_tune(tx_dboard, f, &tune_result);
    ok &= tune_ok;
    print_tune_result("Tx", tune_ok, f, &tune_result);

    if (was_streaming)
      restart_streaming();
  }

  if (p->valid & CFGV_INTERP_DECIM){
    int interp = p->interp;
    int hb1 = 0;
    int hb2 = 0;

    if (!(interp & 1)){
      hb2 = 1;
      interp = interp >> 1;
    }

    if (!(interp & 1)){
      hb1 = 1;
      interp = interp >> 1;
    }
    
    if (interp < MIN_CIC_INTERP || interp > MAX_CIC_INTERP)
      ok = false;
    else {
      dsp_tx_regs->interp_rate = (hb1<<9) | (hb2<<8) | interp;
      // printf("Interp: %d, register %d\n", p->interp, (hb1<<9) | (hb2<<8) | interp);
    }
  }

  if (p->valid & CFGV_SCALE_IQ){
    dsp_tx_regs->scale_iq = p->scale_iq;
  }

  // Build reply subpacket

  r->opcode = OP_CONFIG_TX_REPLY_V2;
  r->len = sizeof(*r);
  r->rid = p->rid;
  r->ok = ok;
  r->inverted = tune_result.inverted;
  r->baseband_freq_hi = u2_fxpt_freq_hi(tune_result.baseband_freq);
  r->baseband_freq_lo = u2_fxpt_freq_lo(tune_result.baseband_freq);
  r->duc_freq_hi = u2_fxpt_freq_hi(tune_result.dxc_freq);
  r->duc_freq_lo = u2_fxpt_freq_lo(tune_result.dxc_freq);
  r->residual_freq_hi = u2_fxpt_freq_hi(tune_result.residual_freq);
  r->residual_freq_lo = u2_fxpt_freq_lo(tune_result.residual_freq);
  return r->len;
}

static size_t
config_rx_v2_cmd(const op_config_rx_v2_t *p, 
		 void *reply_payload, size_t reply_payload_space)
{
  op_config_rx_reply_v2_t *r = (op_config_rx_reply_v2_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))
    return 0;				// no room

  struct tune_result	tune_result;
  memset(&tune_result, 0, sizeof(tune_result));

  bool ok = true;
  
  if (p->valid & CFGV_GAIN){
    //ok &= db_set_gain(rx_dboard, p->gain);
  }

  if (p->valid & CFGV_FREQ){
    bool was_streaming = is_streaming();
    if (was_streaming)
      stop_rx_cmd();
    
    u2_fxpt_freq_t f = u2_fxpt_freq_from_hilo(p->freq_hi, p->freq_lo);
    bool tune_ok = false;//db_tune(rx_dboard, f, &tune_result);
    ok &= tune_ok;
    print_tune_result("Rx", tune_ok, f, &tune_result);

    if (was_streaming)
      restart_streaming();
  }

  if (p->valid & CFGV_INTERP_DECIM){
    int decim = p->decim;
    int hb1 = 0;
    int hb2 = 0;
    
    if(!(decim & 1)) {
      hb2 = 1;
      decim = decim >> 1;
    }
    
    if(!(decim & 1)) {
      hb1 = 1;
      decim = decim >> 1;
    }
    
    if (decim < MIN_CIC_DECIM || decim > MAX_CIC_DECIM)
      ok = false;
    else {
      dsp_rx_regs->decim_rate = (hb1<<9) | (hb2<<8) | decim;
      // printf("Decim: %d, register %d\n", p->decim, (hb1<<9) | (hb2<<8) | decim);
    }
  }

  if (p->valid & CFGV_SCALE_IQ){
    dsp_rx_regs->scale_iq = p->scale_iq;
  }

  // Build reply subpacket

  r->opcode = OP_CONFIG_RX_REPLY_V2;
  r->len = sizeof(*r);
  r->rid = p->rid;
  r->ok = ok;
  r->inverted = tune_result.inverted;
  r->baseband_freq_hi = u2_fxpt_freq_hi(tune_result.baseband_freq);
  r->baseband_freq_lo = u2_fxpt_freq_lo(tune_result.baseband_freq);
  r->ddc_freq_hi = u2_fxpt_freq_hi(tune_result.dxc_freq);
  r->ddc_freq_lo = u2_fxpt_freq_lo(tune_result.dxc_freq);
  r->residual_freq_hi = u2_fxpt_freq_hi(tune_result.residual_freq);
  r->residual_freq_lo = u2_fxpt_freq_lo(tune_result.residual_freq);

  return r->len;
}

static void
fill_db_info(u2_db_info_t *p, const struct db_base *db)
{
  /*p->dbid = db->dbid;
  p->freq_min_hi = u2_fxpt_freq_hi(db->freq_min);
  p->freq_min_lo = u2_fxpt_freq_lo(db->freq_min);
  p->freq_max_hi = u2_fxpt_freq_hi(db->freq_max);
  p->freq_max_lo = u2_fxpt_freq_lo(db->freq_max);
  p->gain_min = db->gain_min;
  p->gain_max = db->gain_max;
  p->gain_step_size = db->gain_step_size;*/
}

static size_t
dboard_info_cmd(const op_generic_t *p,
		void *reply_payload, size_t reply_payload_space)
{
  op_dboard_info_reply_t *r = (op_dboard_info_reply_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))		
    return 0;					// no room

  r->opcode = OP_DBOARD_INFO_REPLY;
  r->len = sizeof(*r);
  r->rid = p->rid;
  r->ok = true;

  //fill_db_info(&r->tx_db_info, tx_dboard);
  //fill_db_info(&r->rx_db_info, rx_dboard);

  return r->len;
}

static size_t
peek_cmd(const op_peek_t *p,
	 void *reply_payload, size_t reply_payload_space)
{
  op_generic_t *r = (op_generic_t *) reply_payload;

  //putstr("peek: addr="); puthex32(p->addr);
  //printf(" bytes=%u\n", p->bytes);

  if ((reply_payload_space < (sizeof(*r) + p->bytes)) ||
      p->bytes > MAX_SUBPKT_LEN - sizeof(op_generic_t)) {
    putstr("peek: insufficient reply packet space\n");
    return 0;			// FIXME do partial read?
  }

  r->opcode = OP_PEEK_REPLY;
  r->len = sizeof(*r)+p->bytes;
  r->rid = p->rid;
  r->ok = true;

  memcpy_wa(reply_payload+sizeof(*r), (void *)p->addr, p->bytes);

  return r->len;
}

static bool
poke_cmd(const op_poke_t *p)
{
  int bytes = p->len - sizeof(*p);
  //putstr("poke: addr="); puthex32(p->addr);
  //printf(" bytes=%u\n", bytes);

  uint8_t *src = (uint8_t *)p + sizeof(*p);
  memcpy_wa((void *)p->addr, src, bytes);

  return true;
}

static bool
set_lo_offset_cmd(const op_freq_t *p)
{
  /*u2_fxpt_freq_t f = u2_fxpt_freq_from_hilo(p->freq_hi, p->freq_lo);
  if (p->opcode == OP_SET_TX_LO_OFFSET)
    return db_set_lo_offset(tx_dboard, f);
  else
    return db_set_lo_offset(rx_dboard, f);
  */return false;
}

static size_t
gpio_read_cmd(const op_gpio_t *p,
	      void *reply_payload, size_t reply_payload_space)
{
  op_gpio_read_reply_t *r = (op_gpio_read_reply_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))	// no room
    return 0;

 // Build reply subpacket

  r->opcode = OP_GPIO_READ_REPLY;
  r->len = sizeof(op_gpio_read_reply_t);
  r->rid = p->rid;
  r->ok = true;
  r->mbz = 0;
  r->value = hal_gpio_read(p->bank);

  return r->len;
}

static size_t
generic_reply(const op_generic_t *p,
	      void *reply_payload, size_t reply_payload_space,
	      bool ok)
{
  op_generic_t *r = (op_generic_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))		
    return 0;					// no room
  
  r->opcode = p->opcode | OP_REPLY_BIT;
  r->len = sizeof(*r);
  r->rid = p->rid;
  r->ok = ok;
  
  return r->len;
}

static size_t
add_eop(void *reply_payload, size_t reply_payload_space)
{
  op_generic_t *r = (op_generic_t *) reply_payload;
  if (reply_payload_space < sizeof(*r))		
    return 0;					// no room
  
  r->opcode = OP_EOP;
  r->len = sizeof(*r);
  r->rid = 0;
  r->ok =  0;
  
  return r->len;
}

/***********************************************************************
 * Global vars to make the control replies
 **********************************************************************/
#define REPLY_PAYLOAD_MAX_LEN (4 * sizeof(u2_subpkt_t))
uint16_t host_dst_udp_port;
uint16_t host_src_udp_port;
struct in_addr host_dst_ip_addr;
struct in_addr host_src_ip_addr;
eth_mac_addr_t host_dst_mac_addr;
eth_mac_addr_t host_src_mac_addr;

/***********************************************************************
 * Handle input control data and produce output control data
 **********************************************************************/
static size_t handle_control_packets(
    const void *data_in, size_t len_in, void *data_out
){
  // point to the begining of outgoing payload (subpackets)
  uint8_t *reply_payload = data_out;
  size_t reply_payload_space = REPLY_PAYLOAD_MAX_LEN;

  // point to beginning of incoming payload (subpackets)
  uint8_t *payload = (uint8_t *)data_in;
  size_t payload_len = len_in;
  
  size_t subpktlen = 0;
  bool ok = false;

  while (payload_len >= sizeof(op_generic_t)){
    const op_generic_t *gp = (const op_generic_t *) payload;
    subpktlen = 0;

    // printf("\nopcode = %d\n", gp->opcode);

    switch(gp->opcode){
    case OP_EOP:		// end of subpackets
      goto end_of_subpackets;

    case OP_ID:
      subpktlen = op_id_cmd(gp, reply_payload, reply_payload_space);
      break;
    
    case OP_CONFIG_TX_V2:
      subpktlen = config_tx_v2_cmd((op_config_tx_v2_t *) payload, reply_payload, reply_payload_space);
      break;

    case OP_CONFIG_RX_V2:
      subpktlen = config_rx_v2_cmd((op_config_rx_v2_t *) payload, reply_payload, reply_payload_space);
      break;

    case OP_START_RX_STREAMING:
      start_rx_streaming_cmd((op_start_rx_streaming_t *) payload);
      ok = true;
      goto generic_reply;
    
    case OP_STOP_RX:
      stop_rx_cmd();
      ok = true;
      goto generic_reply;
    
    case OP_BURN_MAC_ADDR:
      ok = ethernet_set_mac_addr(&((op_burn_mac_addr_t *)payload)->addr);
      goto generic_reply;

    case OP_CONFIG_CLOCK:
      ok = config_clock_cmd((op_config_clock_t *) payload);
      goto generic_reply;

    case OP_DBOARD_INFO:
      subpktlen = dboard_info_cmd(gp, reply_payload, reply_payload_space);
      break;

    case OP_SET_TIME:
      ok = set_time((op_set_time_t *) payload);
      goto generic_reply;

    case OP_PEEK:
      subpktlen = peek_cmd((op_peek_t *)payload, reply_payload, reply_payload_space);
      break;

    case OP_POKE:
      ok = poke_cmd((op_poke_t *)payload);
      goto generic_reply;

    case OP_SET_TX_LO_OFFSET:
    case OP_SET_RX_LO_OFFSET:
      ok = set_lo_offset_cmd((op_freq_t *)payload);
      goto generic_reply;

    case OP_RESET_DB:
      db_init();
      ok = true;
      goto generic_reply;

    case OP_GPIO_SET_DDR:
      ok = true;
      hal_gpio_set_ddr(((op_gpio_t *)payload)->bank, 
		       ((op_gpio_t *)payload)->value, 
		       ((op_gpio_t *)payload)->mask);
      goto generic_reply;

    case OP_GPIO_SET_SELS:
      ok = true;
      hal_gpio_set_sels(((op_gpio_set_sels_t *)payload)->bank,
			(char *)(&((op_gpio_set_sels_t *)payload)->sels));
      goto generic_reply;

    case OP_GPIO_READ:
      subpktlen = gpio_read_cmd((op_gpio_t *) payload, reply_payload, reply_payload_space);
      break;

    case OP_GPIO_WRITE:
      ok = true;
      hal_gpio_write(((op_gpio_t *)payload)->bank, 
		     ((op_gpio_t *)payload)->value, 
		     ((op_gpio_t *)payload)->mask);
      goto generic_reply;

    case OP_GPIO_STREAM:
      ok = true;
      dsp_rx_regs->gpio_stream_enable = (uint32_t)((op_gpio_t *)payload)->value;
      goto generic_reply;

    // Add new opcode handlers here

    generic_reply:
      subpktlen = generic_reply(gp, reply_payload, reply_payload_space, ok);
      break;

    default:
      printf("app_common_v2: unhandled opcode = %d\n", gp->opcode);
      break;
    }

    int t = (gp->len + 3) & ~3;		// bump to a multiple of 4
    payload += t;
    payload_len -= t;

    subpktlen = (subpktlen + 3) & ~3;	// bump to a multiple of 4
    reply_payload += subpktlen;
    reply_payload_space -= subpktlen;
  }

  end_of_subpackets:

  // add the EOP marker
  subpktlen = add_eop(reply_payload, reply_payload_space);
  subpktlen = (subpktlen + 3) & ~3;	// bump to a multiple of 4
  reply_payload += subpktlen;
  reply_payload_space -= subpktlen;

  // how big the payload?
  return REPLY_PAYLOAD_MAX_LEN - reply_payload_space;
}

static void handle_control_packet(
    const usrp2_ctrl_data_t *data_in, usrp2_ctrl_data_t *data_out
){
    
}

static void
send_reply(void *reply, size_t reply_len)
{
  if (reply_len < 64) reply_len = 64;

  // wait for buffer to become idle
  hal_set_leds(0x4, 0x4);
  while((buffer_pool_status->status & BPS_IDLE(CPU_TX_BUF)) == 0)
    ;
  hal_set_leds(0x0, 0x4);

  // copy reply into CPU_TX_BUF
  memcpy_wa(buffer_ram(CPU_TX_BUF), reply, reply_len);

  // wait until nobody else is sending to the ethernet
  if (ac_could_be_sending_to_eth){
    hal_set_leds(0x8, 0x8);
    dbsm_wait_for_opening(ac_could_be_sending_to_eth);
    hal_set_leds(0x0, 0x8);
  }

  if (0){
    printf("sending_reply to port %d, len = %d\n", cpu_tx_buf_dest_port, (int)reply_len);
    print_buffer(buffer_ram(CPU_TX_BUF), reply_len/4);
  }

  // fire it off
  bp_send_from_buf(CPU_TX_BUF, cpu_tx_buf_dest_port, 1, 0, reply_len/4);

  // wait for it to complete (not long, it's a small pkt)
  while((buffer_pool_status->status & (BPS_DONE(CPU_TX_BUF) | BPS_ERROR(CPU_TX_BUF))) == 0)
    ;

  bp_clear_buf(CPU_TX_BUF);
}

static uint16_t 
chksum_buffer(void *buff_, size_t len)
{
  uint16_t *buff = (uint16_t *) buff_;

  // sum the individual 16 bit words
  uint32_t sum = 0;
  for (size_t i = 0; i < len/sizeof(uint16_t); i++){
    sum += buff[i];
  }

  // take only 16 bits out of the 32 bit sum and add up the carries
  while (sum >> 16){
    sum = (sum & 0xffff) + (sum >> 16);
  }

  // one's complement the result
  return ~sum;
}

static struct in_addr
create_ip_from_host(struct in_addr addr){
    //get an address that looks like the hosts
    uint8_t low_byte = addr.s_addr & 0xff;
    low_byte += 1;
    if (low_byte == 0xff) low_byte = 0;
    if (low_byte == 0x00) low_byte = 1;
    addr.s_addr = (addr.s_addr & ~0xff) | low_byte;
    return addr;
}

static void
handle_control_chan_frame(u2_eth_ip_udp_t *pkt, size_t len)
{
  // setup reply
  struct {
    uint32_t ctrl_word;
    u2_eth_ip_udp_t hdr;
    usrp2_ctrl_data_t data;
  } reply _AL4;
  memset(&reply, 0, sizeof(reply));

  // process the control data
  handle_control_packet(
    (usrp2_ctrl_data_t*)((uint8_t*)pkt + sizeof(u2_eth_ip_udp_t)),
    &reply.data
  );
  size_t total_len = sizeof(reply);
  reply.ctrl_word = total_len;

  // load the ethernet header
  reply.hdr.eth.dst = host_dst_mac_addr;
  reply.hdr.eth.src = host_src_mac_addr;
  reply.hdr.eth.ethertype = ETHERTYPE_IPV4;

  // load the ip header
  reply.hdr.ip.ip_hl = sizeof(u2_ipv4_hdr_t)/sizeof(uint32_t);
  reply.hdr.ip.ip_v = 4;
  reply.hdr.ip.ip_tos = 0;
  reply.hdr.ip.ip_len = sizeof(u2_ipv4_hdr_t) + sizeof(u2_udp_hdr_t) + sizeof(usrp2_ctrl_data_t);
  reply.hdr.ip.ip_id = 0;
  reply.hdr.ip.ip_off = IP_DF;
  reply.hdr.ip.ip_ttl = 255;
  reply.hdr.ip.ip_p = IP_PROTO_UDP;
  reply.hdr.ip.ip_sum = 0;
  reply.hdr.ip.ip_src = host_src_ip_addr;
  reply.hdr.ip.ip_dst = host_dst_ip_addr;
  reply.hdr.ip.ip_sum = chksum_buffer(&reply.hdr.ip, sizeof(u2_ipv4_hdr_t));

  // load the udp header
  reply.hdr.udp.src_port = pkt->udp.dst_port;
  reply.hdr.udp.dst_port = pkt->udp.src_port;
  reply.hdr.udp.length = sizeof(u2_udp_hdr_t) + sizeof(usrp2_ctrl_data_t);
  reply.hdr.udp.checksum = 0;

  //send the reply
  send_reply(&reply, total_len);
}


/*
 * Called when an ethernet packet is received.
 * Return true if we handled it here, otherwise
 * it'll be passed on to the DSP Tx pipe
 */
bool
eth_pkt_inspector(dbsm_t *sm, int bufno)
{
  u2_eth_ip_udp_t *pkt = (u2_eth_ip_udp_t *) buffer_ram(bufno);
  size_t byte_len = (buffer_pool_status->last_line[bufno] - 3) * 4;
  printf("Got an eth packet of len %d\n", (int)byte_len);

  if (pkt->eth.ethertype != ETHERTYPE_IPV4)
    return true; // ignore, probably bogus PAUSE frame from MAC

  // inspect rcvd frame and figure out what do do.
  switch (pkt->udp.dst_port){

  case USRP2_UDP_CTRL_PORT:
    //record the ip and mac addrs (used when setting up data init)
    host_dst_ip_addr = pkt->ip.ip_src;
    host_src_ip_addr = create_ip_from_host(pkt->ip.ip_src);
    host_dst_mac_addr = pkt->eth.src;
    host_src_mac_addr = *ethernet_mac_addr();
    //handle_control_chan_frame(pkt, byte_len);
    return true;

  case USRP2_UDP_DATA_PORT:
    //record the udp data ports (used when setting up data init)
    host_dst_udp_port = pkt->udp.src_port;
    host_src_udp_port = pkt->udp.dst_port;
    return false;	// pass it on to Tx DSP

  }
  return true;	// ignore, whatever
}

/*
 * Called when eth phy state changes (w/ interrupts disabled)
 */
void
link_changed_callback(int speed)
{
  link_is_up = speed != 0;
  hal_set_leds(link_is_up ? LED_RJ45 : 0x0, LED_RJ45);
  printf("\neth link changed: speed = %d\n", speed);
}


void
print_tune_result(char *msg, bool tune_ok,
		  u2_fxpt_freq_t target_freq, struct tune_result *r)
{
#if 0
  printf("db_tune %s %s\n", msg, tune_ok ? "true" : "false");
  putstr("  target_freq   "); print_fxpt_freq(target_freq); newline();
  putstr("  baseband_freq "); print_fxpt_freq(r->baseband_freq); newline();
  putstr("  dxc_freq      "); print_fxpt_freq(r->dxc_freq); newline();
  putstr("  residual_freq "); print_fxpt_freq(r->residual_freq); newline();
  printf("  inverted      %s\n", r->inverted ? "true" : "false");
#endif
}
