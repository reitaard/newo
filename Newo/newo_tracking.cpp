#include "newo_tracking.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include "newo_collector_discovery.h"
#include "newo_config.h"
#include "newo_csi_measurement.h"

namespace {
TaskHandle_t senderTask; volatile bool senderStop,senderExited; WiFiUDP udp;
uint8_t peerMac[6]; SemaphoreHandle_t peerAck; uint32_t peerSession,peerSequence;
volatile uint32_t awaitingSequence; volatile bool peerAckState,peerAckApplied; bool ownsEspNow;
uint32_t transportSent,transportDrops;
bool parseMac(const char*t,uint8_t*m){unsigned v[6];char x;if(!t||sscanf(t,"%2x:%2x:%2x:%2x:%2x:%2x%c",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&x)!=6)return false;for(int i=0;i<6;i++){if(v[i]>255)return false;m[i]=v[i];}return true;}
void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
uint32_t get32(const uint8_t*p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void onPeer(const esp_now_recv_info_t*i,const uint8_t*d,int n){if(!i||memcmp(i->src_addr,peerMac,6)||n!=24||memcmp(d,"NTRK",4)||d[4]!=2||d[6]!=1||get32(d+8)!=peerSession||get32(d+12)!=awaitingSequence||get32(d+16)||get32(d+20)!=ncsi_crc32c(d,20))return;peerAckState=d[5];peerAckApplied=d[7];xSemaphoreGive(peerAck);}
bool beginPeer(){if(!parseMac(NewoConfig::TRACK_PEER_MAC,peerMac))return false;if(!peerAck)peerAck=xSemaphoreCreateBinary();if(!peerAck)return false;esp_err_t e=esp_now_init();if(e!=ESP_OK)return false;ownsEspNow=true;esp_now_peer_info_t p={};memcpy(p.peer_addr,peerMac,6);p.ifidx=WIFI_IF_STA;p.channel=0;if(esp_now_add_peer(&p)!=ESP_OK||esp_now_register_recv_cb(onPeer)!=ESP_OK){esp_now_deinit();ownsEspNow=false;return false;}return true;}
void endPeer(){if(!ownsEspNow)return;esp_now_unregister_recv_cb();esp_now_del_peer(peerMac);esp_now_deinit();ownsEspNow=false;}
bool requestPeer(bool active){uint8_t b[24]={};memcpy(b,"NTRK",4);b[4]=2;b[5]=active;put32(b+8,peerSession);uint32_t seq=++peerSequence;put32(b+12,seq);put32(b+20,ncsi_crc32c(b,20));awaitingSequence=seq;while(xSemaphoreTake(peerAck,0)==pdTRUE){}for(int a=0;a<3;a++)if(esp_now_send(peerMac,b,sizeof(b))==ESP_OK&&xSemaphoreTake(peerAck,pdMS_TO_TICKS(450))==pdTRUE&&peerAckApplied&&peerAckState==active)return true;return false;}
void sender(void*){uint8_t wire[NCSI_MAX_RECORD_SIZE];NewoCsiSlot slot;while(!senderStop||!newoCsiMeasurementEmpty()){if(!newoCsiMeasurementPop(slot)){vTaskDelay(pdMS_TO_TICKS(2));continue;}size_t n=ncsi_serialize_csi(&slot.record,wire,sizeof(wire));auto d=newoCollectorSnapshot();IPAddress ip(d.address);bool ok=n&&udp.beginPacket(ip,d.port)==1&&udp.write(wire,n)==n&&udp.endPacket()==1;__atomic_fetch_add(ok?&transportSent:&transportDrops,1,__ATOMIC_RELAXED);}senderExited=true;senderTask=nullptr;vTaskDelete(nullptr);}
}

void NewoTracking::begin(){peerSession=esp_random();if(!peerSession)peerSession=1;state_=State::OFF;peerStatus_="stopped";Serial.println("[track] state=TRACK_OFF resources=released");}
bool NewoTracking::start(){
  if(state_==State::ACTIVE)return true;if(WiFi.status()!=WL_CONNECTED){lastError_="wifi_unavailable";return false;}uint8_t receiver[6],ap[6];WiFi.macAddress(receiver);const uint8_t*bssid=WiFi.BSSID();if(!bssid||!parseMac(NewoConfig::TRACK_PEER_MAC,peerMac)){lastError_="identity_unavailable";return false;}memcpy(ap,bssid,6);
  if(!beginPeer()){lastError_="espnow_unavailable";return false;}if(!requestPeer(true)){peerStatus_="unavailable";lastError_="newo2_start_unconfirmed";endPeer();return false;}peerStatus_="active";
  if(!newoCollectorDiscoveryStart(NewoConfig::TRACK_COLLECTOR_FALLBACK,NewoConfig::TRACK_COLLECTOR_PORT)||!newoCsiMeasurementBegin(receiver,ap,peerMac,NewoConfig::TRACK_RATE_HZ)){newoCollectorDiscoveryStop();(void)requestPeer(false);endPeer();lastError_="resource_start_failed";return false;}
  wifi_csi_config_t cfg={};cfg.lltf_en=true;cfg.htltf_en=true;cfg.stbc_htltf2_en=true;cfg.ltf_merge_en=true;cfg.channel_filter_en=false;cfg.manu_scale=false;
  if(esp_wifi_set_csi_config(&cfg)!=ESP_OK||esp_wifi_set_csi_rx_cb(newoCsiRxCallback,nullptr)!=ESP_OK||esp_wifi_set_csi(true)!=ESP_OK){newoCsiMeasurementStopAccepting();newoCollectorDiscoveryStop();(void)requestPeer(false);endPeer();lastError_="csi_start_failed";return false;}
  senderStop=senderExited=false;if(xTaskCreatePinnedToCore(sender,"newo_track",6144,nullptr,3,&senderTask,0)!=pdPASS){esp_wifi_set_csi(false);esp_wifi_set_csi_rx_cb(nullptr,nullptr);newoCsiMeasurementStopAccepting();newoCollectorDiscoveryStop();(void)requestPeer(false);endPeer();lastError_="sender_start_failed";return false;}
  state_=State::ACTIVE;lastError_=nullptr;Serial.println("[track] transition=TRACK_ACTIVE paths=router_newo+newo2_newo");return true;
}
bool NewoTracking::stop(){
  if(state_==State::OFF)return true;bool peerStopped=requestPeer(false);peerStatus_=peerStopped?"stopped":"uncertain";
  newoCsiMeasurementStopAccepting();esp_wifi_set_csi(false);esp_wifi_set_csi_rx_cb(nullptr,nullptr);senderStop=true;uint32_t began=millis();while(!senderExited&&millis()-began<1000)vTaskDelay(pdMS_TO_TICKS(10));if(!senderExited){newoCsiMeasurementDiscardPending();if(senderTask){vTaskDelete(senderTask);senderTask=nullptr;}senderExited=true;}
  udp.stop();newoCollectorDiscoveryStop();endPeer();state_=State::OFF;lastError_=peerStopped?nullptr:"newo2_stop_unconfirmed";Serial.printf("[track] transition=TRACK_OFF peer=%s\n",peerStatus_);return true;
}
NewoTracking::Result NewoTracking::apply(const char*a,const char*epoch,uint32_t seq){if(!a||!epoch||!*epoch||!seq)return{false,false,"invalid_control",peerStatus_};if(strcmp(a,"on")&&strcmp(a,"off")&&strcmp(a,"toggle")&&strcmp(a,"status"))return{false,false,"invalid_action",peerStatus_};if(!strcmp(epoch,commandEpoch_)&&seq<commandSequence_)return{false,false,"stale_control",peerStatus_};if(!strcmp(epoch,commandEpoch_)&&seq==commandSequence_)return{lastApplied_,true,lastError_,peerStatus_};strlcpy(commandEpoch_,epoch,sizeof(commandEpoch_));commandSequence_=seq;bool target=!strcmp(a,"on")||(!strcmp(a,"toggle")&&state_==State::OFF);lastApplied_=!strcmp(a,"status")?true:(target?start():stop());return{lastApplied_,false,lastApplied_?lastError_:lastError_?lastError_:"transition_failed",peerStatus_};}
NewoTracking::Metrics NewoTracking::metrics()const{auto c=newoCsiMeasurementCounters();auto d=newoCollectorSnapshot();return{c.callbacks,c.accepted,c.peerGateDrops,c.ringDrops,transportDrops,transportSent,c.ringHighWater,ESP.getFreeHeap(),ESP.getMinFreeHeap(),ESP.getFreePsram(),senderTask?uxTaskGetStackHighWaterMark(senderTask)*sizeof(StackType_t):0,d.source,ownsEspNow?"owned":"released"};}
void NewoTracking::loop(){static uint32_t last;if(state_==State::ACTIVE&&millis()-last>=30000){last=millis();auto m=metrics();auto c=newoCsiMeasurementCounters();Serial.printf("[track] active ap=%lu peer=%lu unrelated=%lu peer_gate=%lu ring=%lu/%lu transport=%lu/%lu heap=%lu min=%lu psram=%lu stack=%lu collector=%s espnow=%s\n",c.pathAccepted[0],c.pathAccepted[2],c.unrelated,c.peerGateDrops,m.ringHighWater,m.ringDrops,m.sent,m.transportDrops,m.freeHeap,m.minFreeHeap,m.freePsram,m.taskStackBytes,m.collectorSource,m.espNowState);}}
