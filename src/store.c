/* Store: bounded or unbounded FIFO of opaque items. */

#include "internal.h"

#include <stdlib.h>

typedef struct item_node {
    void             *item;
    struct item_node *next;
} item_node_t;

typedef struct waiter_node {
    sim_event_t       *event;
    void              *item;   /* used by put-waiters */
    struct waiter_node *next;
} waiter_node_t;

struct sim_store {
    sim_env_t     *env;
    int            capacity;     /* <= 0 = unbounded */
    int            count;
    item_node_t   *head, *tail;
    waiter_node_t *get_head, *get_tail;
    waiter_node_t *put_head, *put_tail;
};

sim_store_t *sim_store_create(sim_env_t *env, int capacity) {
    sim_store_t *s = (sim_store_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->env = env;
    s->capacity = capacity;
    return s;
}

void sim_store_destroy(sim_store_t *s) {
    if (!s) return;
    item_node_t *n;
    for (n = s->head; n; ) { item_node_t *nn = n->next; free(n); n = nn; }
    waiter_node_t *w;
    for (w = s->get_head; w; ) { waiter_node_t *nn = w->next; free(w); w = nn; }
    for (w = s->put_head; w; ) { waiter_node_t *nn = w->next; free(w); w = nn; }
    free(s);
}

static void item_push(sim_store_t *s, void *item) {
    item_node_t *n = (item_node_t *)malloc(sizeof(*n));
    n->item = item; n->next = NULL;
    if (s->tail) s->tail->next = n;
    else         s->head = n;
    s->tail = n;
    s->count++;
}

static void *item_pop(sim_store_t *s) {
    item_node_t *n = s->head;
    void *item = n->item;
    s->head = n->next;
    if (!s->head) s->tail = NULL;
    s->count--;
    free(n);
    return item;
}

static void waiter_push(waiter_node_t **head, waiter_node_t **tail,
                        sim_event_t *ev, void *item) {
    waiter_node_t *n = (waiter_node_t *)malloc(sizeof(*n));
    n->event = ev; n->item = item; n->next = NULL;
    if (*tail) (*tail)->next = n;
    else       *head = n;
    *tail = n;
}

sim_event_t *sim_store_put(sim_store_t *s, void *item) {
    sim_event_t *ev = _sim_event_alloc(s->env);
    ev->recyclable = 1;

    /* Hand directly to a waiting get if any. */
    if (s->get_head) {
        waiter_node_t *w = s->get_head;
        s->get_head = w->next;
        if (!s->get_head) s->get_tail = NULL;
        sim_event_succeed(w->event, item);
        free(w);
        sim_event_succeed(ev, s);
        return ev;
    }
    /* Room in the buffer? */
    if (s->capacity <= 0 || s->count < s->capacity) {
        item_push(s, item);
        sim_event_succeed(ev, s);
    } else {
        waiter_push(&s->put_head, &s->put_tail, ev, item);
    }
    return ev;
}

sim_event_t *sim_store_get(sim_store_t *s) {
    sim_event_t *ev = _sim_event_alloc(s->env);
    ev->recyclable = 1;

    if (s->count > 0) {
        void *item = item_pop(s);
        sim_event_succeed(ev, item);

        /* Make room: pull from a waiting putter if any. */
        if (s->put_head) {
            waiter_node_t *w = s->put_head;
            s->put_head = w->next;
            if (!s->put_head) s->put_tail = NULL;
            item_push(s, w->item);
            sim_event_succeed(w->event, s);
            free(w);
        }
    } else {
        waiter_push(&s->get_head, &s->get_tail, ev, NULL);
    }
    return ev;
}

int sim_store_count(const sim_store_t *s) { return s->count; }
