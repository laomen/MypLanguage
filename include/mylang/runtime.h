#ifndef MYLANG_RUNTIME_H
#define MYLANG_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Basic I/O ----
void myp_print(const char* str);
void myp_println(const char* str);
void myp_print_int(int32_t val);
void myp_print_long(int64_t val);
void myp_print_float(double val);
void myp_print_bool(int32_t val);

// ---- Memory ----
void* myp_alloc(size_t size);
void myp_free(void* ptr);
// Free all allocations made by the current thread (called at main exit)
void myp_free_all(void);

// ---- ARC (automatic reference counting on class instances, §五-1) ----
// Object layout: 8-byte header { rc:u32, type_id:u32 } sits *before* the data
// pointer returned to codegen. myp_alloc_object allocates size+header, sets
// rc=1 + type_id, and returns the data pointer (base+8). myp_retain/myp_release
// operate on the data pointer and reach the header at (obj-8). myp_release
// reaching rc=0 dispatches to the per-TU __myp_release_table[type_id] destroy
// stub, which cascades reference fields then calls myp_free_object.
void* myp_alloc_object(size_t size, uint32_t type_id);
void myp_retain(void* obj);
uint32_t myp_release(void* obj);
void myp_free_object(void* obj);
// M7 weak references: a `@weak` class field stores a plain pointer to its
// target (no retain) in a slot whose ADDRESS is registered in a global weak
// registry. myp_weak_store sets the slot + updates the registry; myp_weak_load
// upgrades weak→strong (returns a fresh strong ref the caller must release, or
// NULL if the target died); myp_weak_clear unregisters + nulls the slot (called
// from a holder's destroy stub when the HOLDER is freed). When a target's rc
// hits 0, myp_release nulls every registered slot before freeing.
void myp_weak_store(void** slot, void* obj);
void* myp_weak_load(void** slot);
void myp_weak_clear(void** slot);
// Ref-counted class arrays (§五-1): `new T[n]` with a class element type. A
// 24-byte header { count:u64, elem_size:u32, pad:u32, rc:u32, type_id:u32 }
// (rc/type_id at the same obj-8/obj-4 offsets as a class-object header) sits
// before the element-data pointer returned to codegen, so myp_retain/myp_release
// treat arrays uniformly; myp_release on an array (type_id==MYP_ARR_TYPE_ID)
// releases every element then frees the header.
void* myp_alloc_class_array(uint64_t count, uint32_t elem_size);
// `slice<T>` with class T uses the same counted backing layout, but its slice
// value is borrowed/copyable. The runtime owns one cleanup registration for
// the backing and releases it at region or thread/process teardown.
void* myp_alloc_class_slice(uint64_t count);
void myp_release_fixed_class_array(void* data, uint64_t count);
// Per-TU destroy-stub table, defined by generated code (indexed by type_id).
extern void (*__myp_release_table[])(void*);
// Per-TU class-name table, defined by generated code (indexed by type_id,
// index 0 = ""). Backs the RTTI intrinsics (§五-4).
extern const char* __myp_type_name_table[];
// Number of live (not yet freed) class instances on this thread — diagnostic.
int64_t myp_live_object_count(void);
// M9 diagnostics: live counted strings, live array/slice backings, total, and
// per-type class-instance count (0 if type_id unknown). Plus strict-mode toggle
// (release underflow / corrupted header → abort) and deterministic allocation
// failure injection (the Nth allocation reaching the allocator aborts).
int64_t myp_live_string_count(void);
int64_t myp_live_array_count(void);
int64_t myp_live_total_count(void);
int64_t myp_live_object_count_by_type(int64_t type_id);
void myp_diag_set_strict(int64_t on);
int64_t myp_diag_get_strict(void);
void myp_fail_alloc_enable(int64_t nth);
void myp_fail_alloc_disable(void);
int64_t myp_fail_alloc_get(void);

// ---- RTTI (§五-4) ----
// Runtime type id of a class instance (0 = null / non-class object).
int myp_obj_type_id(void* obj);
// Class name for a type id ("" for 0 / unknown).
const char* myp_type_name(int type_id);
// Runtime type name of a class instance ("" if null / non-class).
const char* myp_obj_type_name(void* obj);

// ---- Timeline ----
int64_t myp_now_ms(void);
int64_t myp_now_realtime_ms(void);
void myp_sleep_ms(int64_t ms);

// ---- Date / Time Formatting ----
char* myp_date_format(const char* fmt);
char* myp_date_format_ms(int64_t ms, const char* fmt);
int32_t myp_date_field(int64_t ms, int32_t field);

