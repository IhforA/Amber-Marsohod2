/*----------------------------------------------------------------
//                                                              //
//  boot-loader-ethmac.c                                        //
//                                                              //
//  This file is part of the Amber project                      //
//  http://www.opencores.org/project,amber                      //
//                                                              //
//  Description                                                 //
//  The main functions for the boot loader application. This    //
//  application is embedded in the FPGA's SRAM and is used      // 
//  to load larger applications into the DDR3 memory on         //
//  the development board.                                      //
//                                                              //
//  Author(s):                                                  //
//      - Conor Santifort, csantifort.amber@gmail.com           //
//                                                              //
//////////////////////////////////////////////////////////////////
//                                                              //
// Copyright (C) 2011 Authors and OPENCORES.ORG                 //
//                                                              //
// This source file may be used and distributed without         //
// restriction provided that this copyright statement is not    //
// removed from the file and that any derivative work contains  //
// the original copyright notice and the associated disclaimer. //
//                                                              //
// This source file is free software; you can redistribute it   //
// and/or modify it under the terms of the GNU Lesser General   //
// Public License as published by the Free Software Foundation; //
// either version 2.1 of the License, or (at your option) any   //
// later version.                                               //
//                                                              //
// This source is distributed in the hope that it will be       //
// useful, but WITHOUT ANY WARRANTY; without even the implied   //
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR      //
// PURPOSE.  See the GNU Lesser General Public License for more //
// details.                                                     //
//                                                              //
// You should have received a copy of the GNU Lesser General    //
// Public License along with this source; if not, download it   //
// from http://www.opencores.org/lgpl.shtml                     //
//                                                              //
----------------------------------------------------------------*/


#include "address_map.h"
#include "line-buffer.h"
#include "timer.h"
#include "utilities.h"
#include "packet.h"
#include "udp.h"


int         udp_checksum_errors_g = 0;
block_t*    udp_file_g = NULL;
block_t*    udp_current_block_g = NULL;


void parse_udp_packet(char * buf, packet_t* rx_packet)
{
    unsigned int udp_src_port    = buf[0]<<8|buf[1];
    unsigned int udp_dst_port    = buf[2]<<8|buf[3];
    unsigned int udp_len         = buf[4]<<8|buf[5];
    
    unsigned short prot_udp=17;
    unsigned short word16;
    unsigned long  sum = 0;     
    unsigned int   checksum;
    int mode_offset;
    int binary_mode;
    int i;
    
    
    for (i=0;i<4;i=i+2){
            word16 =((rx_packet->src_ip[i]<<8)&0xFF00)+(rx_packet->src_ip[i+1]&0xFF);
            sum=sum+word16; 
    }
    for (i=0;i<4;i=i+2){
            word16 =((rx_packet->dst_ip[i]<<8)&0xFF00)+(rx_packet->dst_ip[i+1]&0xFF);
            sum=sum+word16;         
    }
    // the protocol number and the length of the TCP packet
    sum = sum + prot_udp + udp_len;

    checksum = header_checksum16(buf, udp_len, sum);
       
    if (checksum)  
        udp_checksum_errors_g++;      
       
       
     /* TFTP */   
    if (udp_dst_port == 69 && !checksum) {
        unsigned int opcode = buf[8]<<8|buf[9];
        unsigned int block  = buf[10]<<8|buf[11];
        int tftp_len = udp_len - 12;
        
        mode_offset = next_string(&buf[10]);
        binary_mode = strcmp("octet", &buf[10+mode_offset]);
        
        switch (opcode) {
        
            case  UDP_READ:
                udp_reply(rx_packet, udp_dst_port, udp_src_port, 0, UDP_ERROR);
                break;
                
            case  UDP_WRITE:
                udp_file_g = init_buffer_512();
                udp_file_g->filename = malloc(256);
                strcpy(udp_file_g->filename, &buf[10]);
                
                if (strncmp(&buf[10], "vmlinux") == 0)
                    udp_file_g->linux_boot == 1;
                    
                udp_current_block_g = udp_file_g;
                
                
                if (binary_mode)
                    udp_reply(rx_packet, udp_dst_port, udp_src_port, 0, UDP_ACK);
                else
                    udp_reply(rx_packet, udp_dst_port, udp_src_port, 0, UDP_ERROR);
                break;


            case  UDP_DATA:
                udp_reply(rx_packet, udp_dst_port, udp_src_port, block, UDP_ACK);
                    
                if (block > udp_file_g->last_block) {
                    // Have not already received this block
                    udp_file_g->last_block = block;
                    
                    /* receive and save a block */
                    udp_current_block_g->bytes = tftp_len;
                    udp_file_g->total_bytes += tftp_len;
                    udp_file_g->total_blocks++;
                
                    memcpy(udp_current_block_g->buf512, &buf[12], tftp_len);
                
                    /* Prepare the next block */
                    if (tftp_len == 512) {
                        udp_current_block_g->next = init_buffer_512(); 
                        udp_current_block_g = udp_current_block_g->next;
                        }
                    else { /* Last block */
                        udp_file_g->ready = 1;
                        }
                    }
                break;
                
            
            default: break;
            }
        }
}



