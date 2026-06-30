#include "ml_map_parser.h"
#include "microlink_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    ml_map_parse_stats_t *stats;
    microlink_t *ml;
    bool apply_node;
    bool apply_derp;
    bool apply_peers;
} parser_t;

static void skip_ws(parser_t *p);
static bool parse_literal(parser_t *p, const char *lit);
static bool parse_number(parser_t *p);

static bool parse_ipv4_prefix(const char *s, uint32_t *out_ip) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out_ip = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

static bool parse_ipv4_endpoint(const char *s, uint32_t *out_ip, uint16_t *out_port) {
    unsigned a, b, c, d, port;
    if (sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) != 5) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255 || port > 65535) return false;
    *out_ip = (a << 24) | (b << 16) | (c << 8) | d;
    *out_port = (uint16_t)port;
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_key(const char *s, const char *prefix, uint8_t out[32]) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(s, prefix, prefix_len) == 0) s += prefix_len;
    for (int i = 0; i < 32; i++) {
        int hi = hex_value(s[i * 2]);
        int lo = hex_value(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool queue_peer_update(parser_t *p, const ml_peer_update_t *src) {
    if (!p->apply_peers || !p->ml || !p->ml->peer_update_queue) return true;

    ml_peer_update_t *update = ml_psram_calloc(1, sizeof(*update));
    if (!update) return false;
    *update = *src;

    if (xQueueSend(p->ml->peer_update_queue, &update, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(update);
        p->stats->peer_updates_dropped++;
        return false;
    }

    switch (src->action) {
    case ML_PEER_ADD:
        p->stats->peer_updates_queued++;
        break;
    case ML_PEER_REMOVE:
        p->stats->removed_updates_queued++;
        break;
    case ML_PEER_UPDATE_ENDPOINT:
        p->stats->patch_updates_queued++;
        break;
    }
    return true;
}

static bool parse_key_expiry_epoch(const char *s, int64_t *out_epoch) {
    int yr, mo, dy, hr, mn, sc;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) < 6) return false;

    int64_t days = 0;
    for (int y = 1970; y < yr; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }
    static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 1; m < mo; m++) {
        days += mdays[m];
        if (m == 2 && (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0))) days++;
    }
    days += dy - 1;
    *out_epoch = days * 86400 + hr * 3600 + mn * 60 + sc;
    return true;
}

static bool parse_number_int(parser_t *p, int *out) {
    skip_ws(p);
    size_t start = p->pos;
    if (!parse_number(p)) return false;

    char tmp[24];
    size_t n = p->pos - start;
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, p->s + start, n);
    tmp[n] = '\0';
    *out = atoi(tmp);
    return true;
}

static bool parse_bool_value(parser_t *p, bool *out) {
    if (parse_literal(p, "true")) {
        *out = true;
        return true;
    }
    if (parse_literal(p, "false")) {
        *out = false;
        return true;
    }
    return false;
}

static void skip_ws(parser_t *p) {
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) p->pos++;
}

static bool consume(parser_t *p, char c) {
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != c) return false;
    p->pos++;
    return true;
}

static bool parse_string(parser_t *p, char *out, size_t out_size) {
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != '"') return false;
    p->pos++;

    size_t n = 0;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos++];
        if (c == '"') {
            if (out_size > 0) out[n < out_size ? n : out_size - 1] = '\0';
            return true;
        }
        if (c == '\\') {
            if (p->pos >= p->len) return false;
            c = (unsigned char)p->s[p->pos++];
            if (c == 'u') {
                if (p->pos + 4 > p->len) return false;
                p->pos += 4;
                c = '?';
            }
        }
        if (out && n + 1 < out_size) out[n] = (char)c;
        n++;
    }
    return false;
}

static bool parse_literal(parser_t *p, const char *lit) {
    skip_ws(p);
    size_t n = strlen(lit);
    if (p->pos + n > p->len || memcmp(p->s + p->pos, lit, n) != 0) return false;
    p->pos += n;
    return true;
}