// ---- String ----
char* myp_strcat(const char* a, const char* b);
// In-place append (`s = s + x` fast path): consumes `s` (reuses its buffer when
// unique, else frees it) and returns a fresh counted string = concat(s, x).
char* myp_str_append(char* s, const char* x);
char* myp_strdup(const char* s);
int32_t myp_str_eq(const char* a, const char* b);
int32_t myp_str_contains(const char* s, const char* sub);
int32_t myp_str_index_of(const char* s, const char* sub);
int32_t myp_str_starts_with(const char* s, const char* prefix);
int32_t myp_str_ends_with(const char* s, const char* suffix);
char* myp_str_substring(const char* s, int32_t start, int32_t end);
char* myp_str_replace(const char* s, const char* old_str, const char* new_str);
char* myp_str_to_upper(const char* s);
char* myp_str_to_lower(const char* s);
char* myp_str_trim(const char* s);
int32_t myp_str_split_count(const char* s, const char* delim);
char* myp_str_split_get(const char* s, const char* delim, int32_t index);

// ---- JSON Parser ----
int64_t myp_json_parse(const char* json_str);
int32_t myp_json_get_type(int64_t handle, const char* path);
const char* myp_json_get_string(int64_t handle, const char* path);
double myp_json_get_number(int64_t handle, const char* path);
int32_t myp_json_get_bool(int64_t handle, const char* path);
int32_t myp_json_array_length(int64_t handle, const char* path);
void myp_json_free(int64_t handle);

// ---- String Hash ----
int32_t myp_str_hash(const char* s);

// ---- File System ----
int32_t myp_fs_exists(const char* path);
int32_t myp_fs_is_dir(const char* path);
int32_t myp_fs_is_file(const char* path);
int64_t myp_fs_file_size(const char* path);
int64_t myp_fs_modified_time(const char* path);
int32_t myp_fs_list_count(const char* path);
char* myp_fs_list_get(const char* path, int32_t index);
char* myp_fs_dirname(const char* path);
char* myp_fs_basename(const char* path);
char* myp_fs_join(const char* dir, const char* file);

// ---- Networking (TCP Sockets) ----
int32_t myp_net_server(int32_t port);
int32_t myp_net_accept(int32_t server_fd);
int32_t myp_net_connect(const char* host, int32_t port);
int32_t myp_net_send(int32_t fd, const char* data);
char* myp_net_recv(int32_t fd, int32_t max_len);
char* myp_net_recv_line(int32_t fd);
void myp_net_close(int32_t fd);
void myp_net_set_nonblock(int32_t fd);   // §五-5 P2: async socket IO

// ---- Async file IO (§五-5 P3, worker-pool executor) ----
char* myp_coro_file_read_line(int32_t io_handle);
char* myp_coro_file_read_all(int32_t io_handle);

// ---- §五-5 P4: unified waitAny (mix EVENT/TIMER/FD in one wait) ----
int64_t __myp_coro_wait_any_of(const int64_t* spec, int64_t count, int64_t timeout_ms,
                               int64_t val);

// ---- §五-1 收尾: coroutine-frame ARC registry (codegen-emitted) ----
void __myp_coro_frame_set(int64_t slot_id, int64_t obj);
void __myp_coro_frame_clear(int64_t slot_id);

// ---- Process Management ----
int32_t myp_process_run(const char* cmd);
char* myp_process_output(const char* cmd);
int32_t myp_process_get_pid(void);
int32_t myp_process_get_ppid(void);
int32_t myp_process_is_running(int32_t pid);

// ---- Command-Line Arguments ----
int32_t myp_args_count(void);
char* myp_args_get(int32_t index);

// ---- Environment Variables ----
char* myp_env_get(const char* name);
int32_t myp_env_set(const char* name, const char* value);
int32_t myp_env_unset(const char* name);

// ---- String Enhancements ----
char* myp_str_repeat(const char* s, int32_t count);
char* myp_str_pad_left(const char* s, int32_t total_len, int32_t pad_char);
char* myp_str_pad_right(const char* s, int32_t total_len, int32_t pad_char);

// ---- printf-style formatting (stdlib/fmt.myp) ----
char* myp_fmt_u64_base(int32_t v, int32_t base, int32_t upper);
char* myp_fmt_double_f(double v, int32_t prec);
char* myp_fmt_double_e(double v, int32_t prec);
char* myp_fmt_double_g(double v, int32_t prec);

