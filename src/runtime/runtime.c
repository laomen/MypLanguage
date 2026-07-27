#include "mylang/runtime.h"

#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>

// ======================
// Terminal raw mode (for real-time keyboard input)
// ======================

static struct termios myp_orig_term;
static int myp_raw_mode = 0;

static void myp_enable_raw(void) {
    if (!myp_raw_mode) {
        struct termios raw;
        if (tcgetattr(0, &myp_orig_term) != 0) {
            // Not a terminal (e.g. pipe) — skip raw mode
            myp_raw_mode = 0;
            return;
        }
        raw = myp_orig_term;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
        myp_raw_mode = 1;
    }
}

static void myp_restore_term(void) {
    if (myp_raw_mode) {
        tcsetattr(0, TCSANOW, &myp_orig_term);
        myp_raw_mode = 0;
    }
}

// Check if a key is available (non-blocking)
int32_t myp_kbhit(void) {
    myp_enable_raw();
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv) > 0 ? 1 : 0;
}

// Read one character (non-blocking, raw mode). Returns 0 if no key available.
int32_t myp_getch(void) {
    myp_enable_raw();
    char c = 0;
    if (read(0, &c, 1) > 0) return (int32_t)(unsigned char)c;
    return 0;
}

// ======================
// File I/O
// ======================

static FILE* myp_io_fp = NULL;

int32_t myp_io_fopen(const char* path, const char* mode) {
    FILE* fp = fopen(path, mode);
    if (!fp) return -1;
    myp_io_fp = fp;
    return 0;
}

void myp_io_fclose(void) {
    if (myp_io_fp) fclose(myp_io_fp);
    myp_io_fp = NULL;
}

const char* myp_io_read_line(void) {
    if (!myp_io_fp) return NULL;
    static char buf[4096];
    if (!fgets(buf, sizeof(buf), myp_io_fp)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return buf;
}

void myp_io_write(const char* text) {
    if (!myp_io_fp) return;
    fputs(text, myp_io_fp);
}

void myp_io_write_line(const char* text) {
    if (!myp_io_fp) return;
    fprintf(myp_io_fp, "%s\n", text);
}

int32_t myp_io_has_next(void) {
    if (!myp_io_fp) return 0;
    return !feof(myp_io_fp);
}

// Binary file I/O for IDX parsing
#include <stdint.h>
int32_t myp_io_read_byte(void) {
    if (!myp_io_fp) return -1;
    unsigned char c;
    if (fread(&c, 1, 1, myp_io_fp) != 1) return -1;
    return (int32_t)c;
}

int32_t myp_io_read_i32be(void) {
    if (!myp_io_fp) return -1;
    unsigned char buf[4];
    if (fread(buf, 1, 4, myp_io_fp) != 4) return -1;
    return ((int32_t)buf[0] << 24) | ((int32_t)buf[1] << 16) |
           ((int32_t)buf[2] << 8) | (int32_t)buf[3];
}

int32_t myp_io_seek(int32_t offset, int32_t whence) {
    if (!myp_io_fp) return -1;
    return fseek(myp_io_fp, offset, whence) == 0 ? 0 : -1;
}

// Binary file I/O for weight save/load
int32_t myp_io_write_byte(int32_t c) {
    if (!myp_io_fp) return -1;
    unsigned char byte = (unsigned char)(c & 0xFF);
    if (fwrite(&byte, 1, 1, myp_io_fp) != 1) return -1;
    return 0;
}

int32_t myp_io_write_i32be(int32_t val) {
    if (!myp_io_fp) return -1;
    unsigned char buf[4];
    buf[0] = (unsigned char)((val >> 24) & 0xFF);
    buf[1] = (unsigned char)((val >> 16) & 0xFF);
    buf[2] = (unsigned char)((val >> 8) & 0xFF);
    buf[3] = (unsigned char)(val & 0xFF);
    if (fwrite(buf, 1, 4, myp_io_fp) != 4) return -1;
    return 0;
}

int32_t myp_io_write_double(double val) {
    if (!myp_io_fp) return -1;
    if (fwrite(&val, sizeof(double), 1, myp_io_fp) != 1) return -1;
    return 0;
}

double myp_io_read_double(void) {
    if (!myp_io_fp) return 0.0;
    double val;
    if (fread(&val, sizeof(double), 1, myp_io_fp) != 1) return 0.0;
    return val;
}

// ======================
// Basic I/O
// ======================

void myp_print(const char* str) { printf("%s", str); }
void myp_println(const char* str) { printf("%s\n", str); fflush(stdout); }
void myp_print_int(int32_t val) { printf("%d\n", val); fflush(stdout); }
void myp_print_long(int64_t val) { printf("%ld\n", val); fflush(stdout); }
void myp_print_float(double val) { printf("%g", val); fflush(stdout); }
void myp_print_bool(int32_t val) { printf(val ? "true" : "false"); fflush(stdout); }

void myp_flush(void) { fflush(stdout); }

// Terminal size (for TUI rendering)
int32_t myp_term_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return (int32_t)w.ws_col;
    return 80; // fallback
}
int32_t myp_term_height(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return (int32_t)w.ws_row;
    return 24; // fallback
}

