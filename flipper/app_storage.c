#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif

/* Resolved once, from this app's own thread, at app init (see flipper_esp32_over_ble_app())
   -- never call APP_DATA_PATH() again after this. `APP_DATA_PATH(...)` is a pure compile-
   time string concatenation ("/data/" + path); the "/data" prefix is only resolved to a
   real `/ext/apps_data/<app id>/...` path when a storage_* call carrying it is issued, and
   that resolution keys off the *calling* thread's registered app id (confirmed in
   applications/services/storage/storage_processing.c's storage_process_alias(), via
   furi_thread_get_appid(thread_id)). handle_pair_complete() (and therefore
   pairing_storage_save()) run synchronously inside profile_event_handler() on the BLE
   stack's own "BleEventWorker" thread, owned by the firmware's built-in `bt` service --
   any `APP_DATA_PATH(...)` call made from that thread resolves against `bt`'s app id, not
   this app's, silently writing pairing files under the wrong directory (found during the
   step 5 hardware-verification pass, see docs/SESSION_MEMORY.md). Fixed by resolving the
   alias exactly once, synchronously, from this app's own thread via the exported
   storage_common_resolve_path_and_ensure_app_directory() (captures
   furi_thread_get_current_id() of its *caller*, not of whichever thread later reuses the
   resulting string), then caching the resulting absolute path (already starting with
   "/ext/apps_data/...", not "/data") for every later pairing-file path build -- a path
   that doesn't start with "/data" is used as a literal absolute path by every storage_*
   call regardless of which thread issues it, so this is safe to reuse from
   BleEventWorker afterward. */