static bool parse_number(parser_t *p) {
    skip_ws(p);
    size_t start = p->pos;
    if (p->pos < p->len && p->s[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    return p->pos > start;
}

static bool skip_value(parser_t *p);

static bool skip_array(parser_t *p) {
    if (!consume(p, '[')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (!skip_value(p)) return false;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool skip_object(parser_t *p) {
    char key[64];
    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key))) return false;
        if (!consume(p, ':')) return false;
        if (!skip_value(p)) return false;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool skip_value(parser_t *p) {
    skip_ws(p);
    if (p->pos >= p->len) return false;
    p->stats->skipped_values++;
    switch (p->s[p->pos]) {
    case '{': return skip_object(p);
    case '[': return skip_array(p);
    case '"': return parse_string(p, NULL, 0);
    case 't': return parse_literal(p, "true");
    case 'f': return parse_literal(p, "false");
    case 'n': return parse_literal(p, "null");
    default: return parse_number(p);
    }
}

static bool parse_string_array_first(parser_t *p, char *first, size_t first_size, bool *found) {
    char value[128];
    *found = false;
    if (first && first_size > 0) first[0] = '\0';
    if (!consume(p, '[')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
    while (p->pos < p->len) {
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '"') {
            if (!parse_string(p, value, sizeof(value))) return false;
            if (!*found) {
                *found = true;
                if (first && first_size > 0) {
                    strncpy(first, value, first_size - 1);
                    first[first_size - 1] = '\0';
                }
            }
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_endpoints(parser_t *p, uint32_t *ipv4_count, uint32_t *ipv6_skip,
                            ml_peer_update_t *update) {
    char ep[96];
    if (!consume(p, '[')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
    while (p->pos < p->len) {
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '"') {
            if (!parse_string(p, ep, sizeof(ep))) return false;
            if (strchr(ep, '[') || strchr(ep, ':') != strrchr(ep, ':')) (*ipv6_skip)++;
            else {
                (*ipv4_count)++;
                if (update && update->endpoint_count < ML_MAX_ENDPOINTS) {
                    uint32_t ip;
                    uint16_t port;
                    if (parse_ipv4_endpoint(ep, &ip, &port)) {
                        update->endpoints[update->endpoint_count].ip = ip;
                        update->endpoints[update->endpoint_count].port = port;
                        update->endpoints[update->endpoint_count].is_ipv6 = false;
                        update->endpoint_count++;
                    }
                }
            }
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_node(parser_t *p) {
    char key[64];
    bool saw_derp = false;
    if (!consume(p, '{')) return false;
    p->stats->saw_node = true;
    if (p->apply_node && p->ml) p->ml->key_expired = false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        if (p->apply_node && p->ml && p->ml->derp_home_region == 0) {
            p->ml->derp_home_region = ML_DERP_REGION;
        }
        return true;
    }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        if (strcmp(key, "Addresses") == 0) {
            bool found = false;
            char addr[64];
            if (!parse_string_array_first(p, addr, sizeof(addr), &found)) return false;
            if (found) p->stats->node_has_address = true;
            if (found && p->apply_node && p->ml && p->ml->vpn_ip == 0) {
                uint32_t ip;
                if (parse_ipv4_prefix(addr, &ip)) p->ml->vpn_ip = ip;
            }
        } else if (strcmp(key, "HomeDERP") == 0) {
            int region = 0;
            if (!parse_number_int(p, &region)) return false;
            p->stats->node_has_home_derp = true;
            if (region > 0) {
                saw_derp = true;
                if (p->apply_node && p->ml) p->ml->derp_home_region = (uint16_t)region;
            }
        } else if (strcmp(key, "DERP") == 0) {
            char derp[64];
            if (!parse_string(p, derp, sizeof(derp))) return false;
            p->stats->node_has_legacy_derp = true;
            const char *colon = strrchr(derp, ':');
            if (colon) {
                int region = atoi(colon + 1);
                if (region > 0 && !saw_derp) {
                    saw_derp = true;
                    if (p->apply_node && p->ml) p->ml->derp_home_region = (uint16_t)region;
                }
            }
        } else if (strcmp(key, "KeyExpiry") == 0) {
            char expiry[40];
            if (!parse_string(p, expiry, sizeof(expiry))) return false;
            p->stats->node_has_key_expiry = true;
            if (p->apply_node && p->ml) {
                int64_t epoch;
                if (parse_key_expiry_epoch(expiry, &epoch)) p->ml->key_expiry_epoch = epoch;
            }
        } else if (strcmp(key, "Expired") == 0) {
            bool expired = false;
            if (!parse_bool_value(p, &expired)) return false;
            p->stats->node_has_expired = true;
            if (p->apply_node && p->ml) p->ml->key_expired = expired;
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            if (p->apply_node && p->ml && p->ml->derp_home_region == 0) {
                p->ml->derp_home_region = ML_DERP_REGION;
            }
            return true;
        }
        return false;
    }
    return false;
}

static bool parse_peer_object(parser_t *p, bool patch, ml_peer_update_t *update) {
    char key[64];
    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        if (!patch && strcmp(key, "Name") == 0) {
            char name[64];
            if (!parse_string(p, name, sizeof(name))) return false;
            if (update) {
                strncpy(update->hostname, name, sizeof(update->hostname) - 1);
                size_t hlen = strlen(update->hostname);
                if (hlen > 0 && update->hostname[hlen - 1] == '.') update->hostname[hlen - 1] = '\0';
            }
        } else if (!patch && strcmp(key, "Key") == 0) {
            char value[96];
            if (!parse_string(p, value, sizeof(value))) return false;
            if (update) decode_key(value, "nodekey:", update->public_key);
        } else if (!patch && strcmp(key, "DiscoKey") == 0) {
            char value[96];
            if (!parse_string(p, value, sizeof(value))) return false;
            if (update) decode_key(value, "discokey:", update->disco_key);
        } else if (!patch && strcmp(key, "Addresses") == 0) {
            bool found = false;
            char addr[64];
            if (!parse_string_array_first(p, addr, sizeof(addr), &found)) return false;
            if (found && update) parse_ipv4_prefix(addr, &update->vpn_ip);
        } else if (!patch && strcmp(key, "HomeDERP") == 0) {
            int region = 0;
            if (!parse_number_int(p, &region)) return false;
            if (update && region > 0) update->derp_region = (uint16_t)region;
        } else if (patch && strcmp(key, "DERPRegion") == 0) {
            int region = 0;
            if (!parse_number_int(p, &region)) return false;
            if (update && region > 0) update->derp_region = (uint16_t)region;
        } else if (strcmp(key, "DERP") == 0) {
            char derp[64];
            if (!parse_string(p, derp, sizeof(derp))) return false;
            if (update && update->derp_region == 0) {
                unsigned region;
                if (sscanf(derp, "127.3.3.40:%u", &region) == 1 && region > 0) {
                    update->derp_region = (uint16_t)region;
                }
            }
        } else if (strcmp(key, "Endpoints") == 0) {
            if (patch) {
                if (!parse_endpoints(p, &p->stats->patch_endpoints,
                                     &p->stats->patch_ipv6_endpoints_skipped,
                                     update)) return false;
            } else {
                if (!parse_endpoints(p, &p->stats->peer_endpoints,
                                     &p->stats->peer_ipv6_endpoints_skipped,
                                     update)) return false;
            }
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_peer_array(parser_t *p, uint32_t *counter) {
    if (!consume(p, '[')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
    while (p->pos < p->len) {
        ml_peer_update_t update;
        memset(&update, 0, sizeof(update));
        update.action = ML_PEER_ADD;
        if (!parse_peer_object(p, false, &update)) return false;
        (*counter)++;
        if (!queue_peer_update(p, &update)) return false;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_removed_array(parser_t *p) {
    if (!consume(p, '[')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (p->pos < p->len && p->s[p->pos] == '"') {
            char value[96];
            if (!parse_string(p, value, sizeof(value))) return false;
            p->stats->peers_removed++;
            ml_peer_update_t update;
            memset(&update, 0, sizeof(update));
            update.action = ML_PEER_REMOVE;
            decode_key(value, "nodekey:", update.public_key);
            if (!queue_peer_update(p, &update)) return false;
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_patches(parser_t *p) {
    char key[96];
    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        ml_peer_update_t update;
        memset(&update, 0, sizeof(update));
        update.action = ML_PEER_UPDATE_ENDPOINT;
        decode_key(key, "nodekey:", update.public_key);
        if (!parse_peer_object(p, true, &update)) return false;
        p->stats->patches++;
        if (!queue_peer_update(p, &update)) return false;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_derp_node(parser_t *p, ml_derp_node_t *node) {
    char key[64];
    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        p->stats->derp_nodes++;
        return true;
    }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        if (strcmp(key, "HostName") == 0) {
            char value[64];
            if (!parse_string(p, value, sizeof(value))) return false;
            if (node) strncpy(node->hostname, value, sizeof(node->hostname) - 1);
        } else if (strcmp(key, "IPv4") == 0) {
            char value[16];
            if (!parse_string(p, value, sizeof(value))) return false;
            if (node) strncpy(node->ipv4, value, sizeof(node->ipv4) - 1);
        } else if (strcmp(key, "IPv6") == 0) {
            char value[46];
            if (!parse_string(p, value, sizeof(value))) return false;
            if (node) strncpy(node->ipv6, value, sizeof(node->ipv6) - 1);
        } else if (strcmp(key, "STUNPort") == 0) {
            int port = 0;
            if (!parse_number_int(p, &port)) return false;
            if (node && port > 0) node->stun_port = (uint16_t)port;
        } else if (strcmp(key, "DERPPort") == 0) {
            int port = 0;
            if (!parse_number_int(p, &port)) return false;
            if (node && port > 0) node->derp_port = (uint16_t)port;
        } else if (strcmp(key, "STUNOnly") == 0) {
            bool stun_only = false;
            if (!parse_bool_value(p, &stun_only)) return false;
            if (node) node->stun_only = stun_only;
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            p->stats->derp_nodes++;
            return true;
        }
        return false;
    }
    return false;
}

static bool parse_derp_nodes(parser_t *p, ml_derp_region_t *region) {
    if (!consume(p, '[')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
    while (p->pos < p->len) {
        ml_derp_node_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        ml_derp_node_t *target = NULL;
        if (region && region->node_count < ML_MAX_DERP_NODES) target = &tmp;
        if (!parse_derp_node(p, target)) return false;
        if (region && region->node_count < ML_MAX_DERP_NODES) {
            region->nodes[region->node_count++] = tmp;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        return false;
    }
    return false;
}

static void finish_derp_region(parser_t *p, const ml_derp_region_t *region) {
    p->stats->derp_regions++;
    if (p->apply_derp && p->ml && p->ml->derp_region_count < ML_MAX_DERP_REGIONS) {
        p->ml->derp_regions[p->ml->derp_region_count++] = *region;
    }
}

static bool parse_derp_region(parser_t *p) {
    char key[64];
    ml_derp_region_t region;
    memset(&region, 0, sizeof(region));

    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        finish_derp_region(p, &region);
        return true;
    }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        if (strcmp(key, "RegionID") == 0) {
            int id = 0;
            if (!parse_number_int(p, &id)) return false;
            if (id > 0) region.region_id = (uint16_t)id;
        } else if (strcmp(key, "RegionCode") == 0) {
            char value[8];
            if (!parse_string(p, value, sizeof(value))) return false;
            strncpy(region.code, value, sizeof(region.code) - 1);
        } else if (strcmp(key, "Avoid") == 0) {
            bool avoid = false;
            if (!parse_bool_value(p, &avoid)) return false;
            region.avoid = avoid;
        } else if (strcmp(key, "Nodes") == 0) {
            if (!parse_derp_nodes(p, p->apply_derp ? &region : NULL)) return false;
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            finish_derp_region(p, &region);
            return true;
        }
        return false;
    }
    return false;
}

static bool parse_derp_regions(parser_t *p) {
    char key[32];
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '[') {
        p->pos++;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
        while (p->pos < p->len) {
            if (!parse_derp_region(p)) return false;
            skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return true; }
            return false;
        }
        return false;
    }
    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        if (!parse_derp_region(p)) return false;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_derp_map(parser_t *p) {
    char key[64];
    if (!consume(p, '{')) return false;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
    while (p->pos < p->len) {
        if (!parse_string(p, key, sizeof(key)) || !consume(p, ':')) return false;
        if (strcmp(key, "Regions") == 0) {
            if (p->apply_derp && p->ml) p->ml->derp_region_count = 0;
            if (!parse_derp_regions(p)) return false;
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return true; }
        return false;
    }
    return false;
}

static bool parse_response_internal(microlink_t *ml, bool apply_node, bool apply_derp,
                                    bool apply_peers,
                                    const char *json, size_t len,
                                    ml_map_parse_stats_t *stats) {
    if (!json || !stats) return false;
    memset(stats, 0, sizeof(*stats));

    parser_t p = {
        .s = json,
        .len = len,
        .pos = 0,
        .stats = stats,
        .ml = ml,
        .apply_node = apply_node,
        .apply_derp = apply_derp,
        .apply_peers = apply_peers,
    };
    char key[64];

    if (!consume(&p, '{')) return false;
    skip_ws(&p);
    if (p.pos < p.len && p.s[p.pos] == '}') return true;

    while (p.pos < p.len) {
        if (!parse_string(&p, key, sizeof(key)) || !consume(&p, ':')) return false;
        stats->top_fields++;

        if (strcmp(key, "Node") == 0) {
            if (!parse_node(&p)) return false;
        } else if (strcmp(key, "DERPMap") == 0) {
            if (!parse_derp_map(&p)) return false;
        } else if (strcmp(key, "Peers") == 0) {
            if (!parse_peer_array(&p, &stats->peers)) return false;
        } else if (strcmp(key, "PeersChanged") == 0) {
            if (!parse_peer_array(&p, &stats->peers_changed)) return false;
        } else if (strcmp(key, "peers") == 0) {
            if (!parse_peer_array(&p, &stats->peers_lowercase)) return false;
        } else if (strcmp(key, "PeersRemoved") == 0) {
            if (!parse_removed_array(&p)) return false;
        } else if (strcmp(key, "PeersChangedPatch") == 0) {
            if (!parse_patches(&p)) return false;
        } else {
            stats->skipped_top_fields++;
            if (!skip_value(&p)) return false;
        }

        skip_ws(&p);
        if (p.pos < p.len && p.s[p.pos] == ',') { p.pos++; continue; }
        if (p.pos < p.len && p.s[p.pos] == '}') { p.pos++; skip_ws(&p); return p.pos == p.len; }
        return false;
    }
    return false;
}

bool ml_map_parse_response_verify(const char *json, size_t len,
                                  ml_map_parse_stats_t *stats) {
    return parse_response_internal(NULL, false, false, false, json, len, stats);
}

bool ml_map_parse_response_apply_basic(microlink_t *ml, const char *json, size_t len,
                                       ml_map_parse_stats_t *stats) {
    return parse_response_internal(ml, true, true, false, json, len, stats);
}

bool ml_map_parse_response_apply_peers(microlink_t *ml, const char *json, size_t len,
                                       ml_map_parse_stats_t *stats) {
    return parse_response_internal(ml, true, true, true, json, len, stats);
}
