/* FilterStore: Store where get() takes a predicate.
 *
 * Items are held in insertion order. A get-waiter carries its filter +
 * cookie; on put, we walk the waiters list and hand the item to the
 * first matching waiter. If no waiter matches, the item is buffered.
 * On get, we walk the buffer looking for a matching item; if none, we
 * queue the waiter. */

#include "internal.h"

#include <stdlib.h>

typedef struct fs_item {
    void           *item;
    struct fs_item *next;
} fs_item_t;

typedef struct fs_get_waiter {
    sim_event_t            *event;
    sim_store_filter_fn     filter;
    void                   *user;
    struct fs_get_waiter   *next;
} fs_get_waiter_t;

typedef struct fs_put_waiter {
    sim_event_t            *event;
    void                   *item;
    struct fs_put_waiter   *next;
} fs_put_waiter_t;

struct sim_filter_store {
    sim_env_t       *env;
    int              capacity;     /* <= 0 unbounded */
    int              count;
    fs_item_t       *head;
    fs_item_t       *tail;
    fs_get_waiter_t *gw_head, *gw_tail;
    fs_put_waiter_t *pw_head, *pw_tail;
};

sim_filter_store_t *sim_filter_store_create(sim_env_t *env, int capacity) {
    sim_filter_store_t *s = (sim_filter_store_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->env = env;
    s->capacity = capacity;
    return s;
}

void sim_filter_store_destroy(sim_filter_store_t *s) {
    if (!s) return;
    fs_item_t *it = s->head;
    while (it) { fs_item_t *nn = it->next; free(it); it = nn; }
    fs_get_waiter_t *g = s->gw_head;
    while (g) { fs_get_waiter_t *nn = g->next; free(g); g = nn; }
    fs_put_waiter_t *p = s->pw_head;
    while (p) { fs_put_waiter_t *nn = p->next; free(p); p = nn; }
    free(s);
}

static int accepts(sim_store_filter_fn filter, void *user, void *item) {
    return filter ? filter(item, user) != 0 : 1;
}

static void buffer_item(sim_filter_store_t *s, void *item) {
    fs_item_t *n = (fs_item_t *)malloc(sizeof(*n));
    n->item = item; n->next = NULL;
    if (s->tail) s->tail->next = n;
    else         s->head = n;
    s->tail = n;
    s->count++;
}

/* After buffering an item, see if any get-waiter accepts. Returns 1 if
 * an item was handed off and the buffer slot is gone again. */
static int try_dispatch_to_waiters(sim_filter_store_t *s) {
    fs_get_waiter_t **prev = &s->gw_head;
    fs_get_waiter_t *prev_node = NULL;
    while (*prev) {
        fs_get_waiter_t *g = *prev;
        /* Try each buffered item for this waiter. */
        fs_item_t **iprev = &s->head;
        fs_item_t *iprev_node = NULL;
        while (*iprev) {
            fs_item_t *it = *iprev;
            if (accepts(g->filter, g->user, it->item)) {
                void *item = it->item;
                *iprev = it->next;
                if (it == s->tail) s->tail = iprev_node;
                s->count--;
                free(it);

                /* Unlink this waiter. */
                *prev = g->next;
                if (g == s->gw_tail) s->gw_tail = prev_node;
                sim_event_succeed(g->event, item);
                free(g);
                return 1;
            }
            iprev_node = it;
            iprev = &it->next;
        }
        prev_node = g;
        prev = &g->next;
    }
    return 0;
}

sim_event_t *sim_filter_store_put(sim_filter_store_t *s, void *item) {
    sim_event_t *ev = _sim_event_alloc(s->env);
    ev->recyclable = 1;

    /* Capacity-blocked? Queue the put. */
    if (s->capacity > 0 && s->count >= s->capacity) {
        fs_put_waiter_t *w = (fs_put_waiter_t *)malloc(sizeof(*w));
        w->event = ev; w->item = item; w->next = NULL;
        if (s->pw_tail) s->pw_tail->next = w;
        else            s->pw_head = w;
        s->pw_tail = w;
        return ev;
    }

    /* Try direct handoff to a matching get-waiter without buffering. */
    fs_get_waiter_t **prev = &s->gw_head;
    fs_get_waiter_t *prev_node = NULL;
    while (*prev) {
        fs_get_waiter_t *g = *prev;
        if (accepts(g->filter, g->user, item)) {
            *prev = g->next;
            if (g == s->gw_tail) s->gw_tail = prev_node;
            sim_event_succeed(g->event, item);
            free(g);
            sim_event_succeed(ev, s);
            return ev;
        }
        prev_node = g;
        prev = &g->next;
    }

    /* Buffer; nobody currently wants it. */
    buffer_item(s, item);
    sim_event_succeed(ev, s);
    return ev;
}

sim_event_t *sim_filter_store_get(sim_filter_store_t *s,
                                  sim_store_filter_fn filter, void *user)
{
    sim_event_t *ev = _sim_event_alloc(s->env);
    ev->recyclable = 1;

    /* Scan the buffer for a matching item. */
    fs_item_t **iprev = &s->head;
    fs_item_t *iprev_node = NULL;
    while (*iprev) {
        fs_item_t *it = *iprev;
        if (accepts(filter, user, it->item)) {
            void *item = it->item;
            *iprev = it->next;
            if (it == s->tail) s->tail = iprev_node;
            s->count--;
            free(it);
            sim_event_succeed(ev, item);

            /* Slot freed — drain a put-waiter if any. */
            if (s->pw_head) {
                fs_put_waiter_t *w = s->pw_head;
                s->pw_head = w->next;
                if (!s->pw_head) s->pw_tail = NULL;
                buffer_item(s, w->item);
                sim_event_succeed(w->event, s);
                free(w);
                /* New item may satisfy other waiters. */
                try_dispatch_to_waiters(s);
            }
            return ev;
        }
        iprev_node = it;
        iprev = &it->next;
    }

    /* Queue the waiter. */
    fs_get_waiter_t *g = (fs_get_waiter_t *)malloc(sizeof(*g));
    g->event  = ev;
    g->filter = filter;
    g->user   = user;
    g->next   = NULL;
    if (s->gw_tail) s->gw_tail->next = g;
    else            s->gw_head = g;
    s->gw_tail = g;
    return ev;
}

int sim_filter_store_count(const sim_filter_store_t *s) { return s->count; }
