/* Internal codec plumbing shared across the cbor_*.c translation units that make up the
   split codec (cbor_primitives.c/cbor_records.c/cbor_wifi_scan.c/cbor_ble_scan.c/
   cbor_wardriving.c). This is NOT part of the project's shared-header API contract the way
   cbor_codec.h is -- it has no obligation to match anything on the Flipper side, since it is
   pure ESP32-side implementation detail (a macro used while building the wire encoders),
   never a declared type or prototype another module or firmware depends on. */
#ifndef FEB_CBOR_INTERNAL_H
#define FEB_CBOR_INTERNAL_H

#include <stddef.h>

/* Byte length of a string literal excluding its trailing NUL, e.g.
   feb_cbor_encode_text(out, cap, "ssid", FEB_CBOR_I_KLEN("ssid")). */
#define FEB_CBOR_I_KLEN(literal) (sizeof(literal) - 1u)

#endif /* FEB_CBOR_INTERNAL_H */