APP_FN bool resolve_pairings_dir_path(Storage* storage) {
    FuriString* resolved = furi_string_alloc_set_str(APP_DATA_PATH(PAIRING_DIR_NAME));
    storage_common_resolve_path_and_ensure_app_directory(storage, resolved);
    bool ok = furi_string_size(resolved) < sizeof(app_pairings_dir_path);
    if(ok) {
        strncpy(app_pairings_dir_path, furi_string_get_cstr(resolved), sizeof(app_pairings_dir_path) - 1);
        app_pairings_dir_path[sizeof(app_pairings_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved pairings path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    /* storage_common_resolve_path_and_ensure_app_directory() only guarantees
       /ext/apps_data/<app id> exists, not our own "pairings" subdirectory beneath it. */
    FS_Error mkdir_err = storage_common_mkdir(storage, app_pairings_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir pairings dir failed: %d", mkdir_err);
        return false;
    }
    return true;
}

static bool build_pairing_path(
    char* out,
    size_t out_cap,
    const char* board_id,
    size_t board_id_len,
    bool tmp) {
    if(!app_pairings_dir_ready) {
        return false;
    }
    int written = snprintf(
        out,
        out_cap,
        "%s/%.*s%s",
        app_pairings_dir_path,
        (int)board_id_len,
        board_id,
        tmp ? ".dat.tmp" : ".dat");
    return written > 0 && (size_t)written < out_cap;
}

/* Same resolve-once-from-this-app's-own-thread rationale as resolve_pairings_dir_path()
   above -- a separate subdirectory, never nested inside "pairings", so an unpair (step 8)
   can delete each independently while still deleting both together as one operation. */
APP_FN bool resolve_capabilities_dir_path(Storage* storage) {
    FuriString* resolved = furi_string_alloc_set_str(APP_DATA_PATH(CAPABILITY_DIR_NAME));
    storage_common_resolve_path_and_ensure_app_directory(storage, resolved);
    bool ok = furi_string_size(resolved) < sizeof(app_capabilities_dir_path);
    if(ok) {
        strncpy(
            app_capabilities_dir_path, furi_string_get_cstr(resolved), sizeof(app_capabilities_dir_path) - 1);
        app_capabilities_dir_path[sizeof(app_capabilities_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved capabilities path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    FS_Error mkdir_err = storage_common_mkdir(storage, app_capabilities_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir capabilities dir failed: %d", mkdir_err);
        return false;
    }
    return true;
}

static bool build_capability_path(
    char* out,
    size_t out_cap,
    const char* board_id,
    size_t board_id_len,
    bool tmp) {
    if(!app_capabilities_dir_ready) {
        return false;
    }
    int written = snprintf(
        out,
        out_cap,
        "%s/%.*s%s",
        app_capabilities_dir_path,
        (int)board_id_len,
        board_id,
        tmp ? ".dat.tmp" : ".dat");
    return written > 0 && (size_t)written < out_cap;
}

/* Same resolve-once-from-this-app's-own-thread rationale as resolve_pairings_dir_path()
   above. Resolves the app data root itself (no subdirectory, no mkdir needed beyond what
   storage_common_resolve_path_and_ensure_app_directory() already guarantees) -- only the two
   publish-flow plumbing files (publish-result, and the host-script-owned credentials file)
   live flat here; the wardriving CSV itself lives one level down, see
   resolve_wardriving_dir_path() below. APP_DATA_PATH("") is "/data/" (trailing slash, from
   the macro's own "/" + path concatenation); trimmed back off after resolution so every
   build_app_data_path() call below doesn't produce a doubled "//" before the filename. */
APP_FN bool resolve_app_data_root_path(Storage* storage) {
    FuriString* resolved = furi_string_alloc_set_str(APP_DATA_PATH(""));
    storage_common_resolve_path_and_ensure_app_directory(storage, resolved);
    size_t resolved_len = furi_string_size(resolved);
    if(resolved_len > 0 && furi_string_get_char(resolved, resolved_len - 1) == '/') {
        furi_string_left(resolved, resolved_len - 1);
    }
    bool ok = furi_string_size(resolved) < sizeof(app_data_root_path);
    if(ok) {
        strncpy(app_data_root_path, furi_string_get_cstr(resolved), sizeof(app_data_root_path) - 1);
        app_data_root_path[sizeof(app_data_root_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved app data root path too long to cache");
    }
    furi_string_free(resolved);
    return ok;
}

/* Same resolve-once-and-mkdir shape as resolve_pairings_dir_path() -- this one owns the
   wardriving CSV's own subdirectory (WARDRIVING_DIR_NAME), which holds both the live
   wardriving_current.csv and, after a successful publish, the host script's timestamped
   archives (docs/WARDRIVING_PUBLISH.md "Result handling"). Depends on app_data_root_path
   already being resolved -- called after resolve_app_data_root_path() at app startup. */
APP_FN bool resolve_wardriving_dir_path(Storage* storage) {
    if(!app_data_root_ready) {
        return false;
    }
    FuriString* resolved = furi_string_alloc_printf("%s/%s", app_data_root_path, WARDRIVING_DIR_NAME);
    bool ok = furi_string_size(resolved) < sizeof(app_wardriving_dir_path);
    if(ok) {
        strncpy(app_wardriving_dir_path, furi_string_get_cstr(resolved), sizeof(app_wardriving_dir_path) - 1);
        app_wardriving_dir_path[sizeof(app_wardriving_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved wardriving dir path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    FS_Error mkdir_err = storage_common_mkdir(storage, app_wardriving_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir wardriving dir failed: %d", mkdir_err);
        return false;
    }
    return true;
}

/* Builds "<wardriving dir>/<filename>" -- used for the wardriving CSV only (current file
   today; the host script's own renamed archives land in this same directory but this FAP
   never builds those paths itself). Returns false if the directory wasn't resolved at init
   or the result would truncate. */
APP_FN bool build_wardriving_path(char* out, size_t out_cap, const char* filename) {
    if(!app_wardriving_dir_ready) {
        return false;
    }
    int written = snprintf(out, out_cap, "%s/%s", app_wardriving_dir_path, filename);
    return written > 0 && (size_t)written < out_cap;
}

/* Same resolve-once-and-mkdir shape as resolve_wardriving_dir_path() above -- this one owns
   the mesh_log accumulator's own subdirectory (MESH_DIR_NAME), sibling of
   WARDRIVING_DIR_NAME. Depends on app_data_root_path already being resolved -- called after
   resolve_app_data_root_path() at app startup. */
APP_FN bool resolve_mesh_dir_path(Storage* storage) {
    if(!app_data_root_ready) {
        return false;
    }
    FuriString* resolved = furi_string_alloc_printf("%s/%s", app_data_root_path, MESH_DIR_NAME);
    bool ok = furi_string_size(resolved) < sizeof(app_mesh_dir_path);
    if(ok) {
        strncpy(app_mesh_dir_path, furi_string_get_cstr(resolved), sizeof(app_mesh_dir_path) - 1);
        app_mesh_dir_path[sizeof(app_mesh_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved mesh dir path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    FS_Error mkdir_err = storage_common_mkdir(storage, app_mesh_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir mesh dir failed: %d", mkdir_err);
        return false;
    }
    return true;
}

/* Builds "<mesh dir>/<filename>" -- used for the mesh_log accumulator only (current file
   today; a future host-script archiving pass would land renamed files in this same
   directory, out of scope for this pass -- see mesh_nodes.h). Returns false if the
   directory wasn't resolved at init or the result would truncate. */
APP_FN bool build_mesh_path(char* out, size_t out_cap, const char* filename) {
    if(!app_mesh_dir_ready) {
        return false;
    }
    int written = snprintf(out, out_cap, "%s/%s", app_mesh_dir_path, filename);
    return written > 0 && (size_t)written < out_cap;
}

/* Builds "<app data root>/<filename>" into `out` -- used for the publish-flow plumbing files
   (publish-result, and the host-script-owned credentials file), both flat at the app data
   root. Returns false if the root wasn't resolved at init or the result would truncate. */
APP_FN bool build_app_data_path(char* out, size_t out_cap, const char* filename) {
    if(!app_data_root_ready) {
        return false;
    }
    int written = snprintf(out, out_cap, "%s/%s", app_data_root_path, filename);
    return written > 0 && (size_t)written < out_cap;
}

/* Free-heap floor enforced before this app asks the Storage service to open a file
   (docs/HARDENING_BACKLOG.md H04, and the 2026-09-28 user-reported "Flipper crashed and was
   rebooted, out of memory" during a wardriving backlog flush).

   Opening a file is not a cheap call on this firmware: storage_ext.c's `storage_process_file_open()`
   mallocs an `SDFile` wrapping a FatFS `FIL`, which carries its own 512-byte sector buffer --
   roughly 600 contiguous bytes out of the same system heap that already holds every one of
   this FAP's `.text`/`.rodata`/`.data`/`.bss` sections (the ELF loader aligned_malloc()s each
   one at launch, see H04). That allocation goes through `pvPortMalloc()`, which
   `furi_check(pvReturn, "out of memory")`s on failure -- i.e. a failed open does not return an
   error this app could handle, it reboots the whole device. Checking the margin first turns
   that reboot into this app's own visible, recoverable failure path ("CSV export write
   failed" on screen, session still alive, records still accumulating on the ESP32's flash
   backlog for a later drain).

   This is a heuristic, not a guarantee -- another thread can allocate between this check and
   the Storage service's own malloc, and nothing here protects the GUI/BLE/notification
   allocations happening concurrently. It removes the single most likely crash point, which is
   a real improvement over rebooting, not a claim that the app is now OOM-proof. The only
   actual fix for the underlying pressure is footprint reduction (see the .bss work in the same
   pass as this comment).

   Threshold: the ~600-byte SDFile/FIL allocation plus the Storage service's own per-call churn
   and a deliberate cushion, since the check is racy by nature. Uses
   memmgr_heap_get_max_free_block() rather than memmgr_get_free_heap() because the allocation
   that fails needs one *contiguous* block, which is exactly H04's already-confirmed
   fragmentation mechanism -- total free bytes can look healthy while no single block fits.

   `logged_once` is a caller-owned latch, not a nicety: FURI_LOG_* is itself a heap allocation
   (furi/core/log.c's furi_log_print_format() does furi_string_alloc() then grows it with
   furi_string_vprintf()), so logging on the very path that just measured the heap as too tight
   is the one place a log line can plausibly be the allocation that trips
   furi_check(pvReturn, "out of memory"). One line per session per call site is worth that
   risk for diagnosis; one per wardriving batch, for minutes on end, is not. */
#define FEB_STORAGE_OPEN_MIN_FREE_BLOCK 3072u

APP_FN bool storage_open_heap_margin_ok(const char* what, bool* logged_once) {
    size_t largest = memmgr_heap_get_max_free_block();
    if(largest >= FEB_STORAGE_OPEN_MIN_FREE_BLOCK) {
        return true;
    }
    if(!*logged_once) {
        *logged_once = true;
        FURI_LOG_E(
            TAG,
            "%s: deferring file open, largest free heap block %u < %u (free heap %u)",
            what,
            (unsigned)largest,
            (unsigned)FEB_STORAGE_OPEN_MIN_FREE_BLOCK,
            (unsigned)memmgr_get_free_heap());
    }
    return false;
}

/* Atomic per-board persistence: temp-file write, exact-length verification,
   storage_file_sync(), close, remove-old, rename -- the archive_favorites.c precedent
   (docs/PLAN.md step 5) with the missing sync call added. One file per board_id so
   replacing one board's pairing can never touch another's. */
APP_FN bool
    pairing_storage_save(Storage* storage, const char* board_id, size_t board_id_len, const uint8_t* secret) {
    static char final_path[FEB_PAIRINGS_PATH_MAX_LEN];
    static char tmp_path[FEB_PAIRINGS_PATH_MAX_LEN];
    if(!build_pairing_path(final_path, sizeof(final_path), board_id, board_id_len, false) ||
       !build_pairing_path(tmp_path, sizeof(tmp_path), board_id, board_id_len, true)) {
        FURI_LOG_E(TAG, "pairing_storage_save: path build failed for board_id '%.*s'",
                   (int)board_id_len, board_id);
        return false;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        size_t written = storage_file_write(file, secret, FEB_PAIRING_SECRET_LEN);
        ok = (written == FEB_PAIRING_SECRET_LEN) && storage_file_sync(file);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok) {
        storage_common_remove(storage, tmp_path);
        return false;
    }

    storage_common_remove(storage, final_path);
    FS_Error rename_err = storage_common_rename(storage, tmp_path, final_path);
    if(rename_err != FSE_OK) {
        FURI_LOG_E(TAG, "rename pairing file failed: %d", rename_err);
        storage_common_remove(storage, tmp_path);
        return false;
    }
    return true;
}

/* Loads a previously-saved pairing_secret for `board_id`. Returns false uniformly for "no
   file" and "file present but unreadable/wrong length" -- docs/PROTOCOL.md's `unknown_board`
   response doesn't distinguish these causes to the peer either (do not expose the cause,
   matching this file's existing pairing_failed convention); the real reason is still
   logged locally for diagnostics. */
APP_FN bool
    pairing_storage_load(Storage* storage, const char* board_id, size_t board_id_len, uint8_t* secret_out) {
    static char path[FEB_PAIRINGS_PATH_MAX_LEN];
    if(!build_pairing_path(path, sizeof(path), board_id, board_id_len, false)) {
        FURI_LOG_E(TAG, "pairing_storage_load: path build failed for board_id '%.*s'",
                   (int)board_id_len, board_id);
        return false;
    }
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        size_t read = storage_file_read(file, secret_out, FEB_PAIRING_SECRET_LEN);
        ok = (read == FEB_PAIRING_SECRET_LEN);
        if(!ok) {
            FURI_LOG_W(TAG, "Pairing file for '%.*s' unreadable or wrong length", (int)board_id_len, board_id);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

APP_FN bool any_saved_pairing_exists(Storage* storage) {
    if(!app_pairings_dir_ready) {
        return false;
    }
    File* dir = storage_file_alloc(storage);
    bool found = false;
    if(storage_dir_open(dir, app_pairings_dir_path)) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            size_t len = strlen(name);
            if(len > 4 && strcmp(name + len - 4, ".dat") == 0) {
                found = true;
                break;
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return found;
}

APP_FN bool
    capability_storage_exists(Storage* storage, const char* board_id, size_t board_id_len) {
    static char path[FEB_CAPABILITIES_PATH_MAX_LEN];
    if(!build_capability_path(path, sizeof(path), board_id, board_id_len, false)) {
        FURI_LOG_E(TAG, "capability_storage_exists: path build failed for board_id '%.*s'",
                   (int)board_id_len, board_id);
        return false;
    }
    return storage_file_exists(storage, path);
}

/* Persists the raw canonical-CBOR `capability_response` payload bytes verbatim (docs/
   CAPABILITIES.md "Storage and persistence") -- same atomic temp-file/exact-write/
   storage_file_sync()/close/rename sequence as pairing_storage_save(), but a variable
   length rather than a fixed FEB_PAIRING_SECRET_LEN. */
APP_FN bool capability_storage_save(
    Storage* storage,
    const char* board_id,
    size_t board_id_len,
    const uint8_t* payload,
    size_t payload_len) {
    static char final_path[FEB_CAPABILITIES_PATH_MAX_LEN];
    static char tmp_path[FEB_CAPABILITIES_PATH_MAX_LEN];
    if(!build_capability_path(final_path, sizeof(final_path), board_id, board_id_len, false) ||
       !build_capability_path(tmp_path, sizeof(tmp_path), board_id, board_id_len, true)) {
        FURI_LOG_E(TAG, "capability_storage_save: path build failed for board_id '%.*s'",
                   (int)board_id_len, board_id);
        return false;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        size_t written = storage_file_write(file, payload, payload_len);
        ok = (written == payload_len) && storage_file_sync(file);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok) {
        storage_common_remove(storage, tmp_path);
        return false;
    }

    storage_common_remove(storage, final_path);
    FS_Error rename_err = storage_common_rename(storage, tmp_path, final_path);
    if(rename_err != FSE_OK) {
        FURI_LOG_E(TAG, "rename capability file failed: %d", rename_err);
        storage_common_remove(storage, tmp_path);
        return false;
    }
    return true;
}

/* Loads a previously-cached capability_response payload for `board_id` into `out` (capacity
   `out_cap`, callers pass FEB_CBOR_MAX_PAYLOAD). Returns false uniformly for "no file",
   "unreadable", and "too large for out_cap" -- matches pairing_storage_load()'s
   don't-expose-the-cause convention, though this cache is non-sensitive. */
APP_FN bool capability_storage_load(
    Storage* storage,
    const char* board_id,
    size_t board_id_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    static char path[FEB_CAPABILITIES_PATH_MAX_LEN];
    if(!build_capability_path(path, sizeof(path), board_id, board_id_len, false)) {
        FURI_LOG_E(TAG, "capability_storage_load: path build failed for board_id '%.*s'",
                   (int)board_id_len, board_id);
        return false;
    }
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        uint64_t size = storage_file_size(file);
        if(size == 0 || size > out_cap) {
            ok = false;
        } else {
            size_t read = storage_file_read(file, out, (size_t)size);
            ok = (read == (size_t)size);
            if(ok) {
                *out_len = read;
            }
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}


/* Shared by wardriving_settings_load()/_save() (never nested, both main-thread-only) and by
   wardriving_csv_count_rows()'s recount loop further down. */
