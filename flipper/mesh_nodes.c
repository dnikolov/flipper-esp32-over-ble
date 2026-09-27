/* See mesh_nodes.h for the format/scope note. */
#include "mesh_nodes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t feb_mesh_log_format_line(
    char* out,
    size_t out_cap,
    const char* node_id,
    size_t node_id_len,
    const char* network,
    size_t network_len,
    double lat,
    double lon) {
    if(out_cap == 0 || node_id == NULL || network == NULL) {
        return 0;
    }
    if(node_id_len == 0 || node_id_len > FEB_MESH_LOG_NODE_ID_MAX_LEN ||
       network_len == 0 || network_len > FEB_MESH_LOG_NETWORK_MAX_LEN) {
        return 0;
    }

    int written = snprintf(
        out,
        out_cap,
        "%.*s|%.*s|%.7f|%.7f\n",
        (int)node_id_len,
        node_id,
        (int)network_len,
        network,
        lat,
        lon);
    if(written < 0 || (size_t)written >= out_cap) {
        return 0;
    }
    return (size_t)written;
}

bool feb_mesh_log_parse_line(const char* line, size_t line_len, feb_mesh_node_entry_t* out) {
    if(line == NULL || out == NULL || line_len == 0) {
        return false;
    }

    const char* p = line;
    size_t remaining = line_len;

    const char* sep1 = memchr(p, '|', remaining);
    if(sep1 == NULL) {
        return false;
    }
    size_t node_id_len = (size_t)(sep1 - p);
    if(node_id_len == 0 || node_id_len > FEB_MESH_LOG_NODE_ID_MAX_LEN) {
        return false;
    }

    const char* p2 = sep1 + 1;
    size_t remaining2 = remaining - node_id_len - 1;
    const char* sep2 = memchr(p2, '|', remaining2);
    if(sep2 == NULL) {
        return false;
    }
    size_t network_len = (size_t)(sep2 - p2);
    if(network_len == 0 || network_len > FEB_MESH_LOG_NETWORK_MAX_LEN) {
        return false;
    }

    const char* p3 = sep2 + 1;
    size_t remaining3 = remaining2 - network_len - 1;
    const char* sep3 = memchr(p3, '|', remaining3);
    if(sep3 == NULL) {
        return false;
    }
    size_t lat_len = (size_t)(sep3 - p3);
    if(lat_len == 0 || lat_len >= 32) {
        return false;
    }

    const char* p4 = sep3 + 1;
    size_t lon_len = remaining3 - lat_len - 1;
    if(lon_len == 0 || lon_len >= 32) {
        return false;
    }

    char lat_buf[32];
    char lon_buf[32];
    memcpy(lat_buf, p3, lat_len);
    lat_buf[lat_len] = '\0';
    memcpy(lon_buf, p4, lon_len);
    lon_buf[lon_len] = '\0';

    char* end = NULL;
    double lat = strtod(lat_buf, &end);
    if(end == lat_buf || *end != '\0') {
        return false;
    }
    end = NULL;
    double lon = strtod(lon_buf, &end);
    if(end == lon_buf || *end != '\0') {
        return false;
    }

    memcpy(out->node_id, p, node_id_len);
    out->node_id[node_id_len] = '\0';
    memcpy(out->network, p2, network_len);
    out->network[network_len] = '\0';
    out->lat = lat;
    out->lon = lon;
    return true;
}