// Read a line from stdin (for interactive input)
const char* myp_read_line(void) {
    static char buf[256];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return buf;
}

// String length
int32_t myp_strlen(const char* s) {
    if (!s) return 0;
    return (int32_t)strlen(s);
}

// Convert a character code to a single-character string
const char* myp_chr(int32_t code) {
    static char buf[2];
    buf[0] = (char)(code & 0xFF);
    buf[1] = '\0';
    return buf;
}

// String equality comparison (content, not pointer)
int32_t myp_str_eq(const char* a, const char* b) {
    if (!a || !b) return a == b ? 1 : 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

// String to double (for parsing data files)
double myp_atof(const char* s) {
    if (!s) return 0.0;
    return atof(s);
}

// ======================
// Memory — Thread-Local Allocation Tracking
// ======================

typedef struct myp_alloc_node {
    void* ptr;
    struct myp_alloc_node* next;
} myp_alloc_node_t;

static pthread_key_t myp_alloc_key;
static pthread_once_t myp_alloc_key_once = PTHREAD_ONCE_INIT;

static void myp_free_alloc_list(void* ptr) {
    myp_alloc_node_t* node = (myp_alloc_node_t*)ptr;
    while (node) {
        if (node->ptr) free(node->ptr);
        myp_alloc_node_t* next = node->next;
        free(node);
        node = next;
    }
}

static void myp_make_alloc_key(void) {
    pthread_key_create(&myp_alloc_key, myp_free_alloc_list);
}

static myp_alloc_node_t* myp_alloc_list_head(void) {
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    return (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
}

static void myp_alloc_list_push(void* p) {
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    myp_alloc_node_t* node = (myp_alloc_node_t*)malloc(sizeof(myp_alloc_node_t));
    if (!node) return;
    node->ptr = p;
    node->next = (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
    pthread_setspecific(myp_alloc_key, node);
}

void* myp_alloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) myp_alloc_list_push(ptr);
    return ptr;
}

void myp_free(void* ptr) {
    // Individual free is optional — myp_free_all() at exit handles bulk cleanup.
    // This is provided for cases where deterministic early release is needed.
    free(ptr);
}

void myp_free_all(void) {
    // Restore terminal if we changed it to raw mode
    myp_restore_term();
    // Trigger the pthread key destructor which calls myp_free_alloc_list
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    myp_alloc_node_t* head = (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
    if (head) {
        myp_free_alloc_list(head);
        pthread_setspecific(myp_alloc_key, NULL);
    }
}

// ======================
// Timeline
// ======================

int64_t myp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void myp_sleep_ms(int64_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// ======================
// String concatenation
// ======================

#include <string.h>

char* myp_strcat(const char* a, const char* b) {
    if (!a && !b) return NULL;
    if (!a) return strdup(b);
    if (!b) return strdup(a);
    size_t la = strlen(a), lb = strlen(b);
    char* result = (char*)myp_alloc(la + lb + 1);
    if (result) {
        memcpy(result, a, la);
        memcpy(result + la, b, lb);
        result[la + lb] = '\0';
    }
    return result;
}

// Convert a value to string (for string concatenation with non-strings)
char* myp_to_string_i32(int32_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_i64(int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_double(double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_bool(int32_t val) {
    return myp_strcat(val ? "true" : "false", "");
}

// ======================
// Timer System
// ======================

#define MYP_MAX_TIMERS 64

typedef struct {
    int event_id;
    void* instance;
    int64_t fire_time;   // absolute time (ms) when timer should fire
    int64_t param;       // parameter to pass to the event
    int64_t interval;    // repeat interval (ms); 0 = one-shot
    int active;
} myp_timer_entry_t;

static myp_timer_entry_t myp_timers[MYP_MAX_TIMERS];
static int myp_timer_count = 0;
static pthread_mutex_t myp_timer_mutex = PTHREAD_MUTEX_INITIALIZER;

// Create a timer that fires event_id on instance after delay_ms.
// If interval_ms > 0, the timer repeats every interval_ms.
// param is passed as event data when the timer fires.
// Returns 0 on success, -1 on failure (no timer slots available).
int32_t myp_timer_create(int event_id, void* instance, int64_t delay_ms,
                         int64_t param, int64_t interval_ms) {
    pthread_mutex_lock(&myp_timer_mutex);
    if (myp_timer_count >= MYP_MAX_TIMERS) {
        pthread_mutex_unlock(&myp_timer_mutex);
        return -1;
    }
    int64_t now = myp_now_ms();
    myp_timers[myp_timer_count].event_id = event_id;
    myp_timers[myp_timer_count].instance = instance;
    myp_timers[myp_timer_count].fire_time = now + delay_ms;
    myp_timers[myp_timer_count].param = param;
    myp_timers[myp_timer_count].interval = interval_ms;
    myp_timers[myp_timer_count].active = 1;
    myp_timer_count++;
    pthread_mutex_unlock(&myp_timer_mutex);
    return 0;
}

// Cancel all timers for a given instance (e.g., when instance is destroyed)
void myp_timer_cancel_all(void* instance) {
    pthread_mutex_lock(&myp_timer_mutex);
    for (int i = 0; i < myp_timer_count; i++) {
        if (myp_timers[i].active && myp_timers[i].instance == instance)
            myp_timers[i].active = 0;
    }
    pthread_mutex_unlock(&myp_timer_mutex);
}

// Check and fire expired timers. Called from the event loop.
// Returns the number of timers fired.
int myp_timer_check(void) {
    int fired = 0;
    int64_t now = myp_now_ms();
    pthread_mutex_lock(&myp_timer_mutex);
    for (int i = 0; i < myp_timer_count; i++) {
        if (!myp_timers[i].active) continue;
        if (now >= myp_timers[i].fire_time) {
            // Fire the event on the timer's instance
            int eid = myp_timers[i].event_id;
            void* inst = myp_timers[i].instance;
            int64_t pval = myp_timers[i].param;

            if (myp_timers[i].interval > 0) {
                // Repeating timer: reschedule
                myp_timers[i].fire_time = now + myp_timers[i].interval;
            } else {
                // One-shot: deactivate
                myp_timers[i].active = 0;
            }
            pthread_mutex_unlock(&myp_timer_mutex);

            // Fire the event (this may re-enter timer code, so unlock first)
            // Since param data (pval) is stack-local, process it now
            myp_event_fire(eid, inst, &pval);
            // Process this event immediately so pval is still valid
            myp_event_process_one();

            pthread_mutex_lock(&myp_timer_mutex);
            fired++;
        }
    }
    pthread_mutex_unlock(&myp_timer_mutex);
    return fired;
}

// ======================
// Event System — Per-Thread Queues
// ======================

#define MYP_EVENT_QUEUE_SIZE 1024

typedef struct {
    int event_id;
    void* sender;
    void* data;
    int has_data;
} myp_event_t;

// Event queue (ring buffer)
typedef struct {
    myp_event_t events[MYP_EVENT_QUEUE_SIZE];
    volatile int head;
    volatile int tail;
    pthread_mutex_t mutex;
} myp_event_queue_t;

// Event handler registration (global)
typedef struct {
    int event_id;
    int registered;
    void* instance;
    myp_handler_fn handler;
} myp_handler_entry_t;

static myp_handler_entry_t myp_handlers[MYP_MAX_HANDLERS];
static int myp_handler_count = 0;

// Thread-local queue key (pthread_getspecific / pthread_setspecific)
static pthread_key_t myp_queue_key;
static pthread_once_t myp_queue_key_once = PTHREAD_ONCE_INIT;

static void myp_free_queue(void* ptr) {
    if (ptr) {
        myp_event_queue_t* q = (myp_event_queue_t*)ptr;
        pthread_mutex_destroy(&q->mutex);
        free(q);
    }
}

static void myp_make_queue_key(void) {
    pthread_key_create(&myp_queue_key, myp_free_queue);
}

// Create a new event queue
static myp_event_queue_t* myp_queue_create(void) {
    myp_event_queue_t* q = (myp_event_queue_t*)calloc(1, sizeof(myp_event_queue_t));
    pthread_mutex_init(&q->mutex, NULL);
    return q;
}

// Set the current thread's queue
static void myp_queue_set_current(myp_event_queue_t* q) {
    pthread_once(&myp_queue_key_once, myp_make_queue_key);
    pthread_setspecific(myp_queue_key, q);
}

// Get the current thread's queue
static myp_event_queue_t* myp_queue_current(void) {
    pthread_once(&myp_queue_key_once, myp_make_queue_key);
    myp_event_queue_t* q = (myp_event_queue_t*)pthread_getspecific(myp_queue_key);
    if (!q) {
        // Main thread: create queue lazily
        q = myp_queue_create();
        myp_queue_set_current(q);
    }
    return q;
}

// Push an event to a specific queue (thread-safe)
static void myp_queue_push(myp_event_queue_t* q, int event_id, void* sender, void* data) {
    pthread_mutex_lock(&q->mutex);
    int next = (q->head + 1) % MYP_EVENT_QUEUE_SIZE;
    if (next != q->tail) {
        q->events[q->head].event_id = event_id;
        q->events[q->head].sender = sender;
        q->events[q->head].data = data;
        q->events[q->head].has_data = (data != NULL);
        q->head = next;
    }
    pthread_mutex_unlock(&q->mutex);
}

// Pop an event from a specific queue (thread-safe, returns 0 if empty)
static int myp_queue_pop(myp_event_queue_t* q, myp_event_t* ev) {
    pthread_mutex_lock(&q->mutex);
    if (q->tail == q->head) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    *ev = q->events[q->tail];
    q->tail = (q->tail + 1) % MYP_EVENT_QUEUE_SIZE;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

// ---- Thread Support (@thread) ----
struct myp_thread {
    pthread_t thread;
    volatile int running;
    myp_event_queue_t* queue;
    void (*startup_fn)(void*, void*);
    void* startup_arg;
};

// ---- Instance→Thread mapping (for async cross-thread event delivery) ----
static pthread_mutex_t myp_inst_map_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MYP_INST_MAP_SIZE 128

typedef struct {
    void* instance;
    myp_thread_t* thread;
} myp_inst_map_entry_t;

static myp_inst_map_entry_t myp_inst_map[MYP_INST_MAP_SIZE];
static int myp_inst_map_count = 0;

void myp_thread_associate_instance(void* instance, myp_thread_t* thr) {
    pthread_mutex_lock(&myp_inst_map_mutex);
    if (myp_inst_map_count < MYP_INST_MAP_SIZE) {
        myp_inst_map[myp_inst_map_count].instance = instance;
        myp_inst_map[myp_inst_map_count].thread = thr;
        myp_inst_map_count++;
    }
    pthread_mutex_unlock(&myp_inst_map_mutex);
}

static myp_thread_t* myp_thread_for_instance(void* instance) {
    for (int i = 0; i < myp_inst_map_count; i++) {
        if (myp_inst_map[i].instance == instance)
            return myp_inst_map[i].thread;
    }
    return NULL;
}

// ---- Public API ----

void myp_event_register(int event_id, void* instance, myp_handler_fn handler) {
    if (myp_handler_count >= MYP_MAX_HANDLERS) return;
    myp_handlers[myp_handler_count].event_id = event_id;
    myp_handlers[myp_handler_count].instance = instance;
    myp_handlers[myp_handler_count].handler = handler;
    myp_handlers[myp_handler_count].registered = 1;
    myp_handler_count++;
}

// Scope-based handler management for mapping() @scope
#define MYP_SCOPE_STACK_DEPTH 64
static int myp_scope_stack[MYP_SCOPE_STACK_DEPTH];
static int myp_scope_depth = 0;

void myp_event_push_scope(void) {
    if (myp_scope_depth >= MYP_SCOPE_STACK_DEPTH) return;
    myp_scope_stack[myp_scope_depth++] = myp_handler_count;
}

void myp_event_pop_scope(void) {
    if (myp_scope_depth <= 0) return;
    int saved = myp_scope_stack[--myp_scope_depth];
    myp_handler_count = saved;
}

void myp_event_fire(int event_id, void* sender, void* event_data) {
#ifdef TRACE_ENABLED
    fprintf(stderr, "[TRACE] event_fire(id=%d, sender=%p)\n", event_id, sender);
#endif
    // Check if sender belongs to another thread → async cross-thread delivery
    myp_thread_t* target_thr = sender ? myp_thread_for_instance(sender) : NULL;
    if (target_thr) {
        // Post to the target thread's queue (async — no process_all here)
        myp_queue_push(target_thr->queue, event_id, sender, event_data);
        return;
    }
    // Same-thread (or no thread): post to current thread's queue
    myp_event_queue_t* q = myp_queue_current();
    myp_queue_push(q, event_id, sender, event_data);
}

static void myp_event_dispatch(myp_event_t* ev) {
#ifdef TRACE_ENABLED
    fprintf(stderr, "[TRACE] dispatch(event_id=%d, sender=%p)\n", ev->event_id, ev->sender);
#endif
    for (int i = 0; i < myp_handler_count; i++) {
        if (myp_handlers[i].registered &&
            myp_handlers[i].event_id == ev->event_id) {
            myp_handlers[i].handler(myp_handlers[i].instance, ev->data);
        }
    }
}

void myp_event_process_all(void) {
    myp_event_queue_t* q = myp_queue_current();
    myp_event_t ev;
    while (myp_queue_pop(q, &ev)) {
        myp_event_dispatch(&ev);
    }
    // Also check expired timers
    myp_timer_check();
}

int myp_event_process_one(void) {
    myp_event_queue_t* q = myp_queue_current();
    myp_event_t ev;
    if (!myp_queue_pop(q, &ev)) return 0;
    myp_event_dispatch(&ev);
    return 1;
}

// ---- Thread Support (@thread) ----

myp_thread_t* myp_thread_create(void (*startup)(void*, void*), void* startup_arg) {
    myp_thread_t* thr = (myp_thread_t*)calloc(1, sizeof(myp_thread_t));
    thr->running = 1;
    thr->queue = myp_queue_create();
    thr->startup_fn = startup;
    thr->startup_arg = startup_arg;
    return thr;
}

// Post an event to a specific thread's queue (thread-safe)
void myp_thread_post_event(myp_thread_t* thr, int event_id, void* sender, void* data) {
    myp_queue_push(thr->queue, event_id, sender, data);
}

static void* myp_thread_entry(void* arg) {
    myp_thread_t* thr = (myp_thread_t*)arg;
    // Set this thread's queue
    myp_queue_set_current(thr->queue);

    // Run startup function (e.g. @startup action) on this thread
    if (thr->startup_fn) {
        thr->startup_fn(thr->startup_arg, NULL);
    }

    // Event loop — process this thread's own queue + timers
    while (thr->running) {
        myp_event_process_all();
        myp_timer_check();  // fire expired timers
        // Brief sleep to avoid busy-waiting
        struct timespec ts = {0, 1000000}; // 1ms
        nanosleep(&ts, NULL);
    }
    // Clear TLS so destructor doesn't double-free
    pthread_setspecific(myp_queue_key, NULL);
    return NULL;
}

void myp_thread_run_loop(myp_thread_t* thr) {
    pthread_create(&thr->thread, NULL, myp_thread_entry, thr);
}

void myp_thread_stop(myp_thread_t* thr) {
    thr->running = 0;
}

void myp_thread_destroy(myp_thread_t* thr) {
    myp_thread_stop(thr);
    pthread_join(thr->thread, NULL);
    free(thr->queue);
    // Note: thr->startup_arg (the instance) is tracked by myp_alloc and
    // will be freed by myp_free_all() at main exit. Do NOT free it here.
    free(thr);
}

// ======================
// Math functions
// ======================

double myp_math_sqrt(double v)    { return sqrt(v); }
double myp_math_abs(double v)     { return fabs(v); }
double myp_math_floor(double v)   { return floor(v); }
double myp_math_ceil(double v)    { return ceil(v); }
double myp_math_sin(double v)     { return sin(v); }
double myp_math_cos(double v)     { return cos(v); }
double myp_math_tan(double v)     { return tan(v); }
double myp_math_exp(double v)     { return exp(v); }
double myp_math_log(double v)     { return log(v); }
double myp_math_pow(double b, double e) { return pow(b, e); }
int32_t myp_math_abs_int(int32_t v) { return v < 0 ? -v : v; }

// ======================
// Error handling (setjmp/longjmp)
// ======================

// setjmp/longjmp are called directly from LLVM IR.
// myp_setjmp is a wrapper that forwards to system setjmp,
// but setjmp must be called in the same function that branches
// on its return value, so we generate @llvm.setjmp or direct
// calls in codegen instead.
static char myp_error_msg[256];
static int myp_error_active = 0;

// Called from generated code via intrinsic: saves error context
void myp_error_setup(void) {
    myp_error_active = 1;
}

// Called from generated code via intrinsic: records error and triggers longjmp
void myp_throw(const char* msg) {
    strncpy(myp_error_msg, msg, 255);
    myp_error_msg[255] = '\0';
}

const char* myp_get_error(void) {
    return myp_error_msg;
}

int myp_error_is_active(void) {
    return myp_error_active;
}

void myp_error_clear(void) {
    myp_error_active = 0;
}

// ======================
// Test framework
// ======================
#include <stdio.h>

static int myp_test_pass_count = 0;
static int myp_test_fail_count = 0;

void myp_assert(int cond) {
    if (!cond) {
        fprintf(stderr, "  ASSERTION FAILED\n");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_eq(int a, int b) {
    if (a != b) {
        fprintf(stderr, "  ASSERTION FAILED: %d != %d\n", a, b);
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_str_eq(const char* a, const char* b) {
    int eq = (a == b) || (a && b && strcmp(a, b) == 0);
    if (!eq) {
        fprintf(stderr, "  ASSERTION FAILED: \"%s\" != \"%s\"\n", a ? a : "null", b ? b : "null");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_test_report(const char* name, int passed) {
    if (passed) {
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
    }
}

// ======================
// Barrier 同步 (pthread_barrier 封装)
// ======================

#define MYP_MAX_BARRIERS 64
static pthread_barrier_t myp_barriers[MYP_MAX_BARRIERS];
static int myp_barrier_used[MYP_MAX_BARRIERS] = {0};
static pthread_mutex_t myp_barrier_mutex = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_barrier_create(int32_t count) {
    pthread_mutex_lock(&myp_barrier_mutex);
    for (int32_t i = 0; i < MYP_MAX_BARRIERS; i++) {
        if (!myp_barrier_used[i]) {
            if (pthread_barrier_init(&myp_barriers[i], NULL, (unsigned int)count) != 0) {
                pthread_mutex_unlock(&myp_barrier_mutex);
                return -1;
            }
            myp_barrier_used[i] = 1;
            pthread_mutex_unlock(&myp_barrier_mutex);
            return i; // return handle
        }
    }
    pthread_mutex_unlock(&myp_barrier_mutex);
    return -1; // no free slot
}

int32_t myp_barrier_wait(int32_t handle) {
    if (handle < 0 || handle >= MYP_MAX_BARRIERS || !myp_barrier_used[handle])
        return -1;
    int32_t ret = pthread_barrier_wait(&myp_barriers[handle]);
    return (ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD) ? 0 : -1;
}

void myp_barrier_destroy(int32_t handle) {
    if (handle >= 0 && handle < MYP_MAX_BARRIERS && myp_barrier_used[handle]) {
        pthread_barrier_destroy(&myp_barriers[handle]);
        myp_barrier_used[handle] = 0;
    }
}

// ======================
// Future/Promise (条件变量封装)
// ======================

#define MYP_MAX_FUTURES 64
typedef struct {
    int32_t value;
    int32_t ready;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int32_t used;
} myp_future_t;

static myp_future_t myp_futures[MYP_MAX_FUTURES];
static pthread_mutex_t myp_future_mutex = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_future_create(void) {
    pthread_mutex_lock(&myp_future_mutex);
    for (int32_t i = 0; i < MYP_MAX_FUTURES; i++) {
        if (!myp_futures[i].used) {
            myp_futures[i].used = 1;
            myp_futures[i].ready = 0;
            myp_futures[i].value = 0;
            pthread_mutex_init(&myp_futures[i].mutex, NULL);
            pthread_cond_init(&myp_futures[i].cond, NULL);
            pthread_mutex_unlock(&myp_future_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_future_mutex);
    return -1;
}

void myp_future_set(int32_t handle, int32_t value) {
    if (handle < 0 || handle >= MYP_MAX_FUTURES || !myp_futures[handle].used) return;
    pthread_mutex_lock(&myp_futures[handle].mutex);
    myp_futures[handle].value = value;
    myp_futures[handle].ready = 1;
    pthread_cond_broadcast(&myp_futures[handle].cond);
    pthread_mutex_unlock(&myp_futures[handle].mutex);
}

int32_t myp_future_get(int32_t handle) {
    if (handle < 0 || handle >= MYP_MAX_FUTURES || !myp_futures[handle].used) return 0;
    pthread_mutex_lock(&myp_futures[handle].mutex);
    while (!myp_futures[handle].ready) {
        pthread_cond_wait(&myp_futures[handle].cond, &myp_futures[handle].mutex);
    }
    int32_t val = myp_futures[handle].value;
    pthread_mutex_unlock(&myp_futures[handle].mutex);
    return val;
}

void myp_future_destroy(int32_t handle) {
    if (handle >= 0 && handle < MYP_MAX_FUTURES && myp_futures[handle].used) {
        pthread_mutex_destroy(&myp_futures[handle].mutex);
        pthread_cond_destroy(&myp_futures[handle].cond);
        myp_futures[handle].used = 0;
    }
}
