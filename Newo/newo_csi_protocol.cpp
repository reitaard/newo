// Compile the validated experiment serializer into the Arduino sketch without
// maintaining a divergent protocol implementation.
extern "C" {
#include "../experiments/wifi-sensing-v1/newo-rx/main/ncsi_protocol.c"
}
