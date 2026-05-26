/* PriorityStore: Store where put() takes a priority and get() returns
 * the lowest-priority item first. Ties broken by insertion order.
 *
 * The item list is kept sorted on insert. Get-waiters are FIFO; on
 * each put, the head waiter is served (since it always wants the
 * highest-priority item, which is now at head). */

#include "internal.h"

#include <stdlib.h>

typedef struct ps_item {
    void           *item;
    int             priority;
    uint64_t        seq;
    struct ps_item *next;
} ps_item_t;

typedef struct ps_get_waiter {
    sim_event_t          *event;
    struct ps_get_waiter *next;
} ps_get_waiter_t;

typedef struct ps_put_waiter {
    sim_event_t          *event;
    void                 *item;
    int                   priority;
    struct ps_put_waiter *next;
} ps_put_waiter_t;

struct sim_priority_store {
    sim_env_t       *env;
    int              capacity;     /* <= 0 unbounded */
    int              count;
    uint64_t         next_seq;
    ps_item_t       *head;         /* sorted asc by (priority, seq) */
    ps_get_waiter_t *gw_head, *gw_tail;
    ps_put_waiter_t *pw_head, *pw_tail;
};

sim_priority_store_t *sim_priority_store_create(sim_env_t *env, int capacity) {
    sim_priority_store_t *s = (sim_priority_store_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->env = env;
    s->capacity = capacity;
    return s;
}

void sim_priority_store_destroy(sim_priority_store_t *s) {
    if (!s) return;
    ps_item_t *it = s->head;
    while (it) { ps_item_t *nn = it->next; free(it); it = nn; }
    ps_get_waiter_t *g = s->gw_head;
    while (g) { ps_get_waiter_t *nn = g->next; free(g); g = nn; }
    ps_put_waiter_t *p = s->pw_head;
    while (p) { ps_put_waiter_t *nn = p->next; free(p); p = nn; }
    free(s);
}

static void item_insert_sorted(sim_priority_store_t *s, ps_item_t *n) {
    ps_item_t **cur = &s->head;
    while (*cur) {
        int p = (*cur)->priority;
        if (n->priority < p) break;
        if (n->priority == p && n->seq < (*cur)->seq) break;
        cur = &(*cur)->next;
    }
    n->next = *cur;
    *cur = n;
    s->count++;
}

sim_event_t *sim_priority_store_put(sim_priority_store_t *s,
                                    int priority, void *item)
{
    sim_event_t *ev = _sim_event_alloc(s->env);
    ev->recyclable = 1;

    /* Capacity-blocked? Queue. */
    if (s->capacity > 0 && s->count >= s->capacity) {
        ps_put_waiter_t *w = (ps_put_waiter_t *)malloc(sizeof(*w));
        w->event    = ev;
        w->item     = item;
        w->priority = priority;
        w->next     = NULL;
        if (s->pw_tail) s->pw_tail->next = w;
        else            s->pw_head = w;
        s->pw_tail = w;
        return ev;
    }

    /* If a get is waiting, the new item becomes head of priority order
     * potentially. Insert sorted, then hand the new head to the
     * front waiter. */
    ps_item_t *n = (ps_item_t *)malloc(sizeof(*n));
    n->item     = item;
    n->priority = priority;
    n->seq      = s->next_seq++;
    n->next     = NULL;
    item_insert_sorted(s, n);
    sim_event_succeed(ev, s);

    if (s->gw_head) {
        ps_item_t *top = s->head;
        s->head = top->next;
        s->count--;
        ps_get_waiter_t *g = s->gw_head;
        s->gw_head = g->next;
        if (!s->gw_head) s->gw_tail = NULL;
        sim_event_succeed(g->event, top->item);
        free(top);
        free(g);
    }
    return ev;
}

sim_event_t *sim_priority_store_get(sim_priority_store_t *s) {
    sim_event_t *ev = _sim_event_alloc(s->env);
    ev->recyclable = 1;

    if (s->head) {
        ps_item_t *top = s->head;
        s->head = top->next;
        s->count--;
        sim_event_succeed(ev, top->item);
        free(top);

        /* Drain a put-waiter into the freed slot. */
        if (s->pw_head) {
            ps_put_waiter_t *w = s->pw_head;
            s->pw_head = w->next;
            if (!s->pw_head) s->pw_tail = NULL;
            ps_item_t *n = (ps_item_t *)malloc(sizeof(*n));
            n->item     = w->item;
            n->priority = w->priority;
            n->seq      = s->next_seq++;
            n->next     = NULL;
            item_insert_sorted(s, n);
            sim_event_succeed(w->event, s);
            free(w);
        }
        return ev;
    }

    /* Queue a get-waiter. */
    ps_get_waiter_t *g = (ps_get_waiter_t *)malloc(sizeof(*g));
    g->event = ev;
    g->next  = NULL;
    if (s->gw_tail) s->gw_tail->next = g;
    else            s->gw_head = g;
    s->gw_tail = g;
    return ev;
}

int sim_priority_store_count(const sim_priority_store_t *s) { return s->count; }
