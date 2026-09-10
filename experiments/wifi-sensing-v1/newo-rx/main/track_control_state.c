#include "track_control_state.h"
newo_track_apply_result_t newo_track_control_apply(newo_track_control_state_t*s,const newo_track_control_t*c){
 if(!s||!c||c->ack||!c->session_id||!c->sequence)return NEWO_TRACK_STALE;
 if(c->session_id==s->retired_session_id)return NEWO_TRACK_RETIRED_SESSION;
 if(c->session_id!=s->session_id){s->retired_session_id=s->session_id;s->session_id=c->session_id;s->sequence=0;}
 if(c->sequence<s->sequence)return NEWO_TRACK_STALE;
 if(c->sequence==s->sequence)return NEWO_TRACK_DUPLICATE;
 s->sequence=c->sequence;s->active=c->active;return NEWO_TRACK_APPLIED;
}
