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
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state { // Bản chất đây là danh sách liên kết đơn nhưng thêm trường prev là con trỏ cấp 2 phục vụ việc xóa node ở danh sách liên kết đơn
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */
 
  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  // linked_list_t *segments;  
                              /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */
   /* FIXME: Add other needed fields. */
  ctcp_config_t ctcp_config;              /* ctcp configuration */
  uint32_t seqno;                         /* the next sent byte */
  uint32_t ackno;                         /* the next received byte */ 

  ctcp_segment_t *last_sent_segment;      /* A copy of the lastest sent segment that is waiting ack (for retransmition if needed) */
  size_t last_len;                        /* length of the lastest sent segment */
  uint8_t waiting_ack;                    /* 1 = waiting ack of the last segment, 0 = the lastest sent segment has received ack */
  uint8_t retransmit_count;                    /* the number of retransmition (reset when receives ack or tear down connection when retransmition time > 5) */
  struct timeval last_send_time;          /* last sent time (for caculation timeout) */

  bool fin_sent;                          /* 1: if FIN segment has been sent; 0: if otherwise */
  bool fin_acked;                         /* 1: if FIN segment has been acknowledged; 0: if otherwise */
  bool fin_recv;                          /* 1: if FIN segment from peer has been sent; 0: if otherwise */
  bool fin_ack_sent;                      /* 1: if FIN segment from peer has been acknowledged; 0: if otherwise */
  
  uint8_t *pending_data;
  size_t pending_len;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */



static void send_ack(ctcp_state_t *state) {
  ctcp_segment_t *ackseg = calloc(1, sizeof(ctcp_segment_t));
  ackseg->seqno = htonl(state->seqno);
  ackseg->ackno = htonl(state->ackno);
  ackseg->len = htons(sizeof(ctcp_segment_t));
  ackseg->flags = TH_ACK;
  ackseg->window = htons(state->ctcp_config.recv_window);
  ackseg->cksum = cksum(ackseg, sizeof(ctcp_segment_t));
  conn_send(state->conn, ackseg, sizeof(ctcp_segment_t));
  free(ackseg);
  fprintf(stderr, "[SEND] ACK ackno=%u\n", state->ackno);
}

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL || cfg == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */ // Add new state to the top of the list
  ctcp_state_t *state = (ctcp_state_t *)calloc(1, sizeof(ctcp_state_t));
  state->next = state_list;
  state->prev = &state_list;
  if (state_list != NULL)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */
  /* Set fields for ctcp configuration */
  state->ctcp_config.recv_window = cfg->recv_window;
  state->ctcp_config.send_window = cfg->send_window;
  state->ctcp_config.rt_timeout = cfg->rt_timeout;
  state->ctcp_config.timer = cfg->timer;

  /* Set fields for tx/rx control elements */
  state->seqno = 1; /* Set initial sequence number */
  state->ackno = 1; /* Assume that initial sequence number of peer is 1 */

  state->last_sent_segment = NULL;
  state->last_len = 0;
  state->waiting_ack = 0;
  state->retransmit_count = 0;

  /* Set fields for tear down connection elements*/
  state->fin_ack_sent = false;
  state->fin_acked = false;
  state->fin_recv = false;
  state->fin_sent = false;
  
  free(cfg);

  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  if (state->next)
    state->next->prev = state->prev;
  *state->prev = state->next;

  conn_remove(state->conn);
  if (state->last_sent_segment)
    free(state->last_sent_segment);
  if (state->pending_data)
    free(state->pending_data);

  fprintf(stderr, "[DESTROY] Connection closed.\n");
  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  if (state->waiting_ack)
    return;

  uint8_t buf[MAX_SEG_DATA_SIZE];
  int bytes_read = conn_input(state->conn, buf, sizeof(buf));
  *(buf + bytes_read - 1) = '\0';
  bytes_read -= 1; // Ignore last new line character 
  if (bytes_read == 0)
    return;

  if (bytes_read < 0) {
    /* EOF → send FIN */
    ctcp_segment_t *fin_seg = calloc(1, sizeof(ctcp_segment_t));
    fin_seg->seqno = htonl(state->seqno);
    fin_seg->ackno = htonl(state->ackno);
    fin_seg->len = htons(sizeof(ctcp_segment_t));
    fin_seg->flags = TH_FIN | TH_ACK;
    fin_seg->window = htons(state->ctcp_config.recv_window);
    fin_seg->cksum = cksum(fin_seg, sizeof(ctcp_segment_t));

    conn_send(state->conn, fin_seg, sizeof(ctcp_segment_t));
    gettimeofday(&state->last_send_time, NULL);

    if (state->last_sent_segment)
      free(state->last_sent_segment);
    state->last_sent_segment = fin_seg;
    state->last_len = sizeof(ctcp_segment_t);
    state->waiting_ack = true;
    state->fin_sent = true;
    state->retransmit_count = 0;

    fprintf(stderr, "[SEND] FIN seq=%u\n", state->seqno);
    return;
  }

  /* Normal data segment */
  uint16_t seg_len = sizeof(ctcp_segment_t) + bytes_read;
  ctcp_segment_t *seg = calloc(1, seg_len);
  seg->seqno = htonl(state->seqno);
  seg->ackno = htonl(state->ackno);
  seg->len = htons(seg_len);
  seg->flags = TH_ACK;
  seg->window = htons(state->ctcp_config.recv_window);
  memcpy(seg->data, buf, bytes_read);
  seg->cksum = cksum(seg, seg_len);

  conn_send(state->conn, seg, seg_len);
  gettimeofday(&state->last_send_time, NULL);

  if (state->last_sent_segment)
    free(state->last_sent_segment);
  state->last_sent_segment = seg;
  state->last_len = seg_len;
  state->waiting_ack = true;
  state->retransmit_count = 0;

  fprintf(stderr, "[SEND] Data len=%d seqno=%u ackno=%u\n",
          bytes_read, state->seqno, state->ackno);

  state->seqno += bytes_read;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  if (!state || !segment)
    return;

  /* Check checksum */
  uint16_t old_cksum = segment->cksum;
  segment->cksum = 0;
  uint16_t calc_cksum = cksum(segment, ntohs(segment->len));
  if (old_cksum != calc_cksum) {
    fprintf(stderr, "[WARN] Bad checksum. Dropped.\n");
    free(segment);
    return;
  }

  uint32_t seqno = ntohl(segment->seqno);
  uint32_t ackno = ntohl(segment->ackno);
  uint16_t seg_len = ntohs(segment->len);
  uint16_t data_len = seg_len - sizeof(ctcp_segment_t);

  /* ACK handling */
  if (segment->flags & TH_ACK) {
    fprintf(stderr, "[ACK] Received ackno=%u (expect=%u)\n", ackno, state->seqno);

    if (ackno >= state->seqno) {
      state->waiting_ack = false;
      state->retransmit_count = 0;
      if (state->last_sent_segment) {
        free(state->last_sent_segment);
        state->last_sent_segment = NULL;
      }
      if (state->fin_sent)
        state->fin_acked = true;
    }
  }

  /* FIN handling */
  if (segment->flags & TH_FIN) {
    fprintf(stderr, "\n[FIN] Received FIN seq=%u\n", seqno);
    state->fin_recv = true;
    state->ackno = seqno + 1;
    send_ack(state);
    state->fin_ack_sent = true;
  }

  /* Data handling */
  if (data_len > 0) {
    if (seqno < state->ackno) {
      fprintf(stderr, "\n[DUP] Duplicate segment seq=%u (already acked=%u)\n", seqno, state->ackno);
      send_ack(state);
    } else if (seqno == state->ackno) {
      conn_output(state->conn, (char *)segment->data, data_len);
      state->ackno += data_len;
      fprintf(stderr, "\n[DATA] Received len=%u seqno=%u -> ackno=%u\n", data_len, seqno, state->ackno);
      send_ack(state);
    }
  }

  if (state->fin_sent && state->fin_recv && state->fin_acked && state->fin_ack_sent) {
    fprintf(stderr, "\n[TEARDOWN] Closing connection.\n");
    ctcp_destroy(state);
  }

  free(segment);
}

