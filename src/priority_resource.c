/* PriorityResource: capacity-N resource with priority-ordered queue.
 *
 * Smaller priority value is served first. Ties broken by request
 * arrival order (insertion seq). Queue is a sorted singly-linked
 * list — O(N) insert but N is typically small. */

#include "internal.h"

#include <stdlib.h>

typedef struct pr_node {
    sim_event_t   *event;
    int            priority;
    uint64_t       seq;
    struct pr_node *next;
} pr_node_t;

struct sim_priority_resource {
    sim_env_t  *env;
    int         capacity;
    int         in_use;
    int         queue_len;
    pr_node_t  *head;
    uint64_t    next_seq;
};

sim_priority_resource_t *sim_priority_resource_create(sim_env_t *env, int capacity) {
    sim_priority_resource_t *r = (sim_priority_resource_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->env = env;
    r->capacity = capacity;
    return r;
}

void sim_priority_resource_destroy(sim_priority_resource_t *r) {
    if (!r) return;
    pr_node_t *n = r->head;
    while (n) { pr_node_t *nn = n->next; free(n); n = nn; }
    free(r);
}

static void insert_sorted(sim_priority_resource_t *r, pr_node_t *n) {
    pr_node_t **cur = &r->head;
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

sim_event_t *sim_priority_resource_request(sim_priority_resource_t *r, int priority) {
    sim_event_t *ev = _sim_event_alloc(r->env);
    if (r->in_use < r->capacity) {
        r->in_use++;
        sim_event_succeed(ev, r);
    } else {
        pr_node_t *n = (pr_node_t *)malloc(sizeof(*n));
        n->event    = ev;
        n->priority = priority;
        n->seq      = r->next_seq++;
        n->next     = NULL;
        insert_sorted(r, n);
    }
    return ev;
}

void sim_priority_resource_release(sim_priority_resource_t *r, sim_event_t *req) {
    if (r->in_use == 0) return;
    if (r->head) {
        pr_node_t *n = r->head;
        r->head = n->next;
        r->queue_len--;
        sim_event_succeed(n->event, r);
        free(n);
    } else {
        r->in_use--;
    }
    if (req && sim_event_processed(req)) {
        req->free_next = r->env->event_pool;
        r->env->event_pool = req;
    }
}

int sim_priority_resource_count    (const sim_priority_resource_t *r) { return r->in_use; }
int sim_priority_resource_capacity (const sim_priority_resource_t *r) { return r->capacity; }
int sim_priority_resource_queue_len(const sim_priority_resource_t *r) { return r->queue_len; }
