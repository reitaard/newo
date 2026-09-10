#include "track_protocol.h"
#include "ncsi_protocol.h"
#include <string.h>
static void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static uint32_t get32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
size_t newo_track_control_encode(const newo_track_control_t*c,uint8_t*out,size_t cap){if(!c||!out||cap<16)return 0;memset(out,0,16);memcpy(out,"NTRK",4);out[4]=1;out[5]=c->active;out[6]=c->ack;put32(out+8,c->sequence);put32(out+12,ncsi_crc32c(out,12));return 16;}
bool newo_track_control_decode(const uint8_t*in,size_t n,newo_track_control_t*c){if(!in||!c||n!=16||memcmp(in,"NTRK",4)||in[4]!=1||in[5]>1||in[6]>1||in[7]||get32(in+12)!=ncsi_crc32c(in,12))return false;c->active=in[5];c->ack=in[6];c->sequence=get32(in+8);return c->sequence!=0;}
