#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lwip/sockets.h"

typedef enum {
    NEWO_COLLECTOR_CONFIGURED = 0,
    NEWO_COLLECTOR_DISCOVERED = 1,
    NEWO_COLLECTOR_OVERRIDE = 2,
} newo_collector_source_t;

void newo_collector_discovery_start(const char *fallback_ipv4, uint16_t fallback_port);
void newo_collector_discovery_set_associated(bool associated);
bool newo_collector_set_override(const char *ipv4, uint16_t port);
void newo_collector_clear_override(void);
void newo_collector_snapshot(struct sockaddr_in *destination,
                             newo_collector_source_t *source);
const char *newo_collector_source_name(newo_collector_source_t source);
