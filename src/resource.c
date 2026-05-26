/* Resource: N units, FIFO request queue.
 *
 * Each request returns a sim_event_t that fires when the unit is
 * granted. sim_resource_release(r, req) releases the unit; the next
 * waiter (if any) is granted in the same tick.
 */

#include "internal.h"

#include <stdlib.h>

typedef struct req_node {
    sim_event_t     *event;
    struct req_node *next;
} req_node_t;

struct sim_resource {
    sim_env_t   *env;
    int          capacity;
    int          in_use;
    int          queue_len;
    req_node_t  *head;
    req_node_t  *tail;
};

sim_resource_t *sim_resource_create(sim_env_t *env, int capacity) {
    sim_resource_t *r = (sim_resource_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->env = env;
    r->capacity = capacity;
    return r;
}

void sim_resource_destroy(sim_resource_t *r) {
    if (!r) return;
    req_node_t *n = r->head;
    while (n) { req_node_t *nn = n->next; free(n); n = nn; }
    free(r);
}

sim_event_t *sim_resource_request(sim_resource_t *r) {
    sim_event_t *ev = _sim_event_alloc(r->env);
    if (r->in_use < r->capacity) {
        r->in_use++;
        sim_event_succeed(ev, r);
    } else {
        req_node_t *n = (req_node_t *)malloc(sizeof(*n));
        n->event = ev;
        n->next  = NULL;
        if (r->tail) r->tail->next = n;
        else         r->head = n;
        r->tail = n;
        r->queue_len++;
    }
    return ev;
}

void sim_resource_release(sim_resource_t *r, sim_event_t *req) {
    (void)req;     /* req identifies the holder but we count by ref */
    if (r->in_use == 0) return;
    if (r->head) {
        req_node_t *n = r->head;
        r->head = n->next;
        if (!r->head) r->tail = NULL;
        r->queue_len--;
        sim_event_succeed(n->event, r);
        free(n);
        /* in_use stays the same: handed off. */
    } else {
        r->in_use--;
    }
}

int sim_resource_count    (const sim_resource_t *r) { return r->in_use;    }
int sim_resource_capacity (const sim_resource_t *r) { return r->capacity;  }
int sim_resource_queue_len(const sim_resource_t *r) { return r->queue_len; }
