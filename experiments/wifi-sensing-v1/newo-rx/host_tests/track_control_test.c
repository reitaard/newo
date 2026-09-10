#include <assert.h>
#include <string.h>
#include "track_control_state.h"

int main(void){
 uint8_t wire[NEWO_TRACK_CONTROL_SIZE];newo_track_control_t c={.session_id=10,.sequence=1,.active=true};
 assert(newo_track_control_encode(&c,wire,sizeof(wire))==NEWO_TRACK_CONTROL_SIZE);newo_track_control_t decoded={};assert(newo_track_control_decode(wire,sizeof(wire),&decoded));
 newo_track_control_state_t s={};assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_APPLIED&&s.active);
 assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_DUPLICATE);
 decoded.sequence=0;assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_STALE);
 decoded.session_id=11;decoded.sequence=1;decoded.active=false;assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_APPLIED&&!s.active);
 decoded.session_id=10;assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_RETIRED_SESSION);
 memset(&s,0,sizeof(s));decoded.session_id=11;assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_APPLIED); /* Newo2 reboot */
 memset(&s,0,sizeof(s));decoded.session_id=12;decoded.sequence=1;assert(newo_track_control_apply(&s,&decoded)==NEWO_TRACK_APPLIED); /* both reboot */
 return 0;
}