// ---- Hashing (stdlib/crypto.myp) ----
int32_t myp_crc32(const char* msg);
char* myp_hash_md5(const char* msg);
char* myp_hash_sha1(const char* msg);
char* myp_hash_sha256(const char* msg);
char* myp_str_reverse(const char* s);
char* myp_str_replace_all(const char* s, const char* old_str, const char* new_str);

// ---- Regular Expressions (POSIX) ----
int64_t myp_regex_compile(const char* pattern);
int32_t myp_regex_match(int64_t handle, const char* s);
void myp_regex_free(int64_t handle);

// ---- Base64 ----
char* myp_base64_encode(const char* data);
char* myp_base64_decode(const char* data);

// ---- Flush stdout ----
void myp_flush(void);

// ---- GPU / CUDA offload ----
int   myp_gpu_init(void);
void* myp_gpu_alloc(size_t size);
void  myp_gpu_free(void* ptr);
void  myp_gpu_to_device(void* dst, const void* src, size_t size);
void  myp_gpu_to_host(void* dst, const void* src, size_t size);
void* myp_gpu_load_kernel(const char* ptx_str, const char* kernel_name);
int   myp_gpu_launch(void* kernel_ctx, unsigned int grid_dim_x, unsigned int block_dim_x,
                     void** args, unsigned int num_args);
void  myp_gpu_destroy_kernel(void* kernel_ctx);
// Device info (returns 0 / empty if GPU unavailable)
int    myp_gpu_device_count(void);
const char* myp_gpu_device_name(void);   // static buffer, valid until next call
long   myp_gpu_device_memory(void);      // total global memory in bytes
int    myp_gpu_compute_capability(void); // major*100 + minor, e.g. 860
int    myp_gpu_multi_processors(void);   // number of streaming multiprocessors
int    myp_gpu_max_threads_per_block(void);
int    myp_gpu_warp_size(void);

// ---- String to double ----
double myp_atof(const char* s);

// ---- Keyboard Input (non-blocking) ----
int32_t myp_kbhit(void);
int32_t myp_getch(void);

// ---- Timer System ----
// Create a timer that fires event_id on instance after delay_ms.
// interval_ms: 0 = one-shot, >0 = repeating interval.
// param is passed as event data.
// Returns 0 on success, -1 on failure.
int32_t myp_timer_create(int event_id, void* instance, int64_t delay_ms,
                         int64_t param, int64_t interval_ms);
// Cancel all timers for a given instance
void myp_timer_cancel_all(void* instance);
// Check and fire expired timers (called internally by event loop)
int myp_timer_check(void);

// ---- Event System ----
// Max number of event handlers per event type
#define MYP_MAX_HANDLERS 64

// Event handler function type: void (*handler)(void* instance, void* event_data)
typedef void (*myp_handler_fn)(void*, void*);

// Register a handler for an event type
void myp_event_register(int event_id, void* instance, myp_handler_fn handler);

// Scope-based handler management for mapping() @scope
void myp_event_push_scope(void);
void myp_event_pop_scope(void);

// Fire an event: pushes to queue and dispatches
void myp_event_fire(int event_id, void* sender, void* event_data);

// Process all pending events (blocking)
void myp_event_process_all(void);

// Process one event (non-blocking, returns 0 if no events)
int myp_event_process_one(void);

// ---- Thread Support (@thread) ----
// Opaque thread handle
typedef struct myp_thread myp_thread_t;

// Create a new thread with its own event queue.
// If startup is non-NULL, the thread calls startup(startup_arg, NULL) before its event loop.
myp_thread_t* myp_thread_create(void (*startup)(void*, void*), void* startup_arg);

// Post an event to a specific thread's queue (thread-safe)
void myp_thread_post_event(myp_thread_t* thr, int event_id, void* sender, void* data);

// Associate an instance pointer with a thread (for async cross-thread event delivery)
void myp_thread_associate_instance(void* instance, myp_thread_t* thr);

// Start the thread's event loop (non-blocking, creates pthread)
void myp_thread_run_loop(myp_thread_t* thr);

// Stop the thread's event loop
void myp_thread_stop(myp_thread_t* thr);

// Destroy a thread (join + cleanup)
void myp_thread_destroy(myp_thread_t* thr);

