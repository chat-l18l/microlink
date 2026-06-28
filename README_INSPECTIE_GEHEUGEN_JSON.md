# MicroLink v2 Inspectie: geheugen en JSON

Datum: 2026-06-27

Deze notitie is bedoeld als startpunt voor toekomstige verbeteringen aan MicroLink v2. Focus: projectstructuur, runtime geheugenallocatie, JSON-afhandeling en plekken waar waarschijnlijk veel winst te halen is op ESP32/ESP32-S3.

## Korte Samenvatting

MicroLink v2 is een ESP-IDF component/project voor een Tailscale-client op ESP32. De kern staat in `components/microlink/src`, met voorbeelden onder `examples/`.

De grootste geheugenverbruiker is de Tailscale control-plane flow in `ml_coord.c`. Bij een MapResponse worden standaard twee grote PSRAM-buffers tegelijk gebruikt:

| Buffer | Default | Locatie | Doel |
|---|---:|---|---|
| `ML_H2_BUFFER_SIZE` | 512 KB | PSRAM | Alle gedecrypte HTTP/2 frames verzamelen |
| `ML_JSON_BUFFER_SIZE` | 512 KB | PSRAM | DATA-frame payload samenvoegen tot JSON |
| cJSON DOM | variabel, vaak 2-3x JSON | PSRAM via hooks | Parse tree van dezelfde JSON |

Piekgebruik bij MapResponse is daardoor niet alleen 1 MB, maar eerder `H2 buffer + JSON buffer + cJSON DOM + tijdelijke Noise frame buffers`. Voor grote tailnets is dit logisch, maar hier zit ook de meeste optimalisatieruimte.

## Projectstructuur

Belangrijke bestanden:

| Bestand | Rol |
|---|---|
| `components/microlink/src/microlink.c` | Public API, init/start, task/queue setup, cJSON hooks naar PSRAM |
| `components/microlink/src/ml_coord.c` | Tailscale control plane: Noise, HTTP/2, RegisterRequest, MapRequest, MapResponse parsing |
| `components/microlink/src/ml_h2.c` | Handmatige HTTP/2 framebouw |
| `components/microlink/src/ml_derp.c` | DERP relay + TLS + TX queue |
| `components/microlink/src/ml_net_io.c` | UDP select-loop, packet classificatie, heap-copy naar queues |
| `components/microlink/src/ml_wg_mgr.c` | WireGuard, DISCO, peer management |
| `components/microlink/src/ml_peer_nvs.c` | NVS peer cache als compacte blob in PSRAM working copy |
| `components/microlink/src/ml_config_httpd.c` | Web UI/API, runtime config, JSON request/response, NVS settings |
| `components/microlink/Kconfig` | Buffergroottes, peer limits, config server, cellular/net-switch opties |

De component gebruikt ESP-IDF en heeft dependency op `json`/cJSON, `nvs_flash`, `esp_http_server`, `lwip`, `mbedtls` en `wireguard_lwip`.

## Geheugenmodel

### Task stacks

In `microlink_internal.h` staan vaste stackgroottes:

| Task | Stack |
|---|---:|
| `ml_net_io` | 8 KB |
| `ml_derp_tx` | 14 KB |
| `ml_coord` | 12 KB |
| `ml_wg_mgr` | 8 KB |

Samen is dit 42 KB SRAM, exclusief queues, lwIP, TLS en app-code.

### Grote structurele allocaties

| Allocatie | Waar | Grootte/impact |
|---|---|---|
| `microlink_t` | `microlink.c` via `ml_psram_calloc` | Bevat o.a. peer array, DERP map, keys, handles |
| Peer table | in `microlink_t` | `ML_MAX_PEERS` x `ml_peer_t`, default max 16 |
| DERP regions | in `microlink_t` | 32 regio's x 4 nodes met strings |
| Peer NVS cache | `ml_peer_nvs.c` via `ml_psram_calloc` | `4 + ML_NVS_MAX_PEERS * 92` bytes, default ca. 5.9 KB, max 94 KB bij 1024 peers |
| Config HTTP context | `ml_config_httpd.c` via `calloc` | Bevat settings, allowlist en WiFi lijst; in SRAM door normale `calloc` |

