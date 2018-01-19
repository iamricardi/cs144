/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next; 
  struct ctcp_state **prev;

  conn_t *conn;

  uint16_t recv_window;

  uint32_t seqno;
  uint32_t ackno;

  linked_list_t *unacked;
  int rt_timeout;

  _Bool LAST_ACK;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/**
 * Unacknowledged segment struct.
 */
typedef struct unacked {
  ctcp_segment_t *segment;
  long last_send_time;
  int num_rt_attempts;
} unacked_t;

/**
 * The maximum number of times to retransmit an unacknowledged segment before
 * assuming the other side is unresponsive.
 */
#define MAX_NUM_RT_ATTEMPTS 5

/**
 * Returns segment in host byte order.
 */
ctcp_segment_t * dat_segment(ctcp_state_t *state, void *data, size_t datalen);
ctcp_segment_t * ack_segment(ctcp_state_t *state);
ctcp_segment_t * fin_segment(ctcp_state_t *state);

/**
 * Converts segment to host/network byte order.
 */
void hton_segment(ctcp_segment_t *segment);
void ntoh_segment(ctcp_segment_t *segment);


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;

  state->recv_window = cfg->recv_window;

  state->seqno = 1;
  state->ackno = 1;

  state->unacked = ll_create();
  state->rt_timeout = cfg->rt_timeout;

  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* Read input from STDIN. */
  void *buf = calloc(MAX_SEG_DATA_SIZE, 1);
  int num_bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE);

  /* Create segment from input. */
  ctcp_segment_t *segment;
  if (num_bytes_read == -1) {
    segment = fin_segment(state);
  } else {
    segment = dat_segment(state, buf, num_bytes_read);
  }

  /* Convert segment to network byte order. */
  hton_segment(segment);

  /* Send segment. */
  conn_send(state->conn, segment, ntohs(segment->len));

  /* Store segment as an unacknowledged segment. */
  unacked_t *unacked = calloc(sizeof(unacked_t), 1);
  unacked->segment = segment;
  unacked->last_send_time = current_time();
  unacked->num_rt_attempts = 0;
  ll_add(state->unacked, unacked);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* Convert segment to host byte order. */
  ntoh_segment(segment);

  /* Drop segment if it is corrupted. */
  uint16_t tmp = segment->cksum;
  segment->cksum = 0;
  if (tmp != cksum(segment, segment->len)) return;

  /* Drop segment if is is a duplicate or out-of-order. */
  if (segment->seqno != state->ackno) return;

  /* If this segment acknowledges an unacknowledged segment, remove the
     unacknowledged segment from unacknowledged segments. */
  ll_node_t *node = ll_front(state->unacked);
  if (node) {
    ll_remove(state->unacked, node);
  }

  /* If receiving a FIN, transition to LAST_ACK state. */
  if (segment->flags & FIN) {
    state->LAST_ACK = 1;
  }

  /* If in LAST_ACK state and receiving an ACK, close connection. */
  if ((segment->flags & ACK) && state->LAST_ACK) {
    ctcp_destroy(state);
  }

  /* If segment is a FIN segment or has data, output segment. */
  uint16_t datalen = segment->len - sizeof(ctcp_segment_t);
  if ((segment->flags & FIN) || datalen) {
    /* Output segment. */
    conn_output(state->conn, segment->data, datalen);

    /* Update ackno. */
    if (segment->flags & FIN) {
      state->ackno = segment->seqno + 1;
    } else {
      state->ackno = segment->seqno + datalen;
    }

    /* Acknowledge segment. */
    segment = ack_segment(state);
    hton_segment(segment);
    conn_send(state->conn, segment, ntohs(segment->len));
  }
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
}

void ctcp_timer() {
  /* Iterate through each connecton's state. */
  ctcp_state_t *state = state_list;
  while (state) {

    /* Iterate through each unacknowledged segment. */
    ll_node_t *node = ll_front(state->unacked);
    while (node) {
      unacked_t *unacked = node->object;

      /* Retransmit unacknowledged segment if enough time has passed. */
      if (current_time() - unacked->last_send_time >= state->rt_timeout) {

        /* Assume other side is unresponsive if segment has been retransmitted
           the maximum number of retransmission times. */
        if (unacked->num_rt_attempts == MAX_NUM_RT_ATTEMPTS) {
          ctcp_destroy(state);
        }

        unacked->last_send_time = current_time();
        unacked->num_rt_attempts++;
        conn_send(state->conn, unacked->segment, ntohs(unacked->segment->len));
      }

      node = node->next;
    }

    state = state->next;
  }
}

ctcp_segment_t * dat_segment(ctcp_state_t *state, void *data, size_t datalen) {
  uint16_t seglen = sizeof(ctcp_segment_t) + datalen;
  ctcp_segment_t *segment = calloc(seglen, 1);
  segment->seqno = state->seqno;
  segment->ackno = state->ackno;
  segment->len = seglen;
  segment->flags |= ACK;
  segment->window = state->recv_window;
  memcpy(segment->data, data, datalen);
  segment->cksum = cksum(segment, seglen);

  state->seqno += datalen;

  return segment;
}

ctcp_segment_t * ack_segment(ctcp_state_t *state) {
  uint16_t seglen = sizeof(ctcp_segment_t);
  ctcp_segment_t *segment = calloc(seglen, 1);
  segment->seqno = state->seqno;
  segment->ackno = state->ackno;
  segment->len = seglen;
  segment->flags |= ACK;
  segment->window = state->recv_window;
  segment->cksum = cksum(segment, seglen);
  return segment;
}

ctcp_segment_t * fin_segment(ctcp_state_t *state) {
  uint16_t seglen = sizeof(ctcp_segment_t);
  ctcp_segment_t *segment = calloc(seglen, 1);
  segment->seqno = state->seqno;
  segment->ackno = state->ackno;
  segment->len = seglen;
  segment->flags |= FIN;
  segment->window = state->recv_window;
  segment->cksum = cksum(segment, seglen);

  state->seqno++;

  return segment;
}

void hton_segment(ctcp_segment_t *segment) {
  segment->seqno = htonl(segment->seqno);
  segment->ackno = htonl(segment->ackno);
  segment->len = htons(segment->len);
  segment->flags = htonl(segment->flags);
  segment->window = htons(segment->window);
}

void ntoh_segment(ctcp_segment_t *segment) {
  segment->seqno = ntohl(segment->seqno);
  segment->ackno = ntohl(segment->ackno);
  segment->len = ntohs(segment->len);
  segment->flags = ntohl(segment->flags);
  segment->window = ntohs(segment->window);
}