void udp_reply(packet_t* rx_packet, int udp_src_port, int udp_dst_port, int block, int reply_type)
{
    char* buf = (char*)ETHMAC_TX_BUFFER;
    unsigned short checksum;
    unsigned short prot_udp=17;
    unsigned short udp_len;
    unsigned short word16;
    unsigned long  sum = 0;     
    int i;
    mac_ip_t target;
    
    target.mac[0] = rx_packet->src_mac[0];
    target.mac[1] = rx_packet->src_mac[1];
    target.mac[2] = rx_packet->src_mac[2];
    target.mac[3] = rx_packet->src_mac[3];
    target.mac[4] = rx_packet->src_mac[4];
    target.mac[5] = rx_packet->src_mac[5];
    target.ip[0]  = rx_packet->src_ip[0];
    target.ip[1]  = rx_packet->src_ip[1];
    target.ip[2]  = rx_packet->src_ip[2];
    target.ip[3]  = rx_packet->src_ip[3];

    /* udp header */
    buf[34] = (udp_src_port & 0xff00)>>8;
    buf[35] =  udp_src_port & 0xff;
    buf[36] = (udp_dst_port & 0xff00)>>8;
    buf[37] =  udp_dst_port & 0xff;
    
    if (reply_type == UDP_ACK) 
        udp_len = 8+4;
    else /* error */
        udp_len = 8+2+2+14;


    buf[38] = (udp_len      & 0xff00)>>8;
    buf[39] =  udp_len      & 0xff;
    
    buf[40] = 0;  // checksum
    buf[41] = 0;  // checksum
    
    // -------------------------------------
    // tftf payload 
    // -------------------------------------
    // Opcode
    buf[42] = 0; 
    buf[43] = reply_type & 0xff;  // Acknowledgment    
    
    if (reply_type == UDP_ACK) {
        // block
        buf[44] = (block & 0xff00)>>8;
        buf[45] =  block & 0xff;
        }
    else {/* error */
        // Error Code
        buf[44] = 0;
        buf[45] = 0;
        // 46 to 59
        strncpy(&buf[46], "Not supported", 14);
        }
    
    
    // -------------------------------------
    // UDP Checksum calculation
    // -------------------------------------
    for (i=0;i<4;i=i+2){
            word16 =((rx_packet->src_ip[i]<<8)&0xFF00)+(rx_packet->src_ip[i+1]&0xFF);
            sum=sum+word16; 
    }
    for (i=0;i<4;i=i+2){
            word16 =((rx_packet->dst_ip[i]<<8)&0xFF00)+(rx_packet->dst_ip[i+1]&0xFF);
            sum=sum+word16;         
    }
    // the protocol number and the length of the TCP packet
    sum = sum + prot_udp + udp_len;

    checksum = header_checksum16(&buf[34], udp_len, sum);
    buf[40] = (checksum & 0xff00)>>8;  // checksum
    buf[41] =  checksum & 0xff;        // checksum
    
    ip_header(&buf[14], &target, 20+udp_len, 17); /* 20 byes of tcp  options, bytes 14 to 33, ip_proto = 17, UDP */
    ethernet_header(buf, &target, 0x0800);  /*bytes 0 to 13*/
    tx_packet(34+udp_len);  // packet length in bytes
}


block_t* init_buffer_512()
{
    block_t* block = malloc(sizeof(block_t));
    block->buf512 = malloc(512);
    block->next   = NULL;
    block->bytes  = 0;
    block->last_block  = 0;
    block->total_bytes  = 0;
    block->total_blocks  = 0;
    block->ready  = 0;
    block->filename  = NULL;
    return block;
}

