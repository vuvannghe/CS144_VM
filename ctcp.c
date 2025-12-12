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
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);
  /* FIXME: Do any other cleanup here. */
  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  if(state->waiting_ack == 1)
  {
    // Add debug information
    return;
  }

  int byte_read;
  uint8_t *buf = (uint8_t *)malloc(MAX_SEG_DATA_SIZE * sizeof(uint8_t));
  byte_read = conn_input(state->conn, (void *) buf, MAX_SEG_DATA_SIZE);

  if(byte_read == 0)
  {
    // Add debug information
    return; 
  }

  if(byte_read == -1)
  {
    /* Create a FIN segemnt to send */
    ctcp_segment_t *new_segment = (ctcp_segment_t *)calloc(1, sizeof(ctcp_segment_t));
    new_segment->seqno = htonl(state->seqno);
    new_segment->ackno = htonl(state->ackno);
    new_segment->len = htons((uint16_t) sizeof(ctcp_segment_t));
    new_segment->flags |= FIN;
    new_segment->window = htons(state->ctcp_config.recv_window);
    new_segment->cksum = cksum(new_segment, ntohs(new_segment->len));

    state->seqno += 1;
    state->waiting_ack = 1;
    state->fin_sent = true;
    state->retransmit_count = 0;
    gettimeofday(&state->last_send_time, NULL);

    if(state->last_sent_segment != NULL)
    {
      free(state->last_sent_segment);
      state->last_sent_segment = NULL;
    }
    state->last_sent_segment = new_segment;
    state->last_len = ntohs(new_segment->len);

    conn_send(state->conn, new_segment, ntohs(new_segment->len));
  }
  else
  {
    /* if data is available (byte_read > 0). Create a segment to sent */
    uint16_t segment_len = sizeof(ctcp_segment_t) + byte_read;
    ctcp_segment_t *new_segment = (ctcp_segment_t *)calloc(1, segment_len);
    new_segment->seqno = htonl(state->seqno);
    new_segment->ackno = htonl(state->ackno);
    new_segment->len = htons(segment_len);
    // new_segment->flags |= ACK;
    new_segment->window = htons(state->ctcp_config.recv_window);
    memcpy((void *)new_segment->data, (void *)buf, byte_read);
    new_segment->cksum = cksum(new_segment, ntohs(new_segment->len));

    state->seqno += byte_read;
    state->waiting_ack = 1;
    state->retransmit_count = 0;
    gettimeofday(&state->last_send_time, NULL);

    if(state->last_sent_segment != NULL)
    {
      free(state->last_sent_segment);
      state->last_sent_segment = NULL;
    }
    state->last_sent_segment = new_segment;
    state->last_len = ntohs(new_segment->len);

    conn_send(state->conn, new_segment, ntohs(new_segment->len));
  }
  free(buf);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
  if(state == NULL || segment == NULL)
    return;
  
  uint16_t old_cksum = segment->cksum;
  segment->cksum = 0;
  uint16_t calc_cksum = cksum(segment, ntohs(segment->len));
  if(old_cksum != calc_cksum)
  {
    fprintf(stderr, "[WARN] Dropped corrupted segment (bad checksum)\n");
    free(segment);
    return;
  }
  fprintf(stderr, "[OK] Checksum valid\n");

  uint32_t seqno = ntohl(segment->seqno);
  uint32_t ackno = ntohl(segment->ackno);
  uint16_t seg_len = ntohs(segment->len);
  uint8_t flags = segment->flags;
  uint16_t data_len = seg_len - sizeof(ctcp_segment_t);

  if(segment->flags & ACK && seg_len == sizeof(ctcp_segment_t))
  {
    fprintf(stderr, "[ACK] Received ackno=%u (waiting for %u)\n", ackno, state->seqno);
    if (ackno >= state->seqno)
    {
      state->waiting_ack = 0;
      state->retransmit_count = 0;
      if(state->last_sent_segment != NULL)
      {
        free(state->last_sent_segment);
        state->last_sent_segment = NULL;
      }
      if(state->fin_sent == 1)
      {
        state->fin_acked = 1;
      }
    }
  }
  else
  {
    if(segment->flags & FIN)
    {
      // Send ACK segemnt for FIN
      fprintf(stderr, "[FIN] Received FIN seq=%u\n", seqno);

      ctcp_segment_t *finack_segment = (ctcp_segment_t *)calloc(1, sizeof(ctcp_segment_t));
      finack_segment->seqno = htonl(state->seqno);
      finack_segment->ackno = htonl(seqno + 1);
      finack_segment->len = htons(sizeof(ctcp_segment_t));
      finack_segment->flags |= ACK;
      finack_segment->window = htons(state->ctcp_config.recv_window);
      finack_segment->cksum = cksum(finack_segment, ntohs(finack_segment->len));
      conn_send(state->conn, finack_segment, sizeof(ctcp_segment_t));
      state->fin_recv = 1;
      state->fin_ack_sent = 1;
      fprintf(stderr, "[SEND] Sent ACK for FIN ackno=%u\n", seqno + 1);
      free(finack_segment); // Don't need to retransmit ack segment 
    }
  }

  if (state->fin_sent == 1 && state->fin_recv == 1 && state->fin_acked == 1 && state->fin_ack_sent == 1) 
  { 
    fprintf(stderr, "[TEARDOWN] Connection complete.\n");
    ctcp_destroy(state);
  }

  if(seg_len > sizeof(ctcp_segment_t))
  {
    fprintf(stderr, "[DATA] Received seqno=%u len=%u expected=%u\n",
                seqno, data_len, state->ackno);
    if (ackno == state->seqno)
    {
      if (state->pending_data != NULL)
      {
        free(state->pending_data);
        state->pending_data = NULL;
      }
      state->pending_data = (uint8_t *)malloc(data_len * sizeof(uint8_t));
      memcpy(state->pending_data, segment->data, data_len);
      state->pending_len = data_len;
      ctcp_output(state);
      
      state->ackno += data_len;
      ctcp_segment_t *ack_segment = (ctcp_segment_t *)calloc(1, sizeof(ctcp_segment_t));
      ack_segment->seqno = htonl(state->seqno);
      ack_segment->ackno = htonl(state->ackno);
      ack_segment->len = htons(sizeof(ctcp_segment_t));
      ack_segment->flags |= ACK;
      ack_segment->window = htons(state->ctcp_config.recv_window);
      ack_segment->cksum = cksum(ack_segment, ntohs(ack_segment->len));
      conn_send(state->conn, ack_segment, sizeof(ctcp_segment_t));

      free(ack_segment); // Don't need to retransmit ack segment 
    }

    
  }
  free(segment);
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
  if (state == NULL)
    return;

  int buf_space = conn_bufspace(state->conn);
  if (buf_space <= 0)
  { 
    fprintf(stderr, "No buffer space for output");
    return;
  }
  if (state->pending_data && state->pending_len > 0) 
  {
    if (state->pending_len > buf_space)
    {
      fprintf(stderr, "Not enough buffer space for output.");
      fprintf(stderr, "Actual segment data length: %lu. Available buffer space: %lu", 
                                                    state->pending_len, buf_space);
      return;
    }
    conn_output(state->conn, (char *)state->pending_data, state->pending_len);

    // Free buffer
    free(state->pending_data);
    state->pending_data = NULL;
    state->pending_len = 0;
  }

    // If FIN segment is received
  if (state->fin_recv && state->pending_data == NULL)
    conn_output(state->conn, NULL, 0);
}

void ctcp_timer() {
  /* FIXME */
  ctcp_state_t *current_state;
  struct timeval now;

  for(current_state = state_list; current_state != NULL; )
  {
    if(current_state->waiting_ack != 1)
    {
      current_state = current_state->next;
      continue;
    }
    gettimeofday(&now, NULL);
    uint64_t elasped_time = (now.tv_sec - current_state->last_send_time.tv_sec) * 1000 +
                            (now.tv_usec - current_state->last_send_time.tv_usec) / 1000; // in miliseconds
    
    ctcp_state_t *next_state = current_state->next;
    if(elasped_time - current_state->ctcp_config.rt_timeout >= 0)
    {
     
      if(current_state->retransmit_count >= 5)
      {
        ctcp_destroy(current_state);
        current_state = next_state;
        continue;
      }

      gettimeofday(&current_state->last_send_time, NULL);
      conn_send(current_state->conn, current_state->last_sent_segment, current_state->last_len);
      current_state->retransmit_count += 1;
      current_state = next_state;
    }
  }
}
  
