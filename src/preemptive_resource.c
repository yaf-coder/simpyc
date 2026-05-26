/* PreemptiveResource: PriorityResource where a higher-priority request
 * may evict a lower-priority holder.
 *
 * On preemption the holder's process is interrupted; the interrupt
 * cause is the new requester's req event. The evicted req is marked
 * "preempted" so that a later release(req) becomes a no-op.
 *
 * Holders are tracked in a singly-linked list. Waiters are kept in
 * sorted (priority, seq) order. */

#include "internal.h"

#include <stdlib.h>

typedef struct holder {
    sim_event_t   *req;
    sim_process_t *owner;
    int            priority;
    int            preempted;       /* 1 once interrupted */
    struct holder *next;
} holder_t;

typedef struct pwait {
    sim_event_t   *req;
    sim_process_t *owner;
    int            priority;
    int            preempt;
    uint64_t       seq;
    struct pwait  *next;
} pwait_t;

struct sim_preemptive_resource {
    sim_env_t *env;
    int        capacity;
    int        in_use;
    int        queue_len;
    holder_t  *holders;
    pwait_t   *waiters;       /* sorted asc by (priority, seq) */
    uint64_t   next_seq;
};

sim_preemptive_resource_t *sim_preemptive_resource_create(sim_env_t *env, int capacity) {
    sim_preemptive_resource_t *r = (sim_preemptive_resource_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->env = env;
    r->capacity = capacity;
    return r;
}

void sim_preemptive_resource_destroy(sim_preemptive_resource_t *r) {
    if (!r) return;
    holder_t *h = r->holders;
    while (h) { holder_t *nn = h->next; free(h); h = nn; }
    pwait_t *w = r->waiters;
    while (w) { pwait_t *nn = w->next; free(w); w = nn; }
    free(r);
}

static void insert_waiter(sim_preemptive_resource_t *r, pwait_t *n) {
    pwait_t **cur = &r->waiters;
    while (*cur) {
        int p = (*cur)->priority;
        if (n->priority < p) break;
        if (n->priority == p && n->seq < (*cur)->seq) break;
        cur = &(*cur)->next;
    }
    n->next = *cur;
    *cur = n;
    r->queue_len++;
}

/* Find the holder with the worst (largest) priority. Returns NULL on
 * empty holders. Caller holds a pointer to *prev_link for unlink. */
static holder_t *find_weakest(sim_preemptive_resource_t *r, holder_t ***prev_out) {
    holder_t **prev = &r->holders;
    holder_t **weakest_prev = NULL;
    holder_t  *weakest = NULL;
    while (*prev) {
        holder_t *h = *prev;
        if (!weakest || h->priority > weakest->priority) {
            weakest = h;
            weakest_prev = prev;
        }
        prev = &h->next;
    }
    if (prev_out) *prev_out = weakest_prev;
    return weakest;
}

static void add_holder(sim_preemptive_resource_t *r, sim_event_t *req,
                       sim_process_t *owner, int priority) {
    holder_t *h = (holder_t *)malloc(sizeof(*h));
    h->req       = req;
    h->owner     = owner;
    h->priority  = priority;
    h->preempted = 0;
    h->next      = r->holders;
    r->holders   = h;
    r->in_use++;
}

sim_event_t *sim_preemptive_resource_request(sim_preemptive_resource_t *r,
                                             sim_process_t *requester,
                                             int priority,
                                             int preempt)
{
    sim_event_t *ev = _sim_event_alloc(r->env);

    if (r->in_use < r->capacity) {
        add_holder(r, ev, requester, priority);
        sim_event_succeed(ev, r);
        return ev;
    }

    if (preempt) {
        /* Try to evict the weakest holder, if it's strictly weaker
         * (i.e. has a larger priority value) than the new requester. */
        holder_t **prev = NULL;
        holder_t *victim = find_weakest(r, &prev);
        if (victim && victim->priority > priority) {
            /* Unlink victim. */
            *prev = victim->next;
            r->in_use--;
            victim->preempted = 1;
            sim_process_t *vowner = victim->owner;
            free(victim);
            /* Grant to new requester. */
            add_holder(r, ev, requester, priority);
            sim_event_succeed(ev, r);
            /* Interrupt the evicted process; cause is the new req. */
            if (vowner) sim_process_interrupt(vowner, ev);
            return ev;
        }
    }

    /* Queue. */
    pwait_t *w = (pwait_t *)malloc(sizeof(*w));
    w->req      = ev;
    w->owner    = requester;
    w->priority = priority;
    w->preempt  = preempt;
    w->seq      = r->next_seq++;
    w->next     = NULL;
    insert_waiter(r, w);
    return ev;
}

void sim_preemptive_resource_release(sim_preemptive_resource_t *r, sim_event_t *req) {
    /* Locate in holders. If not present, it was preempted out — no-op. */
    holder_t **prev = &r->holders;
    while (*prev && (*prev)->req != req) prev = &(*prev)->next;
    if (*prev) {
        holder_t *h = *prev;
        *prev = h->next;
        free(h);
        r->in_use--;
    }

    /* Pull the highest-priority waiter (head of sorted list) into the
     * just-freed slot. */
    if (r->in_use < r->capacity && r->waiters) {
        pwait_t *w = r->waiters;
        r->waiters = w->next;
        r->queue_len--;
        add_holder(r, w->req, w->owner, w->priority);
        sim_event_succeed(w->req, r);
        free(w);
    }

    if (req && sim_event_processed(req)) {
        req->free_next = r->env->event_pool;
        r->env->event_pool = req;
    }
}

int sim_preemptive_resource_count    (const sim_preemptive_resource_t *r) { return r->in_use; }
int sim_preemptive_resource_capacity (const sim_preemptive_resource_t *r) { return r->capacity; }
int sim_preemptive_resource_queue_len(const sim_preemptive_resource_t *r) { return r->queue_len; }
