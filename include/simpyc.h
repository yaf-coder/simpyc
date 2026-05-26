/*
 * simpyc - a port of SimPy's discrete-event simulation core to C.
 *
 * Public API. All types are opaque; the implementation owns lifecycle.
 * Time is a double (matches SimPy semantics).
 */
#ifndef SIMPYC_H
#define SIMPYC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_env                  sim_env_t;
typedef struct sim_event                sim_event_t;
typedef struct sim_process              sim_process_t;
typedef struct sim_resource             sim_resource_t;
typedef struct sim_priority_resource    sim_priority_resource_t;
typedef struct sim_preemptive_resource  sim_preemptive_resource_t;
typedef struct sim_container            sim_container_t;
typedef struct sim_store                sim_store_t;
typedef struct sim_filter_store         sim_filter_store_t;
typedef struct sim_priority_store       sim_priority_store_t;

/* Heap priority levels. Lower value = earlier dispatch at same time. */
#define SIM_PRIO_URGENT  0
#define SIM_PRIO_NORMAL  1

/* Generic callback fired when an event is processed.
 * The event is passed; user is the cookie supplied at sim_event_on. */
typedef void (*sim_callback_fn)(sim_event_t *event, void *user);

/* Process body. Receives the process handle (for yielding) and the
 * user argument passed at sim_process(). Return to terminate. */
typedef void (*sim_proc_fn)(sim_process_t *self, void *arg);

/* --- Environment ----------------------------------------------------- */

sim_env_t *sim_env_create(void);
void       sim_env_destroy(sim_env_t *env);
double     sim_now(const sim_env_t *env);

/* Run until the queue is empty. Returns number of events processed. */
size_t     sim_run(sim_env_t *env);

/* Run until simulation time reaches `until` (inclusive of events at
 * times <= until). Returns number of events processed. */
size_t     sim_run_until(sim_env_t *env, double until);

/* Process the next single scheduled event. Returns 0 on success,
 * -1 if the queue is empty. */
int        sim_step(sim_env_t *env);

/* Peek the time of the next scheduled event without advancing.
 * Returns 0 on success and writes to *out_time. Returns -1 if empty. */
int        sim_peek(const sim_env_t *env, double *out_time);

/* --- Events ---------------------------------------------------------- */

/* Bare event; manually trigger via sim_event_succeed/fail. */
sim_event_t *sim_event(sim_env_t *env);

/* Auto-triggered after `delay` time units. value defaults to NULL. */
sim_event_t *sim_timeout(sim_env_t *env, double delay);
sim_event_t *sim_timeout_v(sim_env_t *env, double delay, void *value);

/* Trigger an event with success/failure. value is opaque user data. */
void  sim_event_succeed(sim_event_t *e, void *value);
void  sim_event_fail(sim_event_t *e, const char *reason);

int   sim_event_triggered(const sim_event_t *e);
int   sim_event_processed(const sim_event_t *e);
int   sim_event_ok(const sim_event_t *e);   /* 1 = success, 0 = failure */
void *sim_event_value(const sim_event_t *e);
const char *sim_event_reason(const sim_event_t *e);  /* failure reason */

/* Register a callback. May be called multiple times. Callbacks fire
 * in registration order when the event is processed. */
void  sim_event_on(sim_event_t *e, sim_callback_fn cb, void *user);

/* --- Processes ------------------------------------------------------- */

sim_process_t *sim_process(sim_env_t *env, sim_proc_fn fn, void *arg);

/* The event that fires when the process terminates. Watch it to wait
 * for the process. Owned by the process; do not free. */
sim_event_t *sim_process_event(sim_process_t *p);

/* Suspend `self` until `evt` is processed, then resume. Returns the
 * event's value pointer (== sim_event_value(evt)) on normal wake-up,
 * or the interrupt cause if the process was interrupted while waiting.
 * Call sim_process_was_interrupted(self) to distinguish. */
void *sim_yield(sim_process_t *self, sim_event_t *evt);

/* Interrupt a process: schedule it for immediate resume regardless of
 * the event it was waiting on. The originally-yielded event's later
 * firing is ignored. No-op for finished processes. `cause` is opaque
 * user data, retrievable inside the process via sim_process_interrupt_cause. */
void  sim_process_interrupt(sim_process_t *p, void *cause);

/* 1 if the most recent sim_yield returned because of an interrupt
 * rather than the yielded event firing. The flag is cleared on the
 * next call to sim_yield. */
int   sim_process_was_interrupted(const sim_process_t *self);
void *sim_process_interrupt_cause(const sim_process_t *self);

/* --- Conditions ------------------------------------------------------ */

/* AllOf: fires once all listed events are processed.
 * AnyOf: fires once at least one is processed.
 * The returned event's value is unspecified; inspect the input events. */
sim_event_t *sim_all_of(sim_env_t *env, sim_event_t * const *events, size_t n);
sim_event_t *sim_any_of(sim_env_t *env, sim_event_t * const *events, size_t n);

/* --- Resource (capacity N, FIFO) ------------------------------------ */

sim_resource_t *sim_resource_create(sim_env_t *env, int capacity);
void            sim_resource_destroy(sim_resource_t *r);

/* Returns an event that fires when the resource is acquired. The same
 * event handle must be passed to sim_resource_release. */