## JSON-afhandeling

### Globale cJSON allocator

In `microlink.c` wordt cJSON omgezet naar PSRAM:

```c
cJSON_Hooks hooks = {
    .malloc_fn = cjson_psram_malloc,
    .free_fn = free
};
cJSON_InitHooks(&hooks);
```

Dat is goed voor grote JSON, omdat cJSON DOM-nodes niet in interne SRAM landen. Nadeel: alle cJSON in het proces gebruikt daarna PSRAM, ook kleine webserver-responses.

### Control-plane JSON

Belangrijkste flow in `ml_coord.c`:

1. `do_register()` bouwt RegisterRequest met cJSON en print naar string via `cJSON_PrintUnformatted()`.
2. RegisterResponse wordt ontvangen in een 16 KB H2-buffer en 8 KB response-buffer.
3. `do_fetch_peers()` bouwt MapRequest met cJSON.
4. MapResponse wordt volledig verzameld in `h2_recv` (`ML_H2_BUFFER_SIZE`, default 512 KB).
5. DATA frames worden gekopieerd naar `resp_buf` (`ML_JSON_BUFFER_SIZE`, default 512 KB).
6. `resp_buf` wordt tijdelijk null-terminated en volledig geparsed met `cJSON_Parse()`.
7. `parse_peers_from_map_response()` loopt door `Peers`, `PeersChanged`, `PeersRemoved` en `PeersChangedPatch` en stuurt per peer een `ml_peer_update_t` naar `wg_mgr`.

Belangrijk: dezelfde MapResponse staat op piekmoment in meerdere vormen in geheugen:

| Vorm | Geheugen |
|---|---:|
| Gedecrypte HTTP/2 bytes | tot `ML_H2_BUFFER_SIZE` |
| JSON payload copy | tot `ML_JSON_BUFFER_SIZE` |
| cJSON DOM tree | vaak 2-3x JSON-grootte |
| Per-peer update queue items | tijdelijk, `sizeof(ml_peer_update_t)` per pending update |
| Noise frame buffer | tijdelijk 64 KB per read-loop iteratie |

Dit verklaart waarschijnlijk het hoge PSRAM-gebruik tijdens registratie en peer sync.

### Velden die uit server-JSON worden gelezen

Deze lijst is gebaseerd op `ml_coord.c`. Dit zijn de velden die nu met cJSON uit de Tailscale control-plane datastream worden gehaald.

#### EarlyNoise / handshake JSON

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `nodeKeyChallenge` | string / key payload | Challenge van de control-plane om bezit van de WireGuard private key te bewijzen | Wordt opgeslagen in `s_node_key_challenge`; later gebruikt voor `NodeKeyChallengeResponse` in RegisterRequest |

#### RegisterResponse

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `Node` | object | Self-node zoals door control-plane geregistreerd | Container voor eigen node-info |
| `Node.Addresses` | array string | Tailscale IP-adressen van dit apparaat, meestal `100.x.y.z/32` of `100.x.y.z` | Eerste IPv4 wordt `ml->vpn_ip` |
| `Node.HomeDERP` | number | DERP-regio die de server voor deze node kiest | Wordt `ml->derp_home_region` |
| `Node.DERP` | string | Legacy DERP-veld, vaak `127.3.3.40:<region>` | Fallback voor `HomeDERP` |
| `Node.Key` | string | Node public key volgens server, `nodekey:<hex>` | Alleen logging/verificatie tegen lokale WG pubkey |

