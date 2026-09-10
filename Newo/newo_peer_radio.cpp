#include "newo_peer_radio.h"
#include <cstring>

namespace {
constexpr size_t kConsumerCount = 4;
struct Consumer { uint32_t magic; esp_now_recv_cb_t callback; };
bool owned; uint8_t peer[6]; Consumer consumers[kConsumerCount];
NewoPeerIdentity identity{}; NewoPeerRadioMetrics metrics{};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t readMagic(const uint8_t* d) { return uint32_t(d[0])|uint32_t(d[1])<<8|uint32_t(d[2])<<16|uint32_t(d[3])<<24; }
void receive(const esp_now_recv_info_t* info,const uint8_t* data,int length) {
  __atomic_fetch_add(&metrics.received,1,__ATOMIC_RELAXED);
  if(!info||!data||length<4||memcmp(info->src_addr,peer,6)){__atomic_fetch_add(&metrics.dropped,1,__ATOMIC_RELAXED);return;}
  esp_now_recv_cb_t callback=nullptr;uint32_t magic=readMagic(data);
  portENTER_CRITICAL(&lock);for(const auto& c:consumers)if(c.magic==magic){callback=c.callback;break;}portEXIT_CRITICAL(&lock);
  if(!callback){__atomic_fetch_add(&metrics.unmatched,1,__ATOMIC_RELAXED);return;}
  __atomic_fetch_add(&metrics.routed,1,__ATOMIC_RELAXED);callback(info,data,length);
}
}
bool newoPeerRadioAcquire(const uint8_t peerMac[6]) {
  if(owned||!peerMac||esp_now_init()!=ESP_OK)return false;owned=true;memset(&metrics,0,sizeof(metrics));
  esp_now_peer_info_t info={};memcpy(info.peer_addr,peerMac,sizeof(peer));info.ifidx=WIFI_IF_STA;info.channel=0;
  if(esp_now_add_peer(&info)!=ESP_OK||esp_now_register_recv_cb(receive)!=ESP_OK){newoPeerRadioRelease();return false;}
  memcpy(peer,peerMac,sizeof(peer));memcpy(identity.mac,peerMac,sizeof(peer));return true;
}
bool newoPeerRadioRegisterConsumer(uint32_t magic,esp_now_recv_cb_t callback){if(!owned||!magic||!callback)return false;bool result=false;portENTER_CRITICAL(&lock);for(auto& c:consumers)if(c.magic==magic||!c.callback){c.magic=magic;c.callback=callback;result=true;break;}portEXIT_CRITICAL(&lock);return result;}
void newoPeerRadioUnregisterConsumer(uint32_t magic){portENTER_CRITICAL(&lock);for(auto& c:consumers)if(c.magic==magic)c={};portEXIT_CRITICAL(&lock);}
void newoPeerRadioRelease(){if(!owned)return;esp_now_unregister_recv_cb();esp_now_del_peer(peer);esp_now_deinit();portENTER_CRITICAL(&lock);memset(consumers,0,sizeof(consumers));portEXIT_CRITICAL(&lock);memset(peer,0,sizeof(peer));memset(&identity,0,sizeof(identity));owned=false;}
esp_err_t newoPeerRadioSend(const uint8_t*p,size_t n){esp_err_t r=owned?esp_now_send(peer,p,n):ESP_ERR_INVALID_STATE;if(r!=ESP_OK)__atomic_fetch_add(&metrics.sendFailures,1,__ATOMIC_RELAXED);return r;}
bool newoPeerRadioOwned(){return owned;} NewoPeerRadioMetrics newoPeerRadioMetrics(){return metrics;} NewoPeerIdentity newoPeerRadioIdentity(){return identity;} void newoPeerRadioSetIdentity(const NewoPeerIdentity&v){identity=v;}