void ctcp_output(ctcp_state_t *state) {
  if (!state)
    return;

  int buf_space = conn_bufspace(state->conn);
  if (buf_space <= 0)
    return;

  if (state->pending_data && state->pending_len > 0) {
    int to_write = MIN(buf_space, state->pending_len);
    conn_output(state->conn, (char *)state->pending_data, to_write);
    free(state->pending_data);
    state->pending_data = NULL;
    state->pending_len = 0;
  }

  if (state->fin_recv && !state->pending_data)
    conn_output(state->conn, NULL, 0);
}



void ctcp_timer() {
  struct timeval now;
  gettimeofday(&now, NULL);
  ctcp_state_t *s;	
  for (s = state_list; s != NULL; ) {
    ctcp_state_t *next = s->next;

    if (!s->waiting_ack) {
      s = next;
      continue;
    }

    uint64_t elapsed = (now.tv_sec - s->last_send_time.tv_sec) * 1000ULL +
                       (now.tv_usec - s->last_send_time.tv_usec) / 1000ULL;

    if (elapsed >= s->ctcp_config.rt_timeout) {
      if (s->retransmit_count >= 5) {
        fprintf(stderr, "[TIMEOUT] Max retries reached. Closing connection.\n");
        ctcp_destroy(s);
        s = next;
        continue;
      }

      int ret = conn_send(s->conn, s->last_sent_segment, s->last_len);
      if (ret > 0) {
        gettimeofday(&s->last_send_time, NULL);
        s->retransmit_count++;
        fprintf(stderr, "[RETRANSMIT] seq=%u count=%d\n",
                ntohl(s->last_sent_segment->seqno), s->retransmit_count);
      }
    }

    s = next;
  }
}
