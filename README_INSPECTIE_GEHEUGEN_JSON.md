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

## Eerste Hypothese

De grootste winst zit niet in kleine settings JSON of NVS, maar in de initiele Tailscale MapResponse:

```text
h2_recv 512 KB + resp_buf 512 KB + cJSON DOM 2-3x raw JSON + tijdelijke buffers
```

Voor een kleine tailnet is dit acceptabel. Voor grote tailnets of ESP32 zonder ruime PSRAM is dit de plek waar optimalisatie het meeste oplevert.
