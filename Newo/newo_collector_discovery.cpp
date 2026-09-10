#include "newo_collector_discovery.h"

#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

namespace {
TaskHandle_t taskHandle;
volatile bool running;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
uint32_t fallbackAddress, discoveredAddress, overrideAddress; uint16_t fallbackPort, discoveredPort, overridePort;
int64_t leaseDeadline; uint32_t candidateAddress, candidateNonce; uint16_t candidatePort; uint8_t candidateCount;
uint16_t le16(const uint8_t*p){return p[0]|p[1]<<8;} uint32_t le32(const uint8_t*p){return p[0]|p[1]<<8|p[2]<<16|p[3]<<24;}
void discovery(void*) {
  int s=socket(AF_INET,SOCK_DGRAM,IPPROTO_IP); if(s<0){running=false;vTaskDelete(nullptr);return;}
  int yes=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)); timeval tv={1,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  sockaddr_in bindTo={};bindTo.sin_family=AF_INET;bindTo.sin_port=htons(47777);bindTo.sin_addr.s_addr=INADDR_ANY;
  ip_mreq group={};group.imr_multiaddr.s_addr=inet_addr("239.255.77.77");group.imr_interface.s_addr=INADDR_ANY;
  if(bind(s,(sockaddr*)&bindTo,sizeof(bindTo))<0||setsockopt(s,IPPROTO_IP,IP_ADD_MEMBERSHIP,&group,sizeof(group))<0){close(s);running=false;vTaskDelete(nullptr);return;}
  while(running){uint8_t b[19];sockaddr_in from={};socklen_t n=sizeof(from);int got=recvfrom(s,b,sizeof(b),0,(sockaddr*)&from,&n);
    if(got==18&&!memcmp(b,"NCOL",4)&&b[4]==1&&b[5]==0&&le32(b+14)==0){uint16_t p=le16(b+6),lease=le16(b+8);uint32_t nonce=le32(b+10);
      if(p&&lease>=5&&lease<=300&&!IN_MULTICAST(ntohl(from.sin_addr.s_addr))&&from.sin_addr.s_addr!=INADDR_ANY){
        portENTER_CRITICAL(&mux);bool same=candidateAddress==from.sin_addr.s_addr&&candidatePort==p&&candidateNonce==nonce;
        candidateCount=same?candidateCount+1:1;candidateAddress=from.sin_addr.s_addr;candidatePort=p;candidateNonce=nonce;
        if(candidateCount>=2){discoveredAddress=candidateAddress;discoveredPort=candidatePort;leaseDeadline=esp_timer_get_time()+(int64_t)lease*1000000;candidateCount=0;}
        portEXIT_CRITICAL(&mux);
      }} }
  close(s);taskHandle=nullptr;vTaskDelete(nullptr);
}
}
bool newoCollectorDiscoveryStart(const char*fallback,uint16_t port){if(taskHandle)return true;fallbackAddress=inet_addr(fallback);fallbackPort=port;if(fallbackAddress==INADDR_NONE)return false;running=true;return xTaskCreatePinnedToCore(discovery,"ncol",3072,nullptr,2,&taskHandle,0)==pdPASS;}
void newoCollectorDiscoveryStop(){running=false;for(int i=0;taskHandle&&i<12;i++)vTaskDelay(pdMS_TO_TICKS(100));discoveredAddress=0;leaseDeadline=0;}
bool newoCollectorSetOverride(const char*address,uint16_t port){uint32_t parsed=address?inet_addr(address):INADDR_NONE;if(parsed==INADDR_NONE||!port)return false;portENTER_CRITICAL(&mux);overrideAddress=parsed;overridePort=port;portEXIT_CRITICAL(&mux);return true;}
void newoCollectorClearOverride(){portENTER_CRITICAL(&mux);overrideAddress=0;overridePort=0;portEXIT_CRITICAL(&mux);}
NewoCollectorDestination newoCollectorSnapshot(){uint32_t a;uint16_t p;const char*source;portENTER_CRITICAL(&mux);bool fresh=discoveredAddress&&esp_timer_get_time()<leaseDeadline;if(overrideAddress){a=overrideAddress;p=overridePort;source="override";}else if(fresh){a=discoveredAddress;p=discoveredPort;source="discovered";}else{a=fallbackAddress;p=fallbackPort;source="configured";}portEXIT_CRITICAL(&mux);return{a,p,source};}
