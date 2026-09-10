#include "newo_csi_measurement.h"
#include <esp_timer.h>
#include "newo_config.h"

namespace {
struct RingSlot { NewoCsiSlot value; };
RingSlot ring[NewoConfig::TRACK_RING_DEPTH];
volatile uint32_t writeIndex, readIndex, nextSequence;
volatile bool accepting;
uint8_t receiverMac[6], apMac[6], peerMac[6];
uint16_t peerRateHz;
int64_t peerLastAcceptedUs;
NewoCsiCounters counters;

uint8_t secondary(uint8_t value) { return value==WIFI_SECOND_CHAN_NONE?0:value==WIFI_SECOND_CHAN_ABOVE?1:value==WIFI_SECOND_CHAN_BELOW?2:UINT8_MAX; }
uint8_t phy(uint8_t value) { return value<=1?value:UINT8_MAX; }
uint8_t ltf(uint8_t mode,bool stbc){uint8_t v=1;if(mode==1)v|=2;if(mode==1&&stbc)v|=4;return v;}
uint16_t rxFlags(const wifi_pkt_rx_ctrl_t&r){uint16_t f=0;if(r.sig_mode==0)f|=NCSI_RX_FLAG_RATE_VALID;if(r.sig_mode==1)f|=NCSI_RX_FLAG_MCS_VALID;if(r.sgi)f|=NCSI_RX_FLAG_SHORT_GI;if(r.aggregation)f|=NCSI_RX_FLAG_AGGREGATED;if(r.fec_coding)f|=NCSI_RX_FLAG_LDPC;if(r.smoothing)f|=NCSI_RX_FLAG_SMOOTHING;if(r.not_sounding)f|=NCSI_RX_FLAG_NOT_SOUNDING;return f;}
}

bool newoCsiMeasurementBegin(const uint8_t receiver[6],const uint8_t ap[6],const uint8_t peer[6],uint16_t rate){
  if(!receiver||!ap||!peer||rate==0)return false;memcpy(receiverMac,receiver,6);memcpy(apMac,ap,6);memcpy(peerMac,peer,6);peerRateHz=rate;
  writeIndex=readIndex=nextSequence=0;peerLastAcceptedUs=0;memset(&counters,0,sizeof(counters));accepting=true;return true;
}
void newoCsiMeasurementStopAccepting(){accepting=false;}
bool newoCsiMeasurementEmpty(){return __atomic_load_n(&readIndex,__ATOMIC_ACQUIRE)==__atomic_load_n(&writeIndex,__ATOMIC_ACQUIRE);}
bool newoCsiMeasurementPop(NewoCsiSlot&out){uint32_t r=__atomic_load_n(&readIndex,__ATOMIC_RELAXED),w=__atomic_load_n(&writeIndex,__ATOMIC_ACQUIRE);if(r==w)return false;out=ring[r%NewoConfig::TRACK_RING_DEPTH].value;out.record.csi=out.csi;__atomic_store_n(&readIndex,r+1,__ATOMIC_RELEASE);return true;}
uint32_t newoCsiMeasurementDiscardPending(){uint32_t r=__atomic_load_n(&readIndex,__ATOMIC_RELAXED),w=__atomic_load_n(&writeIndex,__ATOMIC_ACQUIRE);uint32_t n=w-r;__atomic_store_n(&readIndex,w,__ATOMIC_RELEASE);__atomic_fetch_add(&counters.discarded,n,__ATOMIC_RELAXED);return n;}
NewoCsiCounters newoCsiMeasurementCounters(){return counters;}

