/* Flat-text line formatting for the `mesh_log` capability's on-SD-card accumulator
   (docs/WARDRIVING_PUBLISH.md "Flipper-side storage", docs/CAPABILITIES.md's mesh_log
   bullet). Mirrors wardriving_csv.h/.c's module shape (pure formatting here, Furi-dependent
   file I/O in flipper_esp32_over_ble.c) but much simpler -- no CSV quoting (node_id is
   bounded hex/text, network is one of two fixed literal values, neither can contain the '|'
   delimiter), no dedup table (the ESP32/Heltec side already dedups "once ever" before a
   record ever reaches its flash log, so the Flipper appends every record it receives
   unconditionally), no header row.

   Flipper-only, not a shared cross-firmware wire-format header (not in
   tools/check_shared_headers.py's HEADER_PAIRS -- there is no Heltec-side equivalent; the
   Heltec only ever sends the CBOR <mesh-log-record> shape, never this flat-line format).
   Deliberately has no Furi dependency (stdint/stddef/cbor_codec.h only, matching
   wardriving_csv.h's own convention) so it builds and is host-testable exactly like the
   cbor_* codec modules.

   One line per record: "node_id|network|lat|lon\n" -- pipe-delimited, decimal lat/lon (not
   the raw e7_offset integers on the wire; decoded first using the same
   `(int32_t)(lat_or_lon * 1e7) + <hemisphere-max-magnitude-offset>` inverse as every other
   lat/lon pair in this protocol, see wardriving_csv.c's own arithmetic for the exact
   double-literal style this must match to avoid -Werror=double-promotion). */
#ifndef FEB_MESH_NODES_H
#define FEB_MESH_NODES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cbor_codec.h"

/* Worst case: node_id(16) + '|'(1) + network(10) + '|'(1) + lat("-90.1234567", 11) + '|'(1)
   + lon("-180.1234567", 12) + '\n'(1) == 43 bytes. Rounded up with margin. */
#define FEB_MESH_LOG_LINE_MAX_LEN 96u

/* One "node_id|network|lat|lon\n" line for a single decoded <mesh-log-record>. `lat`/`lon`
   are already-decoded decimal degrees (caller converts from lat_e7_offset/lon_e7_offset
   before calling, same division as wardriving_csv.c's own row formatter). Returns bytes
   written (excluding NUL), or 0 on failure (out_cap too small, or node_id/network empty or
   over their protocol-bound lengths). */
size_t feb_mesh_log_format_line(
    char* out,
    size_t out_cap,
    const char* node_id,
    size_t node_id_len,
    const char* network,
    size_t network_len,
    double lat,
    double lon);

/* Read-side counterpart to feb_mesh_log_format_line() above, added for the Flipper's Mesh Log
   display screen (docs/WARDRIVING_PUBLISH.md "Mesh node publishing"). Decoded fields are kept
   as display-ready decimal lat/lon (not re-encoded to e7_offset -- nothing on the Flipper side
   needs the wire encoding once a record is already off the wire and on disk). */
#define FEB_MESH_NODES_NODE_ID_DISPLAY_LEN (FEB_MESH_LOG_NODE_ID_MAX_LEN + 1u)
#define FEB_MESH_NODES_NETWORK_DISPLAY_LEN (FEB_MESH_LOG_NETWORK_MAX_LEN + 1u)

typedef struct {
    char node_id[FEB_MESH_NODES_NODE_ID_DISPLAY_LEN];
    char network[FEB_MESH_NODES_NETWORK_DISPLAY_LEN];
    double lat;
    double lon;
} feb_mesh_node_entry_t;

/* Parses one "node_id|network|lat|lon" line (no trailing '\n' -- caller strips it, same as
   wardriving_settings_load()'s own line-splitting convention in flipper_esp32_over_ble.c) into
   `out`. Returns false on any malformed line (wrong field count, empty/oversized node_id or
   network, or an unparsable lat/lon) -- the caller should skip the line and continue rather
   than abort the whole file parse (a hand-edited or partially-written file should degrade one
   line, not the whole screen). */
bool feb_mesh_log_parse_line(const char* line, size_t line_len, feb_mesh_node_entry_t* out);

/* Display-only variant of feb_mesh_node_entry_t (docs/HARDENING_BACKLOG.md H04) -- the Mesh
   Log screen's backing array (mesh_log_display_nodes[] in flipper_esp32_over_ble.c) only ever
   needs to render "%.5f,%.5f", so it stores lat/lon as int32_t e7 (degrees * 1e7, exact for
   every value this protocol ever produces -- see cbor_wardriving.h's own lat_e7_offset/
   lon_e7_offset convention) instead of two `double`s. NOT `float`: a float's ~7.2 significant
   digits does not cover a 3-digit-longitude e7 value (H04's own note). Cuts this struct from
   48 to 36 bytes at MESH_LOG_DISPLAY_MAX_NODES(64)'s unchanged capacity. */
typedef struct {
    char node_id[FEB_MESH_NODES_NODE_ID_DISPLAY_LEN];
    char network[FEB_MESH_NODES_NETWORK_DISPLAY_LEN];
    int32_t lat_e7;
    int32_t lon_e7;
} feb_mesh_node_display_entry_t;

/* Converts a parsed accumulator-file entry (decimal-degree doubles, from
   feb_mesh_log_parse_line() above) to the display-only e7 representation, rounding to the
   nearest 1e-7 degree -- exact for any value that was itself derived from an integer e7 wire
   offset, which is every value this project ever writes to the accumulator file (see
   mesh_nodes.c). Call once at insert/reload time, never per draw call; the draw call converts
   back to a double only for its own single `%.5f` snprintf, never storing one. */
void feb_mesh_node_entry_to_display(const feb_mesh_node_entry_t* src, feb_mesh_node_display_entry_t* dst);

#endif /* FEB_MESH_NODES_H */