sim_event_t    *sim_resource_request(sim_resource_t *r);
void            sim_resource_release(sim_resource_t *r, sim_event_t *req);

int             sim_resource_count(const sim_resource_t *r);     /* in use */
int             sim_resource_capacity(const sim_resource_t *r);
int             sim_resource_queue_len(const sim_resource_t *r); /* waiting */

/* --- PriorityResource (priority-ordered FIFO queue) ----------------- */

/* Smaller `priority` = served sooner. Ties broken by request order. */
sim_priority_resource_t *sim_priority_resource_create(sim_env_t *env, int capacity);
void          sim_priority_resource_destroy(sim_priority_resource_t *r);
sim_event_t  *sim_priority_resource_request(sim_priority_resource_t *r, int priority);
void          sim_priority_resource_release(sim_priority_resource_t *r, sim_event_t *req);
int           sim_priority_resource_count(const sim_priority_resource_t *r);
int           sim_priority_resource_capacity(const sim_priority_resource_t *r);
int           sim_priority_resource_queue_len(const sim_priority_resource_t *r);

/* --- PreemptiveResource (PriorityResource + preemption) ------------- */

/* Like PriorityResource, but a request with `preempt=1` may evict a
 * current holder with a worse (larger) priority. The evicted holder's
 * process is interrupted; the cause is the new requester's req event. */
sim_preemptive_resource_t *sim_preemptive_resource_create(sim_env_t *env, int capacity);
void          sim_preemptive_resource_destroy(sim_preemptive_resource_t *r);

/* `requester` is the process that will own this request (needed so
 * the preemption path can interrupt it). May be NULL if you don't
 * need preemption against this holder. */
sim_event_t  *sim_preemptive_resource_request(sim_preemptive_resource_t *r,
                                              sim_process_t *requester,
                                              int priority,
                                              int preempt);
void          sim_preemptive_resource_release(sim_preemptive_resource_t *r,
                                              sim_event_t *req);
int           sim_preemptive_resource_count(const sim_preemptive_resource_t *r);
int           sim_preemptive_resource_capacity(const sim_preemptive_resource_t *r);
int           sim_preemptive_resource_queue_len(const sim_preemptive_resource_t *r);

/* --- Container (continuous level) ----------------------------------- */

sim_container_t *sim_container_create(sim_env_t *env,
                                      double capacity, double init);
void             sim_container_destroy(sim_container_t *c);
sim_event_t     *sim_container_put(sim_container_t *c, double amount);
sim_event_t     *sim_container_get(sim_container_t *c, double amount);
double           sim_container_level(const sim_container_t *c);
double           sim_container_capacity(const sim_container_t *c);

/* --- Store (discrete items, FIFO) ----------------------------------- */

/* capacity <= 0 means unbounded. */
sim_store_t *sim_store_create(sim_env_t *env, int capacity);
void         sim_store_destroy(sim_store_t *s);
sim_event_t *sim_store_put(sim_store_t *s, void *item);
sim_event_t *sim_store_get(sim_store_t *s);   /* event value() is the item */
int          sim_store_count(const sim_store_t *s);

/* --- FilterStore (Store with predicate-based get) ------------------ */

/* Filter callback: return non-zero to accept the item. `user` is the
 * cookie passed to sim_filter_store_get. */
typedef int (*sim_store_filter_fn)(void *item, void *user);

sim_filter_store_t *sim_filter_store_create(sim_env_t *env, int capacity);
void         sim_filter_store_destroy(sim_filter_store_t *s);
sim_event_t *sim_filter_store_put(sim_filter_store_t *s, void *item);

/* Returns an event whose value, on success, is the first item for
 * which filter(item, user) returned non-zero. If `filter` is NULL,
 * any item matches (equivalent to plain Store.get). */
sim_event_t *sim_filter_store_get(sim_filter_store_t *s,
                                  sim_store_filter_fn filter, void *user);
int          sim_filter_store_count(const sim_filter_store_t *s);

/* --- PriorityStore (priority-ordered Store) ------------------------- */

/* Items put with a priority; get returns the lowest-priority value
 * first (smaller value = higher priority, matching SimPy). */
sim_priority_store_t *sim_priority_store_create(sim_env_t *env, int capacity);
void         sim_priority_store_destroy(sim_priority_store_t *s);
sim_event_t *sim_priority_store_put(sim_priority_store_t *s,
                                    int priority, void *item);
sim_event_t *sim_priority_store_get(sim_priority_store_t *s);
int          sim_priority_store_count(const sim_priority_store_t *s);

/* --- Realtime environment ------------------------------------------- */

/* Enable real-time pacing on `env`. `factor` is wall seconds per
 * simulation time unit (factor=1.0 ⇒ 1 sim sec == 1 wall sec). If
 * `strict` is non-zero, sim_run aborts (returns early) when the
 * scheduler falls behind real time.
 *
 * Call with factor=0 to disable. The wall-clock reference is reset
 * each time this is called. */
void  sim_env_set_realtime(sim_env_t *env, double factor, int strict);
int   sim_env_realtime_lagged(const sim_env_t *env);  /* 1 if strict run aborted */

#ifdef __cplusplus
}
#endif

#endif /* SIMPYC_H */