#### MapResponse self-node

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `Node` | object | Self-node in MapResponse | Container voor eigen node-status |
| `Node.Addresses` | array string | Eigen Tailscale IP-adressen | Zet `ml->vpn_ip` als die nog niet gezet is |
| `Node.HomeDERP` | number | Actuele home DERP-regio | Zet `ml->derp_home_region` |
| `Node.DERP` | string | Legacy DERP-veld | Fallback voor `HomeDERP` |
| `Node.Key` | string | Eigen nodekey volgens server | Logging/verificatie |
| `Node.KeyExpiry` | string | ISO-8601 expiry timestamp van node key | Wordt omgezet naar `ml->key_expiry_epoch` |
| `Node.Expired` | bool | Of node key verlopen is | Zet `ml->key_expired` |

#### Peer-lijsten en peer updates

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `Peers` | array object | Volledige peer-lijst bij initiele MapResponse | Per peer een `ML_PEER_ADD` update naar `wg_mgr` |
| `PeersChanged` | array object | Delta met gewijzigde/nieuwe peers | Zelfde verwerking als `Peers` |
| `peers` | array object | Lowercase fallback | Zelfde verwerking als `Peers` |
| `PeersRemoved` | array string | Verwijderde peers als nodekey strings | Per key een `ML_PEER_REMOVE` update |
| `PeersChangedPatch` | object | Lightweight delta per nodekey | Per entry een `ML_PEER_UPDATE_ENDPOINT` update |

Per peer-object worden deze velden gelezen:

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `Name` | string | Hostname/FQDN van peer | Gekopieerd naar `update->hostname`; trailing dot wordt gestript |
| `Key` | string | Peer WireGuard/node public key, `nodekey:<hex>` | Gedecodeerd naar `update->public_key` |
| `DiscoKey` | string | Peer DISCO public key, `discokey:<hex>` | Gedecodeerd naar `update->disco_key` |
| `Addresses` | array string | Peer Tailscale IP-adressen | Eerste IPv4 wordt `update->vpn_ip` |
| `HomeDERP` | number | Peer home DERP-regio | Wordt `update->derp_region` |
| `DERP` | string | Legacy peer DERP-regio | Fallback voor `HomeDERP`, parse `127.3.3.40:<region>` |
| `Endpoints` | array string | Peer direct UDP endpoints, `IPv4:port` | Max `ML_MAX_ENDPOINTS` IPv4 endpoints naar `update->endpoints` |

Per `PeersChangedPatch` entry worden deze velden gelezen:

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| object key | string | Nodekey van de peer waarop de patch slaat | Gedecodeerd naar `update->public_key` |
| `DERPRegion` | number | Nieuwe DERP-regio in patchvorm | Wordt `update->derp_region` |
| `DERP` | string | Legacy DERP-regio in patchvorm | Fallback voor `DERPRegion` |
| `Endpoints` | array string | Nieuwe endpoint-lijst | Wordt `update->endpoints` |

#### DERPMap

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `DERPMap` | object | Volledige DERP-regio configuratie | Container voor DERP discovery |
| `DERPMap.Regions` | object/array van regio objecten | Regio's die DERP/STUN nodes bevatten | Max `ML_MAX_DERP_REGIONS` worden opgeslagen |
| `RegionID` | number | Numerieke DERP-regio ID | Wordt `ml_derp_region_t.region_id` |
| `RegionCode` | string | Korte regio-code, bv. `dfw` | Wordt `ml_derp_region_t.code` |
| `Avoid` | bool | Of regio vermeden moet worden | Wordt `ml_derp_region_t.avoid` |
| `Nodes` | array object | DERP/STUN nodes binnen regio | Max `ML_MAX_DERP_NODES` worden opgeslagen |
| `HostName` | string | Hostname van DERP node | Wordt `ml_derp_node_t.hostname` |
| `IPv4` | string | IPv4-adres van node | Wordt `ml_derp_node_t.ipv4` |
| `IPv6` | string | IPv6-adres van node | Wordt `ml_derp_node_t.ipv6` |
| `STUNPort` | number | STUN-poort, default 3478 als leeg | Wordt `ml_derp_node_t.stun_port` |
| `DERPPort` | number | DERP/TLS-poort, default 443 als leeg | Wordt `ml_derp_node_t.derp_port` |
| `STUNOnly` | bool | Node is alleen STUN, geen DERP relay | Wordt `ml_derp_node_t.stun_only` |

