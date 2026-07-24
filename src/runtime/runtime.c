#include "mylang/runtime.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>

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

// ======================
// Basic I/O
// ======================

void myp_print(const char* str) { printf("%s", str); }
void myp_println(const char* str) { printf("%s\n", str); }
void myp_print_int(int32_t val) { printf("%d\n", val); }
void myp_print_long(int64_t val) { printf("%ld\n", val); }
void myp_print_float(double val) { printf("%g", val); }
void myp_print_bool(int32_t val) { printf(val ? "true" : "false"); }

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
            myp_event_fire(eid, inst, &pval);

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
