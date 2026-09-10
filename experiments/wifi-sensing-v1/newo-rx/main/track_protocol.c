#include "track_protocol.h"
#include "ncsi_protocol.h"
#include <string.h>
static void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static uint32_t get32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
size_t newo_track_control_encode(const newo_track_control_t*c,uint8_t*out,size_t cap){if(!c||!out||cap<24||!c->session_id||!c->sequence)return 0;memset(out,0,24);memcpy(out,"NTRK",4);out[4]=2;out[5]=c->active;out[6]=c->ack;out[7]=c->applied;put32(out+8,c->session_id);put32(out+12,c->sequence);put32(out+20,ncsi_crc32c(out,20));return 24;}
bool newo_track_control_decode(const uint8_t*in,size_t n,newo_track_control_t*c){if(!in||!c||n!=24||memcmp(in,"NTRK",4)||in[4]!=2||in[5]>1||in[6]>1||in[7]>1||get32(in+16)||get32(in+20)!=ncsi_crc32c(in,20))return false;c->active=in[5];c->ack=in[6];c->applied=in[7];c->session_id=get32(in+8);c->sequence=get32(in+12);return c->session_id&&c->sequence;}