// ---- Synchronization primitives (§五-2): handle-based pthread wrappers ----
int32_t myp_mutex_create(void);
int32_t myp_mutex_create_recursive(void);
void myp_mutex_lock(int32_t h);
int32_t myp_mutex_trylock(int32_t h);
void myp_mutex_unlock(int32_t h);
void myp_mutex_destroy(int32_t h);
int32_t myp_rwlock_create(void);
void myp_rwlock_rdlock(int32_t h);
void myp_rwlock_wrlock(int32_t h);
int32_t myp_rwlock_tryrdlock(int32_t h);
int32_t myp_rwlock_trywrlock(int32_t h);
void myp_rwlock_unlock(int32_t h);
void myp_rwlock_destroy(int32_t h);
int32_t myp_cond_create(void);
void myp_cond_wait(int32_t ch, int32_t mh);
void myp_cond_signal(int32_t ch);
void myp_cond_broadcast(int32_t ch);
void myp_cond_destroy(int32_t ch);
int32_t myp_sem_create(int32_t initial);
void myp_sem_wait(int32_t h);
int32_t myp_sem_trywait(int32_t h);
void myp_sem_post(int32_t h);
void myp_sem_destroy(int32_t h);
int32_t myp_once_create(void);
int32_t myp_once_enter(int32_t h);
void myp_once_done(int32_t h);
void myp_once_destroy(int32_t h);

// ---- Work-Stealing Thread Pool (v6) ----
typedef struct myp_pool myp_pool_t;
myp_pool_t* myp_pool_create(int n_threads);
void myp_pool_parallel_for(myp_pool_t* pool, int start, int end, int step,
                            void (*work_fn)(int, int, int, void*), void* arg);
void myp_pool_destroy(myp_pool_t* pool);
int32_t myp_pool_thread_count(void);extern myp_pool_t* myp_global_pool;
myp_pool_t* myp_pool_ensure_global(void);
// ---- Math ----
double myp_math_sqrt(double v);
double myp_math_abs(double v);
double myp_math_floor(double v);
double myp_math_ceil(double v);
double myp_math_sin(double v);
double myp_math_cos(double v);
double myp_math_tan(double v);
double myp_math_asin(double v);
double myp_math_acos(double v);
double myp_math_atan(double v);
double myp_math_atan2(double y, double x);
double myp_math_sinh(double v);
double myp_math_cosh(double v);
double myp_math_tanh(double v);
double myp_math_exp(double v);
double myp_math_log(double v);
double myp_math_pow(double base, double exp);
int32_t myp_math_abs_int(int32_t v);

// ---- File I/O ----
int32_t myp_io_fopen(const char* path, const char* mode);
void myp_io_fclose(void);
const char* myp_io_read_line(void);
void myp_io_write(const char* text);
void myp_io_write_line(const char* text);
int32_t myp_io_has_next(void);
int32_t myp_io_read_byte(void);
int32_t myp_io_read_i32be(void);
int32_t myp_io_seek(int32_t offset, int32_t whence);
int32_t myp_io_write_byte(int32_t c);
int32_t myp_io_write_i32be(int32_t val);
int32_t myp_io_write_double(double val);
double myp_io_read_double(void);

// ---- Error handling ----
// setjmp/longjmp are generated directly in LLVM IR (not through C wrappers)
// These are helper functions called from generated try/catch code
void myp_error_setup(void);
void myp_throw(const char* msg);
const char* myp_get_error(void);
int myp_error_is_active(void);
void myp_error_clear(void);
// IR-level optimization barrier: codegen passes try-inner ARC slot addresses so
// LLVM keeps them as escaped memory across setjmp/longjmp (see runtime.c).
void myp_try_escape(void* p);
// Release the object held by a local ARC slot, reading the slot's physical
// memory here in C (opaque to LLVM — the -O pipeline cannot fold it to undef on
// the longjmp path). kind: 0=class ptr, 1/2=interface/function fat pointer.
void myp_release_slot(void* slot_addr, int kind);

// ---- Test framework ----
void myp_assert(int cond);
void myp_assert_msg(int cond, const char* msg);
void myp_test_set_msg(const char* msg);
void myp_assert_eq(int a, int b);
void myp_assert_str_eq(const char* a, const char* b);
void myp_test_report(const char* name, int passed);
void myp_test_fail_msg(const char* msg);
int myp_test_summary(int test_count);
void myp_assert_long_neq(int64_t a, int64_t b);
void myp_assert_float_neq(double a, double b, double eps);
void myp_assert_null(const void* p);
void myp_assert_not_null(const void* p);

#ifdef __cplusplus
}
#endif

#endif // MYLANG_RUNTIME_H
