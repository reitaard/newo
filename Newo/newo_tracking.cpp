#include "newo_tracking.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "newo_config.h"
#include "newo_collector_discovery.h"

namespace {
constexpr size_t kMaxCsi = 612;
struct Slot { wifi_pkt_rx_ctrl_t rx; uint16_t length; uint8_t source[6]; uint8_t data[kMaxCsi]; };
Slot ring[NewoConfig::TRACK_RING_DEPTH];
volatile uint32_t writeIndex, readIndex, callbacks, accepted, rateGateDrops, ringDrops, transportDrops, sent, highWater, lastAcceptedTimestamp;
TaskHandle_t senderTask;
WiFiUDP udp;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t sequence;
uint8_t receiverMac[6];
uint8_t peerMac[6]; SemaphoreHandle_t peerAck; volatile uint32_t awaitingPeerSequence; volatile bool peerAckState;

void put16(uint8_t* p, uint16_t v) { p[0]=v; p[1]=v>>8; }
void put32(uint8_t* p, uint32_t v) { for (int i=0;i<4;i++) p[i]=v>>(8*i); }
void put64(uint8_t* p, uint64_t v) { put32(p,(uint32_t)v); put32(p+4,(uint32_t)(v>>32)); }
uint32_t crc32c(const uint8_t* p, size_t n) { uint32_t c=~0u; for(size_t i=0;i<n;i++){c^=p[i];for(int b=0;b<8;b++)c=(c>>1)^(0x82f63b78u & (uint32_t)-(int32_t)(c&1));} return ~c; }
bool parseMac(const char*t,uint8_t*m){unsigned v[6];char extra;if(!t||sscanf(t,"%2x:%2x:%2x:%2x:%2x:%2x%c",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&extra)!=6)return false;for(int i=0;i<6;i++)m[i]=v[i];return true;}
void onNowReceive(const esp_now_recv_info_t*info,const uint8_t*d,int n){if(!info||memcmp(info->src_addr,peerMac,6)||n!=16||memcmp(d,"NTRK",4)||d[4]!=1||d[6]!=1)return;uint32_t seq=(uint32_t)d[8]|(uint32_t)d[9]<<8|(uint32_t)d[10]<<16|(uint32_t)d[11]<<24;uint32_t crc=(uint32_t)d[12]|(uint32_t)d[13]<<8|(uint32_t)d[14]<<16|(uint32_t)d[15]<<24;if(seq!=awaitingPeerSequence||crc32c(d,12)!=crc)return;peerAckState=d[5]!=0;xSemaphoreGive(peerAck);}
bool sendPeerState(bool active){uint8_t b[16]={};memcpy(b,"NTRK",4);b[4]=1;b[5]=active;uint32_t seq=++awaitingPeerSequence;put32(b+8,seq);put32(b+12,crc32c(b,12));while(xSemaphoreTake(peerAck,0)==pdTRUE){}for(int attempt=0;attempt<3;attempt++){if(esp_now_send(peerMac,b,sizeof(b))==ESP_OK&&xSemaphoreTake(peerAck,pdMS_TO_TICKS(450))==pdTRUE&&peerAckState==active)return true;}return false;}
bool startPeer(){if(!parseMac(NewoConfig::TRACK_PEER_MAC,peerMac))return false;if(!peerAck)peerAck=xSemaphoreCreateBinary();if(!peerAck||esp_now_init()!=ESP_OK)return false;esp_now_peer_info_t peer={};memcpy(peer.peer_addr,peerMac,6);peer.ifidx=WIFI_IF_STA;peer.channel=0;if(esp_now_add_peer(&peer)!=ESP_OK||esp_now_register_recv_cb(onNowReceive)!=ESP_OK){esp_now_deinit();return false;}return true;}
void stopPeer(){esp_now_unregister_recv_cb();esp_now_del_peer(peerMac);esp_now_deinit();}

void IRAM_ATTR onCsi(void*, wifi_csi_info_t* info) {
  __atomic_fetch_add(&callbacks, 1, __ATOMIC_RELAXED);
  if (!info || !info->buf || info->len == 0 || info->len > kMaxCsi) return;
  uint32_t previous=__atomic_load_n(&lastAcceptedTimestamp,__ATOMIC_RELAXED);
  if(previous && (uint32_t)(info->rx_ctrl.timestamp-previous)<1000000u/NewoConfig::TRACK_RATE_HZ){__atomic_fetch_add(&rateGateDrops,1,__ATOMIC_RELAXED);return;}
  __atomic_store_n(&lastAcceptedTimestamp,info->rx_ctrl.timestamp,__ATOMIC_RELAXED);
  uint32_t w=__atomic_load_n(&writeIndex,__ATOMIC_RELAXED), r=__atomic_load_n(&readIndex,__ATOMIC_ACQUIRE);
  if (w-r >= NewoConfig::TRACK_RING_DEPTH) { __atomic_fetch_add(&ringDrops,1,__ATOMIC_RELAXED); return; }
  Slot& s=ring[w%NewoConfig::TRACK_RING_DEPTH]; s.rx=info->rx_ctrl; s.length=info->len;
  memcpy(s.source,info->mac,6); memcpy(s.data,info->buf,info->len);
  __atomic_store_n(&writeIndex,w+1,__ATOMIC_RELEASE);
  __atomic_fetch_add(&accepted,1,__ATOMIC_RELAXED);
  uint32_t depth=w+1-r, old=highWater; while(depth>old&&!__atomic_compare_exchange_n(&highWater,&old,depth,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}

void sender(void*) {
  uint8_t wire[88+kMaxCsi];
  while (true) {
    uint32_t r=__atomic_load_n(&readIndex,__ATOMIC_RELAXED), w=__atomic_load_n(&writeIndex,__ATOMIC_ACQUIRE);
    if(r==w){vTaskDelay(pdMS_TO_TICKS(5));continue;}
    Slot& s=ring[r%NewoConfig::TRACK_RING_DEPTH]; size_t n=88+s.length; memset(wire,0,88);
    memcpy(wire,"NCSI",4); wire[4]=1; wire[5]=1; put16(wire+6,88); put32(wire+8,n);
    put32(wire+16,1); memcpy(wire+20,receiverMac,6); memcpy(wire+26,s.source,6); put32(wire+32,sequence++);
    put64(wire+36,esp_timer_get_time()); wire[44]=s.rx.channel; wire[45]=s.rx.secondary_channel;
    wire[48]=(uint8_t)s.rx.rssi; wire[49]=(uint8_t)s.rx.noise_floor; wire[50]=s.rx.ant;
    put16(wire+52,s.length); put16(wire+54,s.length); put16(wire+56,s.length/2); put16(wire+60,1);
    put32(wire+64,s.rx.timestamp); wire[68]=s.rx.rate; wire[69]=s.rx.mcs; put16(wire+74,s.rx.sig_len);
    memset(wire+80,0,6); memcpy(wire+88,s.data,s.length); put32(wire+12,crc32c(wire,n));
    NewoCollectorDestination destination=newoCollectorSnapshot(); IPAddress ip(destination.address);
    bool ok=udp.beginPacket(ip,destination.port)==1 && udp.write(wire,n)==n && udp.endPacket()==1;
    __atomic_fetch_add(ok?&sent:&transportDrops,1,__ATOMIC_RELAXED); __atomic_store_n(&readIndex,r+1,__ATOMIC_RELEASE);
  }
}
}

void NewoTracking::begin() { state_=State::OFF; Serial.println("[track] state=TRACK_OFF resources=released"); }

bool NewoTracking::start() {
  if(state_==State::ACTIVE) return true;
  if(WiFi.status()!=WL_CONNECTED) return false;
  if(!startPeer()||!sendPeerState(true)){stopPeer();return false;}
  if(!newoCollectorDiscoveryStart(NewoConfig::TRACK_COLLECTOR_FALLBACK,NewoConfig::TRACK_COLLECTOR_PORT)){(void)sendPeerState(false);stopPeer();return false;}
  WiFi.macAddress(receiverMac); writeIndex=readIndex=0;
  wifi_csi_config_t cfg={}; cfg.lltf_en=true; cfg.htltf_en=true; cfg.stbc_htltf2_en=true; cfg.ltf_merge_en=true; cfg.channel_filter_en=true; cfg.manu_scale=false;
  if(esp_wifi_set_csi_config(&cfg)!=ESP_OK || esp_wifi_set_csi_rx_cb(onCsi,nullptr)!=ESP_OK || esp_wifi_set_csi(true)!=ESP_OK){newoCollectorDiscoveryStop();(void)sendPeerState(false);stopPeer();return false;}
  if(xTaskCreatePinnedToCore(sender,"newo_track",6144,nullptr,3,&senderTask,0)!=pdPASS){esp_wifi_set_csi(false);esp_wifi_set_csi_rx_cb(nullptr,nullptr);newoCollectorDiscoveryStop();(void)sendPeerState(false);stopPeer();return false;}
  state_=State::ACTIVE; Serial.println("[track] transition=TRACK_ACTIVE collector=configured peer=development-pending"); return true;
}

bool NewoTracking::stop() {
  if(state_==State::OFF) return true;
  if(!sendPeerState(false)) return false;
  esp_wifi_set_csi(false); esp_wifi_set_csi_rx_cb(nullptr,nullptr);
  if(senderTask){vTaskDelete(senderTask);senderTask=nullptr;} udp.stop(); newoCollectorDiscoveryStop(); stopPeer(); readIndex=writeIndex; state_=State::OFF;
  Serial.println("[track] transition=TRACK_OFF resources=released"); return true;
}

NewoTracking::Result NewoTracking::apply(const char* action,const char* epoch,uint32_t seq) {
  if(!action||!epoch||!*epoch||seq==0) return {false,false,"invalid_control"};
  if(strcmp(action,"on")&&strcmp(action,"off")&&strcmp(action,"toggle")&&strcmp(action,"status")) return {false,false,"invalid_action"};
  if(strcmp(epoch,commandEpoch_)==0 && seq<commandSequence_) return {false,false,"stale_control"};
  if(strcmp(epoch,commandEpoch_)==0 && seq==commandSequence_) return {lastApplied_,true,lastApplied_?nullptr:"previous_failure"};
  strlcpy(commandEpoch_,epoch,sizeof(commandEpoch_)); commandSequence_=seq;
  bool target=strcmp(action,"on")==0 ? true : strcmp(action,"off")==0 ? false : strcmp(action,"toggle")==0 ? state_==State::OFF : state_==State::ACTIVE;
  lastApplied_=strcmp(action,"status")==0 ? true : (target?start():stop()); return {lastApplied_,false,lastApplied_?nullptr:"transition_failed"};
}

NewoTracking::Metrics NewoTracking::metrics() const { auto destination=newoCollectorSnapshot();return {callbacks,accepted,rateGateDrops,ringDrops,transportDrops,sent,highWater,ESP.getFreeHeap(),ESP.getMinFreeHeap(),ESP.getFreePsram(),senderTask?uxTaskGetStackHighWaterMark(senderTask)*sizeof(StackType_t):0,destination.source,state_==State::ACTIVE?"coordinated":"released"}; }

void NewoTracking::loop() { static uint32_t last; if(state_==State::ACTIVE&&millis()-last>=30000){last=millis();auto m=metrics();Serial.printf("[track] state=TRACK_ACTIVE heap=%lu min_heap=%lu psram=%lu stack=%lu callbacks=%lu accepted=%lu gate_drop=%lu ring_hwm=%lu ring_drop=%lu transport_drop=%lu sent=%lu collector=%s espnow=%s\n",m.freeHeap,m.minFreeHeap,m.freePsram,m.taskStackBytes,m.callbacks,m.accepted,m.rateGateDrops,m.ringHighWater,m.ringDrops,m.transportDrops,m.sent,m.collectorSource,m.espNowState);} }
