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

// ---- Work-Stealing Thread Pool (v6) ----
typedef struct myp_pool myp_pool_t;
myp_pool_t* myp_pool_create(int n_threads);
void myp_pool_parallel_for(myp_pool_t* pool, int start, int end, int step,
                            void (*work_fn)(int, void*), void* arg);
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

// ---- Test framework ----
void myp_assert(int cond);
void myp_assert_eq(int a, int b);
void myp_assert_str_eq(const char* a, const char* b);
void myp_test_report(const char* name, int passed);

#ifdef __cplusplus
}
#endif

#endif // MYLANG_RUNTIME_H
