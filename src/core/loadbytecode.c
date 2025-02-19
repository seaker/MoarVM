#include "moar.h"

/* Handles loading of bytecode, including triggering the deserialize and load
 * special frames. Takes place in two steps, with a callback between them which
 * is triggered by the special_return mechanism. */
typedef struct {
    MVMCompUnit *cu;
} CompUnitToLoad;
static void run_load(MVMThreadContext *tc, MVMCompUnit *cu);
static void sr_on_return(MVMThreadContext *tc, void *sr_data) {
    CompUnitToLoad *cul = (CompUnitToLoad *)sr_data;
    run_load(tc, cul->cu);
}
static void mark_sr_data(MVMThreadContext *tc, void *sr_data, MVMGCWorklist *worklist) {
    CompUnitToLoad *cul = (CompUnitToLoad *)sr_data;
    MVM_gc_worklist_add(tc, worklist, &(cul->cu));
}
static void run_comp_unit(MVMThreadContext *tc, MVMCompUnit *cu) {
    /* If there's a deserialization frame, need to run that. */
    if (cu->body.deserialize_frame) {
        /* Set up special return to delegate to running the load frame,
         * if any. */
        tc->cur_frame->return_value = NULL;
        tc->cur_frame->return_type = MVM_RETURN_VOID;
        CompUnitToLoad *cul = MVM_callstack_allocate_special_return(tc,
                sr_on_return, NULL, mark_sr_data, sizeof(CompUnitToLoad));
        cul->cu = cu;

        /* Invoke the deserialization frame and return to the runloop. */
        MVM_frame_dispatch_zero_args(tc, cu->body.deserialize_frame->body.static_code);
    }
    else {
        /* No deserialize frame, so do load frame instead. */
        run_load(tc, cu);
    }
}
void MVM_load_bytecode_buffer(MVMThreadContext *tc, MVMObject *buf) {
    MVMCompUnit *cu;
    MVMuint8    *data_start;
    MVMuint32    data_size;

    /* Ensure the source is in the correct form. */
    if (
        !IS_CONCRETE(buf)
        || REPR(buf)->ID != MVM_REPR_ID_VMArray
        || (
               ((MVMArrayREPRData *)STABLE(buf)->REPR_data)->slot_type != MVM_ARRAY_U8
            && ((MVMArrayREPRData *)STABLE(buf)->REPR_data)->slot_type != MVM_ARRAY_I8
        )
    )
        MVM_exception_throw_adhoc(tc, "loadbytecodebuffer requires a native int8 or uint8 array to read from");

    unsigned long interval_id = MVM_telemetry_interval_start(tc, "loadbytecodebuffer");

    /* MVMCompUnit expects the data to be non-GC managed as it usually comes straight from a file */
    data_size = ((MVMArray *)buf)->body.elems;

    MVM_telemetry_interval_annotate((uintptr_t)data_size, interval_id, "this size");

    data_start = MVM_malloc(data_size);
    memcpy(data_start, (MVMuint8 *)(((MVMArray *)buf)->body.slots.i8 + ((MVMArray *)buf)->body.start), data_size);

    cu = MVM_cu_from_bytes(tc, data_start, data_size);

    MVM_telemetry_interval_stop(tc, interval_id, "done mapping");

    run_comp_unit(tc, cu);
}
void MVM_load_bytecode_buffer_to_cu(MVMThreadContext *tc, MVMObject *buf, MVMRegister *res) {
    MVMCompUnit *cu;
    MVMuint8    *data_start;
    MVMuint32    data_size;

    /* Ensure the source is in the correct form. */
    if (
        !IS_CONCRETE(buf)
        || REPR(buf)->ID != MVM_REPR_ID_VMArray
        || (
               ((MVMArrayREPRData *)STABLE(buf)->REPR_data)->slot_type != MVM_ARRAY_U8
            && ((MVMArrayREPRData *)STABLE(buf)->REPR_data)->slot_type != MVM_ARRAY_I8
        )
    )
        MVM_exception_throw_adhoc(tc, "loadbytecodebuffer requires a native int8 or uint8 array to read from");

    unsigned long interval_id = MVM_telemetry_interval_start(tc, "buffertocu");

    /* MVMCompUnit expects the data to be non-GC managed as it usually comes straight from a file */
    data_size = ((MVMArray *)buf)->body.elems;

    MVM_telemetry_interval_annotate((uintptr_t)data_size, interval_id, "this size");

    data_start = MVM_malloc(data_size);
    memcpy(data_start, (MVMuint8 *)(((MVMArray *)buf)->body.slots.i8 + ((MVMArray *)buf)->body.start), data_size);

    cu = MVM_cu_from_bytes(tc, data_start, data_size);

    MVM_telemetry_interval_stop(tc, interval_id, "done mapping");

    cu->body.deallocate = MVM_DEALLOCATE_FREE;
    res->o = (MVMObject *)cu;

    if (cu->body.deserialize_frame) {
        /* Set up special return to delegate to running the load frame,
         * if any. */
        tc->cur_frame->return_value             = NULL;
        tc->cur_frame->return_type              = MVM_RETURN_VOID;

        /* Invoke the deserialization frame and return to the runloop. */
        MVM_frame_dispatch_zero_args(tc, cu->body.deserialize_frame->body.static_code);
    }
}
void MVM_load_bytecode(MVMThreadContext *tc, MVMString *filename) {

    /* Work out actual filename to use, taking --libpath into account. */
    filename = MVM_file_in_libpath(tc, filename);

    if (!MVM_str_hash_key_is_valid(tc, filename)) {
        MVM_str_hash_key_throw_invalid(tc, filename);
    }

    /* See if we already loaded this. */
    uv_mutex_lock(&tc->instance->mutex_loaded_compunits);
    MVM_tc_set_ex_release_mutex(tc, &tc->instance->mutex_loaded_compunits);
    if (MVM_fixkey_hash_fetch_nocheck(tc, &tc->instance->loaded_compunits, filename)) {
        /* already loaded */
        goto LEAVE;
    }

    /* Otherwise, load from disk. */
    MVMROOT(tc, filename) {
        char *c_filename = MVM_string_utf8_c8_encode_C_string(tc, filename);
        MVMCompUnit *cu = MVM_cu_map_from_file(tc, c_filename, 1);
        cu->body.filename = filename;
        MVM_gc_write_barrier_hit(tc, (MVMCollectable *)cu);

        run_comp_unit(tc, cu);

        MVMString **key = MVM_fixkey_hash_insert_nocheck(tc, &tc->instance->loaded_compunits, filename);
        MVM_gc_root_add_permanent_desc(tc, (MVMCollectable **)key,
                                       "Loaded compilation unit filename");
    }

LEAVE:
    MVM_tc_clear_ex_release_mutex(tc);
    uv_mutex_unlock(&tc->instance->mutex_loaded_compunits);
}
void MVM_load_bytecode_fh(MVMThreadContext *tc, MVMObject *oshandle, MVMString *filename) {
    MVMCompUnit *cu;

    if (REPR(oshandle)->ID != MVM_REPR_ID_MVMOSHandle)
        MVM_exception_throw_adhoc(tc, "loadbytecodefh requires an object with REPR MVMOSHandle");

    MVMROOT(tc, filename) {
        MVMuint64 pos = MVM_io_tell(tc, oshandle);
        cu = MVM_cu_map_from_file_handle(tc, MVM_io_fileno(tc, oshandle), pos);
        cu->body.filename = filename;
        MVM_gc_write_barrier_hit(tc, (MVMCollectable *)cu);

        run_comp_unit(tc, cu);
    }
}

/* Callback after running deserialize code to run the load code. */
static void run_load(MVMThreadContext *tc, MVMCompUnit *cu) {
    /* If there's a load frame, need to run that. If not, we're done. */
    if (cu->body.load_frame) {
        /* Make sure the call happens in void context. No special return
         * handler here; we want to go back to the place that used the
         * loadbytecode op in the first place. */
        tc->cur_frame->return_value = NULL;
        tc->cur_frame->return_type  = MVM_RETURN_VOID;

        /* Invoke the load frame and return to the runloop. */
        MVM_frame_dispatch_zero_args(tc, cu->body.load_frame->body.static_code);
    }
}
