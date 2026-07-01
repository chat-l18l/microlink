#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microlink.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Summary counters produced by the MapResponse streaming parser.
 *
 * These counters are intentionally cheap to collect and are used for debug
 * validation on real tailnets. They let us compare the streaming fast path with
 * the old cJSON path without keeping a full JSON DOM in memory.
 */
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

/* Parse a MapResponse and only report counters.
 *
 * Preconditions:
 * - `json` points to the first JSON byte, not the optional 4-byte Tailscale
 *   length prefix.
 * - `json[0..len)` remains valid for the duration of the call.
 * - `stats` is non-NULL.
 *
 * Postconditions:
 * - Runtime MicroLink state is not modified.
 * - `stats` is reset and filled with what the parser observed.
 * - Returns true only if the entire JSON object was consumed successfully.
 */
bool ml_map_parse_response_verify(const char *json, size_t len,
                                  ml_map_parse_stats_t *stats);

/* Parse and apply self-node plus DERPMap fields, but do not queue peers.
 *
 * This mode is useful when validating parser behavior without changing the
 * peer-update path. It updates only state owned by the coordination task.
 */
bool ml_map_parse_response_apply_basic(microlink_t *ml, const char *json, size_t len,
                                       ml_map_parse_stats_t *stats);

/* Parse the initial MapResponse fast path and apply all supported fields.
 *
 * Preconditions:
 * - `ml` is the active MicroLink instance owned by the coordination task.
 * - `ml->peer_update_queue` is valid if peer updates should be delivered.
 * - The input buffer is contiguous JSON and excludes the optional 4-byte
 *   Tailscale length prefix.
 *
 * Postconditions on success:
 * - `ml->vpn_ip`, key-expiry state and home DERP region are updated from
 *   `Node`.
 * - `ml->derp_regions` is refreshed from `DERPMap`.
 * - Peer add/remove/patch events are queued to `wg_mgr` without constructing a
 *   cJSON DOM.
 *
 * The goal is to keep memory usage proportional to the fields MicroLink uses,
 * not to the full Tailscale JSON document. Unknown fields are skipped by JSON
 * subtree depth, which makes large tailnets and future control-plane fields
 * cheaper to handle.
 */
bool ml_map_parse_response_apply_peers(microlink_t *ml, const char *json, size_t len,
                                       ml_map_parse_stats_t *stats);

#ifdef __cplusplus
}
#endif
