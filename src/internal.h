/* Internal layout shared across simpyc TUs. Not installed. */
#ifndef SIMPYC_INTERNAL_H
#define SIMPYC_INTERNAL_H

#include "simpyc.h"
#include "coro.h"

#include <stddef.h>
#include <stdint.h>

/* --- Callbacks ----------------------------------------------------- */

/* (fn, user) tuple stored contiguously in a dynamic array per event.
 * Replaces the older per-callback linked-list node — avoids one malloc
 * per subscription. */
typedef struct cb_pair {
    sim_callback_fn fn;
    void           *user;
} cb_pair_t;

/* --- Events -------------------------------------------------------- */

enum {
    SIM_EV_PENDING   = 0,  /* not yet triggered */
    SIM_EV_TRIGGERED = 1,  /* triggered, in queue */
    SIM_EV_PROCESSED = 2,  /* callbacks fired */
};

struct sim_event {
    sim_env_t *env;
    cb_pair_t *cbs;           /* malloc'd; grows geometrically */
    uint32_t   cb_cap;
    uint32_t   cb_len;
    void      *value;
    const char *fail_reason;  /* NULL on success */
    uint8_t    state;
    uint8_t    ok;            /* 1 succeeded, 0 failed */
    uint8_t    recyclable;    /* return to env pool after processing */
    uint8_t    pad[5];
    struct sim_event *free_next;  /* per-env pool free list */
    struct sim_event *all_next;   /* per-env "all allocated" list */
};

/* --- Heap entry ---------------------------------------------------- */

typedef struct heap_entry {
    double    time;
    int       priority;
    uint64_t  seq;
    sim_event_t *event;
} heap_entry_t;

typedef struct event_heap {
    heap_entry_t *data;
    size_t        len;
    size_t        cap;
} event_heap_t;

void heap_init(event_heap_t *h);
void heap_free(event_heap_t *h);
void heap_push(event_heap_t *h, const heap_entry_t *e);
int  heap_pop (event_heap_t *h, heap_entry_t *out);
int  heap_peek(const event_heap_t *h, heap_entry_t *out);

/* --- Processes ----------------------------------------------------- */

enum {
    SIM_PROC_NEW      = 0,
    SIM_PROC_RUNNING  = 1,
    SIM_PROC_DEAD     = 2,
};

struct sim_process {
    sim_env_t   *env;
    sim_proc_fn  fn;
    void        *arg;
    coro_ctx_t   ctx;
    void        *stack_base;     /* malloc'd; freed on death */
    size_t       stack_size;
    sim_event_t *done;           /* fired when process exits */
    sim_event_t *yielded;        /* event we're currently waiting on; NULL when not waiting */
    sim_event_t *resume_ev;      /* reused per-process resume event */
    void        *resume_value;   /* set just before resume */
    void        *interrupt_cause;
    uint8_t      state;
    uint8_t      interrupt_pending;
    uint8_t      was_interrupted;  /* set to 1 just before resume from interrupt */
    uint8_t      pad[5];
    struct sim_process *free_next;
    struct sim_process *all_next; /* list of all live processes for cleanup */
};

/* --- Environment --------------------------------------------------- */

struct sim_env {
    double         now;
    uint64_t       next_seq;
    event_heap_t   heap;

    /* the scheduler's host context (where sim_run runs). Processes
     * switch back to this when they yield. */
    coro_ctx_t     host_ctx;
    sim_process_t *active;         /* currently running process, or NULL */

    /* pools */
    sim_event_t   *event_pool;
    sim_process_t *process_pool;

    /* tracking for cleanup */
    sim_process_t *all_processes;
    sim_event_t   *all_events;

    /* realtime mode (factor > 0 enables; wall seconds per sim unit) */
    double         rt_factor;
    int            rt_strict;
    int            rt_lagged;
    double         rt_start_wall;
    double         rt_start_sim;
};

/* --- Internal event helpers --------------------------------------- */

sim_event_t *_sim_event_alloc(sim_env_t *env);
void         _sim_event_schedule(sim_event_t *e, double delay, int priority);
void         _sim_event_run_callbacks(sim_event_t *e);

/* --- Internal process helpers ------------------------------------- */

void         _sim_process_resume(sim_process_t *p);

#endif /* SIMPYC_INTERNAL_H */
