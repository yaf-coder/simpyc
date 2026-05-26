/* Container: continuous quantity with put/get blocking until satisfiable.
 *
 * Both queues are FIFO. A put unblocks waiting gets; a get unblocks
 * waiting puts. Multiple waiters of the same kind drain in order. */

#include "internal.h"

#include <stdlib.h>

typedef struct cq_node {
    sim_event_t    *event;
    double          amount;
    struct cq_node *next;
} cq_node_t;

struct sim_container {
    sim_env_t *env;
    double     capacity;
    double     level;
    cq_node_t *put_head, *put_tail;
    cq_node_t *get_head, *get_tail;
};

static void cq_push(cq_node_t **head, cq_node_t **tail,
                    sim_event_t *ev, double amount) {
    cq_node_t *n = (cq_node_t *)malloc(sizeof(*n));
    n->event = ev; n->amount = amount; n->next = NULL;
    if (*tail) (*tail)->next = n;
    else       *head = n;
    *tail = n;
}

sim_container_t *sim_container_create(sim_env_t *env,
                                      double capacity, double init) {
    sim_container_t *c = (sim_container_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->env = env;
    c->capacity = capacity;
    c->level = init;
    return c;
}

void sim_container_destroy(sim_container_t *c) {
    if (!c) return;
    cq_node_t *n;
    for (n = c->put_head; n; ) { cq_node_t *nn = n->next; free(n); n = nn; }
    for (n = c->get_head; n; ) { cq_node_t *nn = n->next; free(n); n = nn; }
    free(c);
}

/* After a get, try to wake puts (more headroom now); after a put, try
 * to wake gets (more level now). */
static void try_wake_gets(sim_container_t *c) {
    while (c->get_head && c->get_head->amount <= c->level) {
        cq_node_t *n = c->get_head;
        c->level -= n->amount;
        c->get_head = n->next;
        if (!c->get_head) c->get_tail = NULL;
        sim_event_succeed(n->event, c);
        free(n);
    }
}

static void try_wake_puts(sim_container_t *c) {
    while (c->put_head && c->level + c->put_head->amount <= c->capacity) {
        cq_node_t *n = c->put_head;
        c->level += n->amount;
        c->put_head = n->next;
        if (!c->put_head) c->put_tail = NULL;
        sim_event_succeed(n->event, c);
        free(n);
    }
}

sim_event_t *sim_container_put(sim_container_t *c, double amount) {
    sim_event_t *ev = _sim_event_alloc(c->env);
    if (!c->put_head && c->level + amount <= c->capacity) {
        c->level += amount;
        sim_event_succeed(ev, c);
        try_wake_gets(c);
    } else {
        cq_push(&c->put_head, &c->put_tail, ev, amount);
    }
    return ev;
}

sim_event_t *sim_container_get(sim_container_t *c, double amount) {
    sim_event_t *ev = _sim_event_alloc(c->env);
    if (!c->get_head && amount <= c->level) {
        c->level -= amount;
        sim_event_succeed(ev, c);
        try_wake_puts(c);
    } else {
        cq_push(&c->get_head, &c->get_tail, ev, amount);
    }
    return ev;
}

double sim_container_level   (const sim_container_t *c) { return c->level;    }
double sim_container_capacity(const sim_container_t *c) { return c->capacity; }