#### Long-poll MapResponse updates

Long-poll gebruikt dezelfde parser voor peer updates, maar leest daarnaast alleen:

| JSON-pad | Type | Betekenis | Gebruik in code |
|---|---|---|---|
| `Node.Addresses` | array string | Mogelijk bijgewerkt eigen VPN IP | Update `ml->vpn_ip` als het adres verandert |
| `PeersChanged` / `PeersRemoved` / `PeersChangedPatch` | array/object | Incremental peer updates | Zelfde verwerking als initiele MapResponse |

### Streaming Parser Veldplan

Als we cJSON voor MapResponse willen vervangen, hoeft de streaming parser niet elk Tailscale veld te begrijpen. Hij moet alleen events genereren voor de velden die MicroLink echt gebruikt.

Aanbevolen parser-events:

| Event | Wordt getriggerd door | Minimale output |
|---|---|---|
| `self_address` | `Node.Addresses[0]` | IPv4 `uint32_t` |
| `self_home_derp` | `Node.HomeDERP` of legacy `Node.DERP` | `uint16_t region` |
| `self_key_expiry` | `Node.KeyExpiry` | epoch seconds of originele string voor latere parse |
| `self_expired` | `Node.Expired` | bool |
| `peer_begin` | object in `Peers`, `PeersChanged` of lowercase `peers` | reset tijdelijke `ml_peer_update_t` |
| `peer_name` | `Name` | hostname string, max 63 chars |
| `peer_key` | `Key` | 32-byte public key |
| `peer_disco_key` | `DiscoKey` | 32-byte disco key |
| `peer_address` | eerste `Addresses[]` IPv4 | `uint32_t vpn_ip` |
| `peer_home_derp` | `HomeDERP` of legacy `DERP` | `uint16_t region` |
| `peer_endpoint` | elke IPv4 `Endpoints[]` | max `ML_MAX_ENDPOINTS` `{ip, port}` |
| `peer_end` | einde peer object | queue `ML_PEER_ADD` als key/IP geldig zijn |
| `peer_removed` | elk item in `PeersRemoved[]` | queue `ML_PEER_REMOVE` met public key |
| `patch_begin` | key/object in `PeersChangedPatch` | nodekey + reset patch update |
| `patch_derp` | `DERPRegion` of legacy `DERP` | region |
| `patch_endpoint` | patch `Endpoints[]` | endpoint list |
| `patch_end` | einde patch object | queue `ML_PEER_UPDATE_ENDPOINT` |
| `derp_region_begin` | item in `DERPMap.Regions` | reset regio struct |
| `derp_region_field` | `RegionID`, `RegionCode`, `Avoid` | regio metadata |
| `derp_node` | item in `Nodes[]` | hostname, IPv4, IPv6, STUNPort, DERPPort, STUNOnly |
| `derp_region_end` | einde regio object | opslaan in `ml->derp_regions` |

Minimale implementatievolgorde voor streaming parsing:

1. Start met initiele `MapResponse` zonder `DERPMap`: parse `Node`, `Peers`, `PeersChanged`, `PeersRemoved`, `PeersChangedPatch`.
2. Voeg daarna `DERPMap` toe, omdat die relatief groot kan zijn maar minder vaak verandert.
3. Houd `RegisterResponse` voorlopig op cJSON; die is klein en geen geheugenhotspot.
4. Houd request-JSON voorlopig op cJSON of vervang later door `snprintf`/kleine stringbuilder; request bodies zijn klein.
5. Zorg dat de parser objecten kan overslaan die niet in bovenstaande veldlijst staan, zonder allocaties.

Belangrijke randvoorwaarden:

1. JSON kan vooraf een 4-byte lengteprefix hebben; bestaande code zoekt/skipt die al.
2. HTTP/2 DATA kan over meerdere Noise frames verdeeld zijn; streaming JSON parsing moet dus bytes incrementeel kunnen voeren.
3. `PeersChangedPatch` is een object waarvan de property-name zelf de nodekey is; de parser moet dus object keys als data kunnen gebruiken.
4. `DERPMap.Regions` kan als object met regio-ID keys binnenkomen; de parser moet niet alleen arrays ondersteunen maar ook object children kunnen itereren.
5. Onbekende velden moeten goedkoop geskipt worden, want Tailscale voegt regelmatig velden toe.

### HTTP Config Server JSON

`ml_config_httpd.c` gebruikt cJSON voor REST endpoints:

| Endpoint/functie | Gedrag |
|---|---|
| `send_json()` | `cJSON_PrintUnformatted()`, response string tijdelijk in PSRAM |
| `read_post_body()` | POST body max 4096 bytes via normale `malloc` |
| `/api/settings` | Parse settings JSON, opslaan als NVS blob |
| `/api/peers/allowed` | Parse allowlist array, opslaan als compacte NVS blob |
| `/api/wifi` | Parse WiFi-lijst, max 16 entries, opslaan als compacte NVS blob |
| `/api/status`, `/api/monitor`, `/api/peers` | Bouwt responses met cJSON DOM |

Hier is het JSON-volume klein. De grootste winst zit niet in de webserver, behalve als de allowlist richting honderden peers gaat.

## NVS en JSON Files

Er lijken geen JSON-bestanden op SPIFFS/LittleFS/SD te worden gelezen of geschreven. JSON wordt gebruikt als netwerkprotocolpayload en HTTP API body/response. Persistente data gaat als binary NVS blobs:

| Namespace/key | Bestand | Data |
|---|---|---|
| `microlink` | `microlink.c` | machine/WG/DISCO keys |
| `ml_peers/tbl` | `ml_peer_nvs.c` | compacte peer cache blob |
| `ml_config/settings` | `ml_config_httpd.c` | packed `ml_config_settings_t` |
| `ml_config/peers` | `ml_config_httpd.c` | count + gebruikte allowlist entries |
| `ml_config/wifi_list` | `ml_config_httpd.c` | count + gebruikte WiFi entries |

De opmerking "JSON files" lijkt dus vooral te slaan op JSON payloads van de Tailscale control-plane, niet op filesystem JSON-bestanden.

## Hotspots Voor Optimalisatie

### 1. MapResponse dubbel bufferen vermijden

Huidige situatie: eerst alle HTTP/2 bytes verzamelen in `h2_recv`, daarna alle DATA payload kopiëren naar `resp_buf`, daarna volledig parsen.

Mogelijke verbetering: HTTP/2 DATA frames direct naar één JSON-buffer schrijven tijdens ontvangst. Dan kan `h2_recv` weg of veel kleiner worden, mits frame-fragmentatie correct wordt afgehandeld.

Impact: bespaart standaard tot 512 KB PSRAM tijdens MapResponse.

Risico: HTTP/2 frames kunnen over Noise frames heen splitsen. Er is een kleine frame-parser/state-machine nodig in plaats van volledige accumulatie.

### 2. cJSON DOM vervangen of beperken voor MapResponse

cJSON bouwt een volledige DOM van de hele MapResponse. Voor grote tailnets is dat de duurste stap.

Opties:

| Optie | Winst | Complexiteit |
|---|---|---|
| Alleen relevante velden streamend scannen | hoog | hoog |
| `jsmn`-achtige token parser zonder allocaties | hoog | medium/hoog |
| cJSON blijven gebruiken, maar kleinere responses afdwingen via filters/delta | medium | laag/medium |
| Peers in batches verwerken en DOM snel vrijgeven | beperkt | laag |

Meest pragmatische eerste stap: dubbel bufferen weghalen voordat cJSON wordt vervangen.

### 3. 64 KB Noise frame buffer herhaald alloceren

In `do_fetch_peers()` wordt per read-loop iteratie `frame_buf = ml_psram_malloc(65536)` gedaan en daarna direct vrijgegeven.

Verbetering: één 64 KB frame buffer buiten de loop alloceren en hergebruiken.