void IRAM_ATTR newoCsiRxCallback(void*,wifi_csi_info_t*info){
  __atomic_fetch_add(&counters.callbacks,1,__ATOMIC_RELAXED);
  if(!accepting||!info||!info->buf||info->len==0||info->len>NCSI_MAX_CSI_BYTES||(info->len&1)){__atomic_fetch_add(&counters.invalid,1,__ATOMIC_RELAXED);return;}
  bool ap=memcmp(info->mac,apMac,6)==0,peer=memcmp(info->mac,peerMac,6)==0;if(!ap&&!peer){__atomic_fetch_add(&counters.unrelated,1,__ATOMIC_RELAXED);return;}
  uint16_t path=peer?3:1;int64_t now=esp_timer_get_time();
  if(peer&&peerLastAcceptedUs&&now-peerLastAcceptedUs<1000000LL/peerRateHz){__atomic_fetch_add(&counters.peerGateDrops,1,__ATOMIC_RELAXED);__atomic_fetch_add(&counters.pathGateDrops[2],1,__ATOMIC_RELAXED);return;}if(peer)peerLastAcceptedUs=now;
  uint32_t w=__atomic_load_n(&writeIndex,__ATOMIC_RELAXED),r=__atomic_load_n(&readIndex,__ATOMIC_ACQUIRE);if(w-r>=NewoConfig::TRACK_RING_DEPTH){__atomic_fetch_add(&counters.ringDrops,1,__ATOMIC_RELAXED);return;}
  NewoCsiSlot&s=ring[w%NewoConfig::TRACK_RING_DEPTH].value;memset(&s.record,0,sizeof(s.record));memcpy(s.record.receiver_mac,receiverMac,6);memcpy(s.record.source_mac,info->mac,6);memcpy(s.record.destination_mac,info->dmac,6);
  s.record.node_id=1;s.record.sequence=__atomic_fetch_add(&nextSequence,1,__ATOMIC_RELAXED);s.record.timestamp_us=now;s.record.channel=info->rx_ctrl.channel;s.record.secondary_channel=secondary(info->rx_ctrl.secondary_channel);s.record.bandwidth=info->rx_ctrl.cwb?1:0;s.record.phy_mode=phy(info->rx_ctrl.sig_mode);s.record.rssi_dbm=info->rx_ctrl.rssi;s.record.noise_floor_dbm=info->rx_ctrl.noise_floor;s.record.antenna=info->rx_ctrl.ant;s.record.ltf_mask=ltf(info->rx_ctrl.sig_mode,info->rx_ctrl.stbc);s.record.driver_csi_length=info->len;s.record.csi_length=info->len;s.record.csi_flags=NCSI_FLAG_RX_METADATA_VALID|NCSI_FLAG_SOURCE_FILTER_MATCHED;s.record.path_id=path;s.record.driver_rx_timestamp_us=info->rx_ctrl.timestamp;s.record.phy_rate=info->rx_ctrl.rate;s.record.mcs=info->rx_ctrl.mcs;s.record.rx_flags=rxFlags(info->rx_ctrl);s.record.ampdu_count=info->rx_ctrl.ampdu_cnt;s.record.rx_state=info->rx_ctrl.rx_state;s.record.packet_length=info->rx_ctrl.sig_len;s.record.driver_rx_sequence=info->rx_seq;if(info->rx_ctrl.stbc)s.record.csi_flags|=NCSI_FLAG_STBC;if(s.record.sequence==0)s.record.csi_flags|=NCSI_FLAG_SEQUENCE_RESET;
  memcpy(s.csi,info->buf,info->len);s.record.csi=s.csi;if(info->first_word_invalid){s.record.sanitized_prefix_bytes=ncsi_sanitize_invalid_prefix(s.csi,info->len,true);s.record.csi_flags|=NCSI_FLAG_FIRST_WORD_INVALID_REPORTED|NCSI_FLAG_INVALID_PREFIX_SANITIZED;}
  __atomic_fetch_add(&counters.accepted,1,__ATOMIC_RELAXED);__atomic_fetch_add(&counters.pathAccepted[path-1],1,__ATOMIC_RELAXED);__atomic_store_n(&writeIndex,w+1,__ATOMIC_RELEASE);uint32_t depth=w+1-r,old=counters.ringHighWater;while(depth>old&&!__atomic_compare_exchange_n(&counters.ringHighWater,&old,depth,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
