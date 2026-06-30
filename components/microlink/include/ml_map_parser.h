#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microlink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t top_fields;
    uint32_t skipped_top_fields;

    bool saw_node;
    bool node_has_address;
    bool node_has_home_derp;
    bool node_has_legacy_derp;
    bool node_has_key_expiry;
    bool node_has_expired;

    uint32_t peers;
    uint32_t peers_changed;
    uint32_t peers_lowercase;
    uint32_t peers_removed;
    uint32_t peer_endpoints;
    uint32_t peer_ipv6_endpoints_skipped;
    uint32_t peer_updates_queued;
    uint32_t peer_updates_dropped;

    uint32_t patches;
    uint32_t patch_endpoints;
    uint32_t patch_ipv6_endpoints_skipped;
    uint32_t patch_updates_queued;
    uint32_t removed_updates_queued;

    uint32_t derp_regions;
    uint32_t derp_nodes;

    uint32_t skipped_values;
} ml_map_parse_stats_t;

bool ml_map_parse_response_verify(const char *json, size_t len,
                                  ml_map_parse_stats_t *stats);
bool ml_map_parse_response_apply_basic(microlink_t *ml, const char *json, size_t len,
                                       ml_map_parse_stats_t *stats);
bool ml_map_parse_response_apply_peers(microlink_t *ml, const char *json, size_t len,
                                       ml_map_parse_stats_t *stats);

#ifdef __cplusplus
}
#endif