Impact: minder allocator churn en fragmentatie; weinig piekbesparing als de buffer toch nodig blijft.

### 4. RegisterResponse tijdelijke allocaties

`do_register()` gebruikt 16 KB H2-buffer, 8 KB response-buffer en per frame een 4 KB buffer. Dit is veel kleiner dan MapResponse, maar dezelfde patroonverbetering kan worden toegepast.

### 5. Per-packet malloc in UDP/DERP datapad

`ml_net_io.c` kopieert elk UDP pakket van stackbuffer naar heap voordat het naar queues gaat. `ml_derp.c` doet vergelijkbare allocaties voor payloads en TX queue items.

Voor throughput-toepassingen kan dit fragmentatie en latency geven. Er is al een optionele zero-copy WG mode (`CONFIG_ML_ZERO_COPY_WG`) die een deel hiervan oplost.

Mogelijke verbetering: fixed-size packet pool/slab voor `ml_rx_packet_t` data, vooral voor 1500-byte WG packets.

### 6. Config HTTP context in SRAM

`ml_config_httpd_init()` gebruikt `calloc(1, sizeof(ml_config_ctx_t))`, dus normale heap/interne SRAM afhankelijk van allocatorconfig. De context bevat o.a. allowlist-array en WiFi-list.

Bij grote `CONFIG_ML_CONFIG_MAX_ALLOWED_PEERS` kan dit flink worden. Overweeg `heap_caps_calloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` voor de context of specifiek de grote arrays.

### 7. Log-volume en strings

Er is veel `ESP_LOGI` in hot paths, o.a. UDP RX, WireGuard periodic, DISCO periodic en MapResponse parsing. Dit is geen grote heap-allocatie, maar kan timing, stack en UART-bandbreedte beïnvloeden.

## Concrete Aanpak Voor Later

Aanbevolen volgorde:

1. Meet eerst piekgebruik rond `do_fetch_peers()` met `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` en `esp_get_free_heap_size()` voor/na elke grote allocatie.
2. Hergebruik de 64 KB Noise frame buffer in `do_fetch_peers()`.
3. Verwijder de dubbele MapResponse-buffer door DATA frames direct naar `resp_buf` te streamen.
4. Maak daarna een keuze: cJSON houden of MapResponse peers streamend/token-based parsen.
5. Als datapad belangrijk wordt: packet pool voor UDP/DERP in plaats van malloc/free per packet.
6. Als config server met grote allowlist wordt gebruikt: verplaats `ml_config_ctx_t` of de allowlist naar PSRAM.

## Werknotitie 2026-06-28

Stap 1 en 2 zijn uitgevoerd in `components/microlink/src/ml_coord.c`:

1. Tijdelijke `[MAP_HEAP]` meetlogs toegevoegd rond `do_fetch_peers()` voor interne heap en PSRAM, inclusief minimum-watermarks.
2. De 64 KB Noise `frame_buf` in `do_fetch_peers()` wordt nu eenmalig buiten de receive-loop gealloceerd en hergebruikt.

Deze instrumentatie is bewust expliciet gelogd zodat de MapResponse-piek op echte ESP32-hardware gemeten kan worden. Als de metingen klaar zijn, deze logs verwijderen of achter een debug/Kconfig-vlag zetten, bijvoorbeeld `CONFIG_ML_COORD_HEAP_TRACE` of een runtime debug flag.

Belangrijke meetpunten in de logs:

| Stage | Wat het laat zien |
|---|---|
| `before MapResponse buffers` | Baseline voor de grote allocaties |
| `after h2_recv alloc` | Kosten van `ML_H2_BUFFER_SIZE` |
| `after resp_buf alloc` | Kosten van `ML_JSON_BUFFER_SIZE` |
| `after frame_buf alloc` | Vaste 64 KB Noise frame buffer |
| `before cJSON parse` / `after cJSON parse` | Extra cJSON DOM-gebruik |
| `after cJSON delete` / `after resp_buf free` | Hoeveel geheugen echt terugkomt |

Volgende stap: de meetresultaten gebruiken om te bepalen of eerst dubbele buffering (`h2_recv` -> `resp_buf`) wordt verwijderd, of dat MapResponse parsing direct cJSON-vrij/token-based gemaakt moet worden.

## Werknotitie 2026-06-28: DISCO/Probe Verkeer Verminderen

Na succesvolle ESP32-P4 Ethernet test viel op dat er veel netwerkverkeer en logging is rond DISCO, CallMeMaybe, STUN en WireGuard periodic processing. Voor embedded use-cases is dit waarschijnlijk te agressief, vooral als het device maar met een paar vaste peers hoeft te praten.

Voorbeelden uit runtime logs:

- `DISCO PING` / `DISCO PONG` via direct UDP en DERP.
- `CallMeMaybe` heen en terug na endpoint discovery.
- Periodieke `CMM probe` naar peer endpoints.
- `STUN probe` / periodic re-probe.
- HTTP/2 control-plane `PING` elke 5 seconden.
- Veel `ESP_LOGI` in hot paths (`UDP RX`, `DISCO RX`, `DERP TX`, `wireguardif_periodic`).

Bestaand instelbaar:

| Setting | Waar | Default | Opmerking |
|---|---|---:|---|
| `disco_heartbeat_ms` | config struct / web UI / NVS | 3000 ms | Wordt gebruikt voor periodieke DISCO heartbeat in `ml_wg_mgr.c` |

Hardcoded timing die later configureerbaar gemaakt kan worden:

| Constant | Huidig | Betekenis |
|---|---:|---|
| `ML_DISCO_PING_INTERVAL_MS` | 5000 ms | Minimum interval tussen DISCO pings naar peer |
| `ML_DISCO_HEARTBEAT_MS` | 3000 ms | Default heartbeat als config leeg is |
| `ML_DISCO_TRUST_DURATION_MS` | 15000 ms | Hoe lang direct path vertrouwd blijft na PONG |
| `ML_DISCO_UPGRADE_INTERVAL_MS` | 15000 ms | Interval voor direct-path upgrade probes |
| `ML_DISCO_SESSION_ACTIVE_MS` | 45000 ms | Recente sessie-activiteit window |
| `ML_STUN_RESTUN_INTERVAL_MS` | 23000 ms | Periodieke STUN re-probe |

Aanpak voor later:

1. Zet voor testen eerst `disco_heartbeat_ms` via web UI/NVS naar `30000` of `60000` ms.
2. Verlaag hot-path logging van `ESP_LOGI` naar `ESP_LOGD`, of guard met bestaande `debug_flags`.
3. Maak DISCO/STUN timing Kconfig/runtime configureerbaar in plaats van hardcoded defines.
4. Voeg eventueel `CONFIG_ML_LOW_TRAFFIC_MODE` toe met embedded-vriendelijke defaults:
   - DISCO heartbeat: 30000 ms
   - STUN re-probe: 120000 ms
   - DISCO upgrade probe: 60000 ms
   - hot-path UDP/DISCO/DERP logs uit tenzij debug flag aan staat
5. Probe niet alle peers periodiek. Beperk tot priority peer, allowlist, peers met recente activiteit, of peers waarvoor een direct path nuttig is.
6. Houd control-plane H2 PING voorzichtig: server idle timeout lijkt rond 20s te liggen, dus die 5s ping pas wijzigen na meting.

Belangrijke nuance: het hoge logvolume betekent niet een-op-een hetzelfde als netwerkvolume. Een deel van de waargenomen drukte komt doordat normale protocol-events op `INFO` worden gelogd.

## Eerste Hypothese

De grootste winst zit niet in kleine settings JSON of NVS, maar in de initiele Tailscale MapResponse:

```text
h2_recv 512 KB + resp_buf 512 KB + cJSON DOM 2-3x raw JSON + tijdelijke buffers
```

Voor een kleine tailnet is dit acceptabel. Voor grote tailnets of ESP32 zonder ruime PSRAM is dit de plek waar optimalisatie het meeste oplevert.
