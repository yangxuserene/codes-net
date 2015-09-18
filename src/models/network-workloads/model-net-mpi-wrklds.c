/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include <inttypes.h>

#include "codes/codes-workload.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/model-net.h"
#include "codes/codes-jobmap.h"

#define TRACE -1
/*global variable for loading multiple jobs' traces*/
char workloads_conf_file[8192];//the file in which the path and name of each job's traces are
char alloc_file[8192];// the file in which the preassgined LP lists for the jobs
int num_traces_of_job[5];//the number_of_traces of each job
char file_name_of_job[5][8192];//the name of each job

/*global variable for nw_test_finalize time analysis */
double totaltime[5] = {0,0,0,0,0};
double maxtime[5]={0,0,0,0,0};
double maxwaittime[5]={0,0,0,0,0};
double maxsendtime[5]={0,0,0,0,0};
double maxrecvtime[5]={0,0,0,0,0};
double maxcommtime[5]={0,0,0,0,0};
double avgtime[5]={0,0,0,0,0};
double avgcommtime[5]={0,0,0,0,0};
double avgwaittime[5]={0,0,0,0,0};
double avgsendtime[5]={0,0,0,0,0};
double avgrecvtime[5]={0,0,0,0,0};

char workload_type[128];
char workload_file[8192];
char offset_file[8192];
static int wrkld_id;
static int num_net_traces = 0;

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;
typedef int16_t dumpi_req_id;

static int net_id = 0;
static float noise = 5.0;
static int num_net_lps, num_nw_lps;
long long num_bytes_sent=0;
long long num_bytes_recvd=0;
double max_time = 0,  max_comm_time = 0, max_wait_time = 0, max_send_time = 0, max_recv_time = 0;
double avg_time = 0, avg_comm_time = 0, avg_wait_time = 0, avg_send_time = 0, avg_recv_time = 0;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH], annotation[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;


/*global variables for job mapping */
struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;

/* runtime option for disabling computation time simulation */
static int disable_delay = 0;

/* MPI_OP_GET_NEXT is for getting next MPI operation when the previous operation completes.
* MPI_SEND_ARRIVED is issued when a MPI message arrives at its destination (the message is transported by model-net and an event is invoked when it arrives. 
* MPI_SEND_POSTED is issued when a MPI message has left the source LP (message is transported via model-net). */
enum MPI_NW_EVENTS
{
	MPI_OP_GET_NEXT=1,
	MPI_SEND_ARRIVED,
        MPI_SEND_ARRIVED_CB, // for tracking message times on sender
	MPI_SEND_POSTED,
};

/* stores pointers of pending MPI operations to be matched with their respective sends/receives. */
struct mpi_msgs_queue
{
	struct codes_workload_op* mpi_op;
	struct mpi_msgs_queue* next;
};

/* stores request IDs of completed MPI operations (Isends or Irecvs) */
struct completed_requests
{
	dumpi_req_id req_id;
	struct completed_requests* next;
};

/* for wait operations, store the pending operation and number of completed waits so far. */
struct pending_waits
{
	struct codes_workload_op* mpi_op;
	int num_completed;
	tw_stime start_time;
};

/* maintains the head and tail of the queue, as well as the number of elements currently in queue. Queues are pending_recvs queue (holds unmatched MPI recv operations) and arrival_queue (holds unmatched MPI send messages). */
struct mpi_queue_ptrs
{
	int num_elems;
	struct mpi_msgs_queue* queue_head;
	struct mpi_msgs_queue* queue_tail;
};

/* state of the network LP. It contains the pointers to send/receive lists */
struct nw_state
{
	long num_events_per_lp;
	tw_lpid nw_id;
	short wrkld_end;

    int app_id;
    int local_rank;

	/* count of sends, receives, collectives and delays */
	unsigned long num_sends;
	unsigned long num_recvs;
	unsigned long num_cols;
	unsigned long num_delays;
	unsigned long num_wait;
	unsigned long num_waitall;
	unsigned long num_waitsome;

	/* time spent by the LP in executing the app trace*/
	double start_time;
	double elapsed_time;

	/* time spent in compute operations */
	double compute_time;

	/* time spent in message send/isend */
	double send_time;

	/* time spent in message receive */
	double recv_time;
	
	/* time spent in wait operation */
	double wait_time;

	/* FIFO for isend messages arrived on destination */
	struct mpi_queue_ptrs* arrival_queue;

	/* FIFO for irecv messages posted but not yet matched with send operations */
	struct mpi_queue_ptrs* pending_recvs_queue;

	/* list of pending waits (and saved pending wait for reverse computation) */
	struct pending_waits* pending_waits;

	/* List of completed send/receive requests */
	struct completed_requests* completed_reqs;
};

/* data for handling reverse computation.
* saved_matched_req holds the request ID of matched receives/sends for wait operations.
* ptr_match_op holds the matched MPI operation which are removed from the queues when a send is matched with the receive in forward event handler. 
* network event being sent. op is the MPI operation issued by the network workloads API. rv_data holds the data for reverse computation (TODO: Fill this data structure only when the simulation runs in optimistic mode). */
struct nw_message
{
   int msg_type;
   /* for reverse computation */
   struct codes_workload_op * op;

   struct
   {
     /* forward event handler */
     struct
     {
        int op_type;
        tw_lpid src_rank;
        tw_lpid dest_rank;
        int num_bytes;
        int data_type;
        double sim_start_time;
        // for callbacks - time message was received
        double msg_send_time;
        int16_t req_id;   
        int tag;
     } msg_info;

     /* required for reverse computation*/
     struct 
      {
	int found_match;
	short matched_op;
	dumpi_req_id saved_matched_req;
	struct codes_workload_op* ptr_match_op;
	struct pending_waits* saved_pending_wait;

	double saved_send_time;
	double saved_recv_time;
	double saved_wait_time;
      } rc;
  } u;
};

/* executes MPI wait operation */
static void codes_exec_mpi_wait(nw_state* s, tw_bf* bf, nw_message* m, tw_lp* lp);

/* reverse of mpi wait function. */
static void codes_exec_mpi_wait_rc(nw_state* s, tw_bf* bf, nw_message* m, tw_lp* lp);

/*find the global LP id of source and destination lps of each message*/
void find_glp_for_msg( nw_message* m, struct codes_jobmap_id *jp_id);

/* executes MPI isend and send operations */
static void codes_exec_mpi_send(nw_state* s, nw_message* m, tw_lp* lp);

/* execute MPI irecv operation */
static void codes_exec_mpi_recv(nw_state* s, nw_message* m, tw_lp* lp);

/* reverse of mpi recv function. */
static void codes_exec_mpi_recv_rc(nw_state* s, nw_message* m, tw_lp* lp);

/* execute the computational delay */
static void codes_exec_comp_delay(nw_state* s, nw_message* m, tw_lp* lp);

/* execute collective operation, currently only skips these operations. */
static void codes_exec_mpi_col(nw_state* s, nw_message* m, tw_lp* lp);

/* gets the next MPI operation from the network-workloads API. */
static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);

/* reverse handler of get next mpi operation. */
static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);

/* Makes a call to get_next_mpi_operation. */
static void codes_issue_next_event(tw_lp* lp);

///////////////////// HELPER FUNCTIONS FOR MPI MESSAGE QUEUE HANDLING ///////////////
/* upon arrival of local completion message, inserts operation in completed send queue */
static void update_send_completion_queue(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* reverse of the above function */
static void update_send_completion_queue_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* upon arrival of an isend operation, updates the arrival queue of the network */
static void update_arrival_queue(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* reverse of the above function */
static void update_arrival_queue_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* callback to a message sender for computing message time */
static void update_message_time(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);
static void update_message_time_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* insert MPI operation in the waiting queue*/
static void mpi_pending_queue_insert_op(struct mpi_queue_ptrs* mpi_queue, struct codes_workload_op* mpi_op);

/* remove completed request IDs from the queue for reuse. Reverse of above function. */
static void remove_req_id(struct completed_requests** requests, int16_t req_id);

/* remove MPI operation from the waiting queue.*/
static int mpi_queue_remove_matching_op(nw_state* s, tw_lp* lp, struct mpi_queue_ptrs* mpi_queue, nw_message * m);

/* remove the tail of the MPI operation from waiting queue */
static int mpi_queue_remove_tail(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue);

/* insert completed MPI requests in the queue. */
static void mpi_completed_queue_insert_op(struct completed_requests** mpi_completed_queue, dumpi_req_id req_id);

/* notifies the wait operations (if any) about the completed receives and sends requests. */
static int notify_waits(nw_state* s, tw_bf* bf, tw_lp* lp, nw_message* m, dumpi_req_id req_id);

/* reverse of notify waits function. */
static void notify_waits_rc(nw_state* s, tw_bf* bf, tw_lp* lp, nw_message* m, dumpi_req_id completed_req);

/* conversion from seconds to eanaoseconds */
static tw_stime s_to_ns(tw_stime ns);

/* helper function - maps an MPI rank to an LP id */
static tw_lpid rank_to_lpid(int rank)
{
    return codes_mapping_get_lpid_from_relative(rank, NULL, "nw-lp", NULL, 0);
}

/* initializes the queue and allocates memory */
static struct mpi_queue_ptrs* queue_init()
{
	struct mpi_queue_ptrs* mpi_queue = malloc(sizeof(struct mpi_queue_ptrs));

	mpi_queue->num_elems = 0;
	mpi_queue->queue_head = NULL;
	mpi_queue->queue_tail = NULL;
	
	return mpi_queue;
}

/* helper function: counts number of elements in the queue */
static int numQueue(struct mpi_queue_ptrs* mpi_queue)
{
	struct mpi_msgs_queue* tmp = malloc(sizeof(struct mpi_msgs_queue)); 
	assert(tmp);

	tmp = mpi_queue->queue_head;
	int count = 0;

	while(tmp)
	{
		++count;
		tmp = tmp->next;
	}
	return count;
	free(tmp);
}

/* prints elements in a send/recv queue */
static void printQueue(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, char* msg)
{
	printf("\n ************ Printing the queue %s *************** ", msg);
	struct mpi_msgs_queue* tmp = malloc(sizeof(struct mpi_msgs_queue));
	assert(tmp);

	tmp = mpi_queue->queue_head;
	
	while(tmp)
	{
		if(tmp->mpi_op->op_type == CODES_WK_SEND || tmp->mpi_op->op_type == CODES_WK_ISEND)
			printf("\n lpid %ld send operation data type %d count %d tag %d source %d", 
				    lpid, tmp->mpi_op->u.send.data_type, tmp->mpi_op->u.send.count, 
				     tmp->mpi_op->u.send.tag, tmp->mpi_op->u.send.source_rank);
		else if(tmp->mpi_op->op_type == CODES_WK_IRECV || tmp->mpi_op->op_type == CODES_WK_RECV)
			printf("\n lpid %ld recv operation data type %d count %d tag %d source %d", 
				   lpid, tmp->mpi_op->u.recv.data_type, tmp->mpi_op->u.recv.count, 
				    tmp->mpi_op->u.recv.tag, tmp->mpi_op->u.recv.source_rank );
		else
			printf("\n Invalid data type in the queue %d ", tmp->mpi_op->op_type);
		tmp = tmp->next;
	}
	free(tmp);
}

/* re-insert element in the queue at the index --- maintained for reverse computation */
static void mpi_queue_update(struct mpi_queue_ptrs* mpi_queue, struct codes_workload_op* mpi_op, int pos)
{
	struct mpi_msgs_queue* elem = malloc(sizeof(struct mpi_msgs_queue));
	assert(elem);
	elem->mpi_op = mpi_op;
	
	/* inserting at the head */
	if(pos == 0)
	{
	   if(!mpi_queue->queue_tail)
		mpi_queue->queue_tail = elem;
	   elem->next = mpi_queue->queue_head;
	   mpi_queue->queue_head = elem;
	   mpi_queue->num_elems++;
	   return;
	}

	int index = 0;
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;
	while(index < pos - 1)
	{
		tmp = tmp->next;
		++index;
	}

	if(!tmp)
		printf("\n Invalid index! %d pos %d size %d ", index, pos, numQueue(mpi_queue));
	if(tmp == mpi_queue->queue_tail)
	    mpi_queue->queue_tail = elem;

	elem->next = tmp->next;
	tmp->next = elem;
	mpi_queue->num_elems++;

	return;
}

/* prints the elements of a queue (for debugging purposes). */
static void printCompletedQueue(nw_state* s, tw_lp* lp)
{
	   if(TRACE == lp->gid)
	   {
	   	printf("\n %lf contents of completed operations queue ", tw_now(lp));
	   	struct completed_requests* current = s->completed_reqs;
	   	while(current)
	    	{
			printf(" %d ",current->req_id);
			current = current->next;
	   	}
	   }
}

/* reverse handler of notify_waits function. */
static void notify_waits_rc(nw_state* s, tw_bf* bf, tw_lp* lp, nw_message* m, dumpi_req_id completed_req)
{
   int i;

   /*if(bf->c1)
    {*/
	/* if pending wait is still present and is of type MPI_WAIT then do nothing*/
/*	s->wait_time = s->saved_wait_time; 	
	mpi_completed_queue_insert_op(&s->completed_reqs, completed_req);	
	s->pending_waits = wait_elem;
	s->saved_pending_wait = NULL;
    }
*/
  if(lp->gid == TRACE)
	  printf("\n %lf reverse -- notify waits req id %d ", tw_now(lp), completed_req);
  printCompletedQueue(s, lp);
  if(m->u.rc.matched_op == 1)
	s->pending_waits->num_completed--;
   /* if a wait-elem exists, it means the request ID has been matched*/
   if(m->u.rc.matched_op == 2) 
    {
	if(lp->gid == TRACE)
	{
		printf("\n %lf matched req id %d ", tw_now(lp), completed_req);
		printCompletedQueue(s, lp);
	}
        struct pending_waits* wait_elem = m->u.rc.saved_pending_wait;
	s->wait_time = m->u.rc.saved_wait_time;
	int count = wait_elem->mpi_op->u.waits.count; 

	for( i = 0; i < count; i++ )
		mpi_completed_queue_insert_op(&s->completed_reqs, wait_elem->mpi_op->u.waits.req_ids[i]);

	wait_elem->num_completed--;	
	s->pending_waits = wait_elem;
	tw_rand_reverse_unif(lp->rng);

   }
}

/* notify the completed send/receive request to the wait operation. */
static int notify_waits(nw_state* s, tw_bf* bf, tw_lp* lp, nw_message* m, dumpi_req_id completed_req)
{
	int i;
	/* traverse the pending waits list and look what type of wait operations are 
	there. If its just a single wait and the request ID has just been completed, 
	then the network node LP can go on with fetching the next operation from the log.
	If its waitall then wait for all pending requests to complete and then proceed. */
	struct pending_waits* wait_elem = s->pending_waits;
	m->u.rc.matched_op = 0;
	
	if(lp->gid == TRACE)
		printf("\n %lf notify waits req id %d ", tw_now(lp), completed_req);

	if(!wait_elem)
		return 0;

	int op_type = wait_elem->mpi_op->op_type;

	if(op_type == CODES_WK_WAIT)
	{
		if(wait_elem->mpi_op->u.wait.req_id == completed_req)	
		  {
			m->u.rc.saved_wait_time = s->wait_time;
			s->wait_time += (tw_now(lp) - wait_elem->start_time);
                        remove_req_id(&s->completed_reqs, completed_req);
	
			m->u.rc.saved_pending_wait = wait_elem;			
			s->pending_waits = NULL;
			codes_issue_next_event(lp);	
			return 0;
		 }
	}
	else
	if(op_type == CODES_WK_WAITALL)
	{
	   int required_count = wait_elem->mpi_op->u.waits.count;
	  for(i = 0; i < required_count; i++)
	   {
	    if(wait_elem->mpi_op->u.waits.req_ids[i] == completed_req)
		{
			if(lp->gid == TRACE)
				printCompletedQueue(s, lp);
			m->u.rc.matched_op = 1;
			wait_elem->num_completed++;	
		}
	   }
	   
	    if(wait_elem->num_completed == required_count)
	     {
		if(lp->gid == TRACE)
		{
			printf("\n %lf req %d completed %d", tw_now(lp), completed_req, wait_elem->num_completed);
			printCompletedQueue(s, lp);
		}
		m->u.rc.matched_op = 2;
		m->u.rc.saved_wait_time = s->wait_time;
		s->wait_time += (tw_now(lp) - wait_elem->start_time);
		m->u.rc.saved_pending_wait = wait_elem;
		s->pending_waits = NULL; 
		for(i = 0; i < required_count; i++)
			remove_req_id(&s->completed_reqs, wait_elem->mpi_op->u.waits.req_ids[i]);	
		codes_issue_next_event(lp); //wait completed
	    }
       }
	return 0;
}

/* reverse handler of MPI wait operation */
static void codes_exec_mpi_wait_rc(nw_state* s, tw_bf* bf, nw_message* m, tw_lp* lp)
{
    if(s->pending_waits)
     {
    	s->pending_waits = NULL;
	return;
     }
   else
    {
 	mpi_completed_queue_insert_op(&s->completed_reqs, m->op->u.wait.req_id);	
	tw_rand_reverse_unif(lp->rng);		
    }
}

/* execute MPI wait operation */
static void codes_exec_mpi_wait(nw_state* s, tw_bf* bf, nw_message* m, tw_lp* lp)
{
    /* check in the completed receives queue if the request ID has already been completed.*/
    assert(!s->pending_waits);
    dumpi_req_id req_id = m->op->u.wait.req_id;

    struct completed_requests* current = s->completed_reqs;
    while(current) {
        if(current->req_id == req_id) {
            remove_req_id(&s->completed_reqs, req_id);
            m->u.rc.saved_wait_time = s->wait_time;
            codes_issue_next_event(lp);
            return;
        }
        current = current->next;
    }

    /* If not, add the wait operation in the pending 'waits' list. */
    struct pending_waits* wait_op = malloc(sizeof(struct pending_waits));
    wait_op->mpi_op = m->op;
    wait_op->num_completed = 0;
    wait_op->start_time = tw_now(lp);
    s->pending_waits = wait_op;
}

static void codes_exec_mpi_wait_all_rc(nw_state* s, tw_bf* bf, nw_message* m, tw_lp* lp)
{
   if(lp->gid == TRACE)
  {
   printf("\n %lf codes exec mpi waitall reverse %d ", tw_now(lp), m->u.rc.found_match);
   printCompletedQueue(s, lp); 
  } 
  if(m->u.rc.found_match)
    {
   	int i;
	int count = m->op->u.waits.count;
	dumpi_req_id req_id[count];

	for( i = 0; i < count; i++)
	{
		req_id[i] = m->op->u.waits.req_ids[i];
		mpi_completed_queue_insert_op(&s->completed_reqs, req_id[i]);
	}
	tw_rand_reverse_unif(lp->rng);
    }
    else
    {
	struct pending_waits* wait_op = s->pending_waits;
	free(wait_op);
	s->pending_waits = NULL;
	assert(!s->pending_waits);
	if(lp->gid == TRACE)
		printf("\n %lf Nullifying codes waitall ", tw_now(lp));
    }
}
static void codes_exec_mpi_wait_all(nw_state* s, tw_bf* bf, nw_message* m, tw_lp* lp)
{
  //assert(!s->pending_waits);
  int count = m->op->u.waits.count;
  int i, num_completed = 0;
  dumpi_req_id req_id[count];
  struct completed_requests* current = s->completed_reqs;

  /* check number of completed irecvs in the completion queue */ 
  if(lp->gid == TRACE)
    {
  	printf(" \n (%lf) MPI waitall posted %d count", tw_now(lp), m->op->u.waits.count);
	for(i = 0; i < count; i++)
		printf(" %d ", (int)m->op->u.waits.req_ids[i]);
   	printCompletedQueue(s, lp);	 
   }
  while(current) 
   {
	  for(i = 0; i < count; i++)
	   {
	     req_id[i] = m->op->u.waits.req_ids[i];
	     if(req_id[i] == current->req_id)
 		 num_completed++;
   	  }
	 current = current->next;
   }

  if(TRACE== lp->gid)
	  printf("\n %lf Num completed %d count %d ", tw_now(lp), num_completed, count);

  m->u.rc.found_match = 0;
  if(count == num_completed)
  {
	m->u.rc.found_match = 1;
	for( i = 0; i < count; i++)	
		remove_req_id(&s->completed_reqs, req_id[i]);

	codes_issue_next_event(lp);
  }
  else
  {
 	/* If not, add the wait operation in the pending 'waits' list. */
	  struct pending_waits* wait_op = malloc(sizeof(struct pending_waits));
	  wait_op->mpi_op = m->op;  
	  wait_op->num_completed = num_completed;
	  wait_op->start_time = tw_now(lp);
	  s->pending_waits = wait_op;
  }
}

/* request ID is being reused so delete it from the list once the matching is done */
static void remove_req_id(struct completed_requests** mpi_completed_queue, dumpi_req_id req_id)
{
	struct completed_requests* current = *mpi_completed_queue;

	if(!current)
		tw_error(TW_LOC, "\n REQ ID DOES NOT EXIST");
	
       if(current->req_id == req_id)
	{
		*mpi_completed_queue = current->next;
		free(current);
		return;
	}
	
	struct completed_requests* elem;
	while(current->next)
	{
	   elem = current->next;
	   if(elem->req_id == req_id)	
	     {
		current->next = elem->next;
		free(elem);
		return;
	     }
	   current = current->next;	
	}
	return;
}

/* inserts mpi operation in the completed requests queue */
static void mpi_completed_queue_insert_op(struct completed_requests** mpi_completed_queue, dumpi_req_id req_id)
{
	struct completed_requests* reqs = malloc(sizeof(struct completed_requests));
	assert(reqs);

	reqs->req_id = req_id;

	if(!(*mpi_completed_queue))	
	{
			reqs->next = NULL;
			*mpi_completed_queue = reqs;
			return;
	}
	reqs->next = *mpi_completed_queue;
	*mpi_completed_queue = reqs;
	return;
}

/* insert MPI send or receive operation in the queues starting from tail. Unmatched sends go to arrival queue and unmatched receives go to pending receives queues. */
static void mpi_pending_queue_insert_op(struct mpi_queue_ptrs* mpi_queue, struct codes_workload_op* mpi_op)
{
	/* insert mpi operation */
	struct mpi_msgs_queue* elem = malloc(sizeof(struct mpi_msgs_queue));
	assert(elem);

	elem->mpi_op = mpi_op;
     	elem->next = NULL;

	if(!mpi_queue->queue_head)
	  mpi_queue->queue_head = elem;

	if(mpi_queue->queue_tail)
	    mpi_queue->queue_tail->next = elem;
	
        mpi_queue->queue_tail = elem;
	mpi_queue->num_elems++;

	return;
}

/* match the send/recv operations */
static int match_receive(nw_state* s, tw_lp* lp, tw_lpid lpid, struct codes_workload_op* op1, struct codes_workload_op* op2)
{
        assert(op1->op_type == CODES_WK_IRECV || op1->op_type == CODES_WK_RECV);
        assert(op2->op_type == CODES_WK_SEND || op2->op_type == CODES_WK_ISEND);

        if((op1->u.recv.num_bytes >= op2->u.send.num_bytes) &&
                   ((op1->u.recv.tag == op2->u.send.tag) || op1->u.recv.tag == -1) &&
                   ((op1->u.recv.source_rank == op2->u.send.source_rank) || op1->u.recv.source_rank == -1))
                   {
                        if(lp->gid == TRACE)
                           printf("\n op1 rank %d bytes %d ", op1->u.recv.source_rank, op1->u.recv.num_bytes);
                        s->recv_time += tw_now(lp) - op1->sim_start_time;
                        mpi_completed_queue_insert_op(&s->completed_reqs, op1->u.recv.req_id);
                        return 1;
                   }
        return -1;
}

/* used for reverse computation. removes the tail of the queue */
static int mpi_queue_remove_tail(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue)
{
	assert(mpi_queue->queue_tail);
	if(mpi_queue->queue_tail == NULL)
	{
		printf("\n Error! tail not updated ");	
		return 0;
	}
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;

	if(mpi_queue->queue_head == mpi_queue->queue_tail)
	{
		mpi_queue->queue_head = NULL;
		mpi_queue->queue_tail = NULL;
		free(tmp);
		mpi_queue->num_elems--;
		 return 1;
	}

	struct mpi_msgs_queue* elem = mpi_queue->queue_tail;

	while(tmp->next != mpi_queue->queue_tail)
		tmp = tmp->next;

	mpi_queue->queue_tail = tmp;
	mpi_queue->queue_tail->next = NULL;
	mpi_queue->num_elems--;

	free(elem);
	return 1;
}

/* search for a matching mpi operation and remove it from the list. 
 * Record the index in the list from where the element got deleted. 
 * Index is used for inserting the element once again in the queue for reverse computation. */
static int mpi_queue_remove_matching_op(nw_state* s, tw_lp* lp, struct mpi_queue_ptrs* mpi_queue, nw_message * m)
{
       struct codes_workload_op * mpi_op = m->op;
 
	if(mpi_queue->queue_head == NULL)
		return -1;

	/* remove mpi operation */
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;
	int indx = 0;

	/* if head of the list has the required mpi op to be deleted */
	int rcv_val = 0;
	if(mpi_op->op_type == CODES_WK_SEND || mpi_op->op_type == CODES_WK_ISEND)
	  {
		rcv_val = match_receive(s, lp, lp->gid, tmp->mpi_op, mpi_op);
		m->u.rc.saved_matched_req = tmp->mpi_op->u.recv.req_id;  
	 }
	else if(mpi_op->op_type == CODES_WK_RECV || mpi_op->op_type == CODES_WK_IRECV)
	  {
		rcv_val = match_receive(s, lp, lp->gid, mpi_op, tmp->mpi_op);
	  	m->u.rc.saved_matched_req = mpi_op->u.recv.req_id;
	  }
	if(rcv_val >= 0)
	{
		/* TODO: fix RC */
		/*memcpy(&m->u.rc.ptr_match_op, &tmp->mpi_op, sizeof(struct codes_workload_op));*/
		if(mpi_queue->queue_head == mpi_queue->queue_tail)
		   {
			mpi_queue->queue_tail = NULL;
			mpi_queue->queue_head = NULL;
			 free(tmp);
		   }
		 else
		   {
			mpi_queue->queue_head = tmp->next;
			free(tmp);	
		   }
		mpi_queue->num_elems--;
		return indx;
	}

	/* record the index where matching operation has been found */
	struct mpi_msgs_queue* elem;

	while(tmp->next)	
	{
	   indx++;
	   elem = tmp->next;
	   
	    if(mpi_op->op_type == CODES_WK_SEND || mpi_op->op_type == CODES_WK_ISEND)
	     {
		rcv_val = match_receive(s, lp, lp->gid, elem->mpi_op, mpi_op);
	     	m->u.rc.saved_matched_req = elem->mpi_op->u.recv.req_id; 
	     }
	    else if(mpi_op->op_type == CODES_WK_RECV || mpi_op->op_type == CODES_WK_IRECV)
	     {
		rcv_val = match_receive(s, lp, lp->gid, mpi_op, elem->mpi_op);
		m->u.rc.saved_matched_req = mpi_op->u.recv.req_id;
	     }
   	     if(rcv_val >= 0)
		{
		    /* TODO: fix RC */
		    /*memcpy(&m->u.rc.ptr_match_op, &elem->mpi_op, sizeof(struct codes_workload_op));*/
		    if(elem == mpi_queue->queue_tail)
			mpi_queue->queue_tail = tmp;
		    
		    tmp->next = elem->next;

		    free(elem);
		    mpi_queue->num_elems--;
		
		    return indx;
		}
	   tmp = tmp->next;
        }
	return -1;
}
/* Trigger getting next event at LP */
static void codes_issue_next_event(tw_lp* lp)
{
   tw_event *e;
   nw_message* msg;

   tw_stime ts;

   ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
   e = tw_event_new( lp->gid, ts, lp );
   msg = tw_event_data(e);

   msg->msg_type = MPI_OP_GET_NEXT;
   tw_event_send(e);
}

/* Simulate delays between MPI operations */
static void codes_exec_comp_delay(nw_state* s, nw_message* m, tw_lp* lp)
{
	struct codes_workload_op* mpi_op = m->op;
	tw_event* e;
	tw_stime ts;
	nw_message* msg;

        if (disable_delay) {
            ts = 0.0; // no compute time sim
        }
        else {
            s->compute_time += s_to_ns(mpi_op->u.delay.seconds);
            ts = s_to_ns(mpi_op->u.delay.seconds);
        }

	ts += g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
	
	e = tw_event_new( lp->gid, ts , lp );
	msg = tw_event_data(e);
	msg->msg_type = MPI_OP_GET_NEXT;

	tw_event_send(e); 
}

/* reverse computation operation for MPI irecv */
static void codes_exec_mpi_recv_rc(nw_state* s, nw_message* m, tw_lp* lp)
{
	num_bytes_recvd -= m->op->u.recv.num_bytes;
	s->recv_time = m->u.rc.saved_recv_time;
	if(m->u.rc.found_match >= 0)
	  {
		s->recv_time = m->u.rc.saved_recv_time;
		mpi_queue_update(s->arrival_queue, m->u.rc.ptr_match_op, m->u.rc.found_match);
		remove_req_id(&s->completed_reqs, m->op->u.recv.req_id);
		tw_rand_reverse_unif(lp->rng);
	  }
	else if(m->u.rc.found_match < 0)
	    {
		mpi_queue_remove_tail(lp->gid, s->pending_recvs_queue);
		if(m->op->op_type == CODES_WK_IRECV)
			tw_rand_reverse_unif(lp->rng);
	    }
}

/* Execute MPI Irecv operation (non-blocking receive) */ 
static void codes_exec_mpi_recv(nw_state* s, nw_message* m, tw_lp* lp)
{
/* Once an irecv is posted, list of completed sends is checked to find a matching isend.
   If no matching isend is found, the receive operation is queued in the pending queue of
   receive operations. */

    struct codes_jobmap_id lid;
    lid.job = s->app_id;
    find_glp_for_msg(m, &lid);


	m->u.rc.saved_recv_time = s->recv_time;
	struct codes_workload_op* mpi_op = m->op;
	mpi_op->sim_start_time = tw_now(lp);
	num_bytes_recvd += mpi_op->u.recv.num_bytes;

	if(lp->gid == TRACE)
		printf("\n %lf codes exec mpi recv req id %d", tw_now(lp), (int)mpi_op->u.recv.req_id);
	
	dumpi_req_id req_id;
	int found_matching_sends = mpi_queue_remove_matching_op(s, lp, s->arrival_queue, m);
	
	/* save the req id inserted in the completed queue for reverse computation. */
	//m->matched_recv = req_id;

	if(found_matching_sends < 0)
	  {
		m->u.rc.found_match = -1;
		mpi_pending_queue_insert_op(s->pending_recvs_queue, mpi_op);
	
	       /* for mpi irecvs, this is a non-blocking receive so just post it and move on with the trace read. */
		if(mpi_op->op_type == CODES_WK_IRECV)
		   {
			codes_issue_next_event(lp);	
			return;
		   }
		else
			printf("\n CODES MPI RECV OPERATION!!! ");
	  }
	else
	  {
		/*if(lp->gid == TRACE)
			printf("\n Matched after removing: arrival queue num_elems %d ", s->arrival_queue->num_elems);*/
		/* update completed requests list */
		//int count_after = numQueue(s->arrival_queue);
		//assert(count_before == (count_after+1));
	   	m->u.rc.found_match = found_matching_sends;
		codes_issue_next_event(lp); 
	 }
}

/*find the global LP id of source and destination lps of each message*/
void find_glp_for_msg( nw_message *m, struct codes_jobmap_id *jp_id)
{
    jp_id->rank = m->op->u.send.dest_rank;
    int global_dest_rank = codes_jobmap_to_global_id( *jp_id, jobmap_ctx);
    if(jp_id->rank != global_dest_rank)
    {
        m->op->u.send.dest_rank = global_dest_rank;
        m->op->u.recv.dest_rank = global_dest_rank;
    }

    jp_id->rank = m->op->u.send.source_rank;
    int global_src_rank = codes_jobmap_to_global_id(*jp_id, jobmap_ctx);
    if(jp_id->rank != global_src_rank){
        m->op->u.send.source_rank = global_src_rank;
        m->op->u.recv.source_rank = global_src_rank;
    }

}


/* executes MPI send and isend operations */
static void codes_exec_mpi_send(nw_state* s, nw_message * m, tw_lp* lp)
{

    struct codes_jobmap_id lid;
    lid.job = s->app_id;
    find_glp_for_msg(m, &lid);

    struct codes_workload_op * mpi_op = m->op; 
    /* model-net event */
    tw_lpid dest_rank;

	codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, 
	    lp_type_name, &mapping_type_id, annotation, &mapping_rep_id, &mapping_offset);

	if(net_id == DRAGONFLY) /* special handling for the dragonfly case */
	{
		int num_routers, lps_per_rep, factor;
		num_routers = codes_mapping_get_lp_count("MODELNET_GRP", 1,
                  "dragonfly_router", NULL, 1);
	 	lps_per_rep = (2 * num_nw_lps) + num_routers;	
		factor = mpi_op->u.send.dest_rank / num_nw_lps;
		dest_rank = (lps_per_rep * factor) + (mpi_op->u.send.dest_rank % num_nw_lps);	
	}
	else
	{
		/* other cases like torus/simplenet/loggp etc. */
		codes_mapping_get_lp_id(lp_group_name, lp_type_name, NULL, 1,  
	    	  mpi_op->u.send.dest_rank, mapping_offset, &dest_rank);
	}

	num_bytes_sent += mpi_op->u.send.num_bytes;

	nw_message* local_m = malloc(sizeof(nw_message));
	nw_message* remote_m = malloc(sizeof(nw_message));
	assert(local_m && remote_m);

        local_m->u.msg_info.sim_start_time = tw_now(lp);
        local_m->u.msg_info.dest_rank = mpi_op->u.send.dest_rank;
	local_m->u.msg_info.src_rank = mpi_op->u.send.source_rank;
        local_m->u.msg_info.op_type = mpi_op->op_type; 
        local_m->msg_type = MPI_SEND_POSTED;
        local_m->u.msg_info.tag = mpi_op->u.send.tag;
        local_m->u.msg_info.num_bytes = mpi_op->u.send.num_bytes;
        local_m->u.msg_info.req_id = mpi_op->u.send.req_id;

        memcpy(remote_m, local_m, sizeof(nw_message));
	remote_m->msg_type = MPI_SEND_ARRIVED;

	model_net_event(net_id, "test", dest_rank, mpi_op->u.send.num_bytes, 0.0, 
	    sizeof(nw_message), (const void*)remote_m, sizeof(nw_message), (const void*)local_m, lp);

	/*if(TRACE == lp->gid)	
		printf("\n !!! %lf send req id %d dest %d nw_message %d ", tw_now(lp), (int)mpi_op->u.send.req_id, (int)dest_rank, sizeof(nw_message));
	*/
	/* isend executed, now get next MPI operation from the queue */ 
	if(mpi_op->op_type == CODES_WK_ISEND)
	   codes_issue_next_event(lp);
}

/* MPI collective operations */
static void codes_exec_mpi_col(nw_state* s, nw_message* m, tw_lp* lp)
{
	codes_issue_next_event(lp);
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}


static void update_send_completion_queue_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//mpi_queue_remove_matching_op(&s->completed_isend_queue_head, &s->completed_isend_queue_tail, &m->op, SEND);
	if(m->u.msg_info.op_type == CODES_WK_SEND)
		tw_rand_reverse_unif(lp->rng);	

	if(m->u.msg_info.op_type == CODES_WK_ISEND)
	  {
		notify_waits_rc(s, bf, lp, m, m->u.msg_info.req_id);
		remove_req_id(&s->completed_reqs, m->u.msg_info.req_id);
	 }
}

/* completed isends are added in the list */
static void update_send_completion_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	if(TRACE == lp->gid)
		printf("\n %lf isend operation completed req id %d ", tw_now(lp), m->u.msg_info.req_id);
	if(m->u.msg_info.op_type == CODES_WK_ISEND)
	   {	
		mpi_completed_queue_insert_op(&s->completed_reqs, m->u.msg_info.req_id);
	   	notify_waits(s, bf, lp, m, m->u.msg_info.req_id);
	   }  
	
	/* blocking send operation */
	if(m->u.msg_info.op_type == CODES_WK_SEND)
		codes_issue_next_event(lp);	

	 return;
}

/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	s->recv_time = m->u.rc.saved_recv_time;

        codes_local_latency_reverse(lp);

	if(m->u.rc.found_match >= 0)
	{
		// TODO: Modify for recvs
		if(lp->gid == TRACE)
			printf("\n %lf reverse-- update arrival queue req ID %d", tw_now(lp), (int) m->u.rc.saved_matched_req);
		dumpi_req_id req_id = m->u.rc.saved_matched_req;
		notify_waits_rc(s, bf, lp, m, m->u.rc.saved_matched_req);
		//int count = numQueue(s->pending_recvs_queue);
		mpi_queue_update(s->pending_recvs_queue, m->u.rc.ptr_match_op, m->u.rc.found_match);
		remove_req_id(&s->completed_reqs, m->u.rc.saved_matched_req);
	
		/*if(lp->gid == TRACE)
			printf("\n Reverse: after adding pending recvs queue %d ", s->pending_recvs_queue->num_elems);*/
	}
	else if(m->u.rc.found_match < 0)
	{
		mpi_queue_remove_tail(lp->gid, s->arrival_queue);	
		/*if(lp->gid == TRACE)
			printf("\n Reverse: after removing arrivals queue %d ", s->arrival_queue->num_elems);*/
	}
}

/* once an isend operation arrives, the pending receives queue is checked to find out if there is a irecv that has already been posted. If no isend has been posted, */
static void update_arrival_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//int count_before = numQueue(s->pending_recvs_queue);
	int is_blocking = 0; /* checks if the recv operation was blocking or not */

	m->u.rc.saved_recv_time = s->recv_time;

        // send a callback to the sender to increment times
        tw_event *e_callback =
            tw_event_new(rank_to_lpid(m->u.msg_info.src_rank),
                    codes_local_latency(lp), lp);
        nw_message *m_callback = tw_event_data(e_callback);
        m_callback->msg_type = MPI_SEND_ARRIVED_CB;
        m_callback->u.msg_info.msg_send_time = tw_now(lp) - m->u.msg_info.sim_start_time;
        tw_event_send(e_callback);

        /*NOTE: this computes send time with respect to the receiver, not the
         * sender
         * s->send_time += tw_now(lp) - m->u.msg_info.sim_start_time; */
	dumpi_req_id req_id = -1;

        /* Now reconstruct the mpi op */
        struct codes_workload_op * arrived_op = (struct codes_workload_op *) malloc(sizeof(struct codes_workload_op));
        arrived_op->sim_start_time = m->u.msg_info.sim_start_time;
        arrived_op->op_type = m->u.msg_info.op_type;
        arrived_op->u.send.source_rank = m->u.msg_info.src_rank;
        arrived_op->u.send.dest_rank = m->u.msg_info.dest_rank;
        arrived_op->u.send.num_bytes = m->u.msg_info.num_bytes;
        arrived_op->u.send.tag = m->u.msg_info.tag;
        arrived_op->u.send.req_id = m->u.msg_info.req_id;
        m->op = arrived_op;

	int found_matching_recv = mpi_queue_remove_matching_op(s, lp, s->pending_recvs_queue, m);

	if(TRACE == lp->gid)
		printf("\n %lf update arrival queue req id %d %d", tw_now(lp), arrived_op->u.send.req_id, m->op->u.send.source_rank);
	if(found_matching_recv < 0)
	 {
		m->u.rc.found_match = -1;
		mpi_pending_queue_insert_op(s->arrival_queue, m->op);
	}
	else
	  {
		m->u.rc.found_match = found_matching_recv;
	   	notify_waits(s, bf, lp, m, m->u.rc.saved_matched_req);
	  }
}

static void update_message_time(
        nw_state * s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp)
{
    m->u.rc.saved_send_time = s->send_time;
    s->send_time += m->u.msg_info.msg_send_time;
}

static void update_message_time_rc(
        nw_state * s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp)
{
    s->send_time = m->u.rc.saved_send_time;
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params = NULL;
   dumpi_trace_params params_d;
  
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, lp_type_name, 
	&mapping_type_id, annotation, &mapping_rep_id, &mapping_offset);
  
   memset(s, 0, sizeof(*s));
   s->nw_id = (mapping_rep_id * num_nw_lps) + mapping_offset;
   s->completed_reqs = NULL;
   s->pending_waits = NULL;

   struct codes_jobmap_id lid;
   lid = codes_jobmap_to_local_id(s->nw_id, jobmap_ctx);

   //In this case, the LP will not generate any workload related events
   if(lid.job == -1)
   {
       // printf("network LP nw id %d not generating events\n", (int)s->nw_id);
       s->app_id = -1;
       s->local_rank = -1;
       return;
   }

   if(strcmp(workload_type, "dumpi") == 0)
   {
       strcpy(params_d.file_name, file_name_of_job[lid.job]);
       params_d.num_net_traces = num_traces_of_job[lid.job];
       params = (char*)&params_d;
       s->app_id = lid.job;
       s->local_rank = lid.rank;
    //printf("lp global id: %llu, file name: %s, num traces: %d, app id: %d, local id: %d\n", s->nw_id, params_d.file_name, params_d.num_net_traces, s->app_id, s->local_rank);
   }
   wrkld_id = codes_workload_load("dumpi-trace-workload", params, s->app_id, s->local_rank);


   s->arrival_queue = queue_init(); 
   s->pending_recvs_queue = queue_init();

   /* clock starts when the first event is processed */
   s->start_time = tw_now(lp);
   codes_issue_next_event(lp);

   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	*(int *)bf = (int)0;
	switch(m->msg_type)
	{
		case MPI_SEND_POSTED:
			update_send_completion_queue(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED:
			update_arrival_queue(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED_CB:
			update_message_time(s, bf, m, lp);
		break;

		case MPI_OP_GET_NEXT:
			get_next_mpi_operation(s, bf, m, lp);	
		break; 
	}
}

static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	codes_workload_get_next_rc(wrkld_id, s->app_id, s->local_rank, m->op);
	if(m->op->op_type == CODES_WK_END)
		return;

	switch(m->op->op_type)
	{
		case CODES_WK_SEND:
		case CODES_WK_ISEND:
		{
			if(lp->gid == TRACE)
				printf("\n %lf reverse send req %d ", tw_now(lp), (int)m->op->u.send.req_id);
			model_net_event_rc(net_id, lp, m->op->u.send.num_bytes);
			if(m->op->op_type == CODES_WK_ISEND)
				tw_rand_reverse_unif(lp->rng);	
			s->num_sends--;
			num_bytes_sent -= m->op->u.send.num_bytes;
		}
		break;

		case CODES_WK_IRECV:
		case CODES_WK_RECV:
		{
			codes_exec_mpi_recv_rc(s, m, lp);
			s->num_recvs--;
		}
		break;
		case CODES_WK_DELAY:
		{
			s->num_delays--;
                        if (!disable_delay) {
                            tw_rand_reverse_unif(lp->rng);
                            s->compute_time -= s_to_ns(m->op->u.delay.seconds);
                        }
		}
		break;
		case CODES_WK_BCAST:
		case CODES_WK_ALLGATHER:
		case CODES_WK_ALLGATHERV:
		case CODES_WK_ALLTOALL:
		case CODES_WK_ALLTOALLV:
		case CODES_WK_REDUCE:
		case CODES_WK_ALLREDUCE:
		case CODES_WK_COL:
		{
			s->num_cols--;
			tw_rand_reverse_unif(lp->rng);
		}
		break;
	
		case CODES_WK_WAIT:
		{
			s->num_wait--;
			codes_exec_mpi_wait_rc(s, bf, m, lp);
		}
		break;
		case CODES_WK_WAITALL:
		{
			s->num_waitall--;
			codes_exec_mpi_wait_all_rc(s, bf, m, lp);
		}
		break;
		case CODES_WK_WAITSOME:
		case CODES_WK_WAITANY:
		{
			s->num_waitsome--;
			tw_rand_reverse_unif(lp->rng);
		}
		break;
		default:
			printf("\n Invalid op type %d ", m->op->op_type);
	}
}

static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
		struct codes_workload_op mpi_op;
    		codes_workload_get_next(wrkld_id, s->app_id, s->local_rank, &mpi_op);
		m->op = malloc(sizeof(struct codes_workload_op));
                memcpy(m->op, &mpi_op, sizeof(struct codes_workload_op));

    		if(mpi_op.op_type == CODES_WK_END)
    	 	{
			s->elapsed_time = tw_now(lp) - s->start_time;
			return;
     		}
		switch(mpi_op.op_type)
		{
			case CODES_WK_SEND:
			case CODES_WK_ISEND:
			 {
				s->num_sends++;
				codes_exec_mpi_send(s, m, lp);
			 }
			break;
	
			case CODES_WK_RECV:
			case CODES_WK_IRECV:
			  {
				s->num_recvs++;
				codes_exec_mpi_recv(s, m, lp);
			  }
			break;

			case CODES_WK_DELAY:
			  {
				s->num_delays++;
				codes_exec_comp_delay(s, m, lp);
			  }
			break;

			case CODES_WK_BCAST:
			case CODES_WK_ALLGATHER:
			case CODES_WK_ALLGATHERV:
			case CODES_WK_ALLTOALL:
			case CODES_WK_ALLTOALLV:
			case CODES_WK_REDUCE:
			case CODES_WK_ALLREDUCE:
			case CODES_WK_COL:
            case CODES_WK_WAITANY:
			  {
				s->num_cols++;
				codes_exec_mpi_col(s, m, lp);
			  }
			break;
			case CODES_WK_WAIT:
			{
				s->num_wait++;
				codes_exec_mpi_wait(s, bf, m, lp);	
			}
			break;
            case CODES_WK_WAITSOME:
			case CODES_WK_WAITALL:
			{
				s->num_waitall++;
				codes_exec_mpi_wait_all(s, bf, m, lp);
			}
			break;
			default:
				printf("\n Invalid op type %d ", m->op->op_type);
		}
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{
    if(s->app_id != -1)
	{
		int count_irecv = numQueue(s->pending_recvs_queue);
        int count_isend = numQueue(s->arrival_queue);

		printf("\n %ld App %d %d %lf %lf %lf", 
			lp->gid, s->app_id, s->local_rank, s->send_time, s->wait_time, s->elapsed_time);
		if(lp->gid == TRACE)
		{
		   printQueue(lp->gid, s->pending_recvs_queue, "irecv ");
		  printQueue(lp->gid, s->arrival_queue, "isend");
	        }

		if(s->elapsed_time - s->compute_time > max_comm_time)
			max_comm_time = s->elapsed_time - s->compute_time;
		
		if(s->elapsed_time > max_time )
			max_time = s->elapsed_time;

		if(s->wait_time > max_wait_time)
			max_wait_time = s->wait_time;

		if(s->send_time > max_send_time)
			max_send_time = s->send_time;

		if(s->recv_time > max_recv_time)
			max_recv_time = s->recv_time;

		avg_time += s->elapsed_time;
		avg_comm_time += (s->elapsed_time - s->compute_time);
		avg_wait_time += s->wait_time;
		avg_send_time += s->send_time;
		 avg_recv_time += s->recv_time;

		//printf("\n LP %ld Time spent in communication %llu ", lp->gid, total_time - s->compute_time);
		free(s->arrival_queue);
		free(s->pending_recvs_queue);
	}
}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	switch(m->msg_type)
	{
		case MPI_SEND_POSTED:
			update_send_completion_queue_rc(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED:
			update_arrival_queue_rc(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED_CB:
			update_message_time_rc(s, bf, m, lp);
		break;

		case MPI_OP_GET_NEXT:
			get_next_mpi_operation_rc(s, bf, m, lp);
		break;
	}
}

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Network workload test"),
    TWOPT_CHAR("workload_type", workload_type, "workload type (either \"scalatrace\" or \"dumpi\")"),
    TWOPT_CHAR("workloads_conf_file", workloads_conf_file, "workload file name"),
    TWOPT_CHAR("alloc_file", alloc_file, "allocation file name"),
	//TWOPT_CHAR("workload_file", workload_file, "workload file name"),
	//TWOPT_UINT("num_net_traces", num_net_traces, "number of network traces"),
    TWOPT_UINT("disable_compute", disable_delay, "disable compute simulation"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
	TWOPT_END()
};

tw_lptype nw_lp = {
    (init_f) nw_test_init,
    (pre_run_f) NULL,
    (event_f) nw_test_event_handler,
    (revent_f) nw_test_event_handler_rc,
    (final_f) nw_test_finalize,
    (map_f) codes_mapping,
    sizeof(nw_state)
};

const tw_lptype* nw_get_lp_type()
{
            return(&nw_lp);
}

static void nw_add_lp_type()
{
  lp_type_register("nw-lp", nw_get_lp_type());
}

int main( int argc, char** argv )
{
  int rank, nprocs;
  int num_nets;
  int* net_ids;

  g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

  workload_type[0]='\0';
  tw_opt_add(app_opt);
  tw_init(&argc, &argv);


  if(strlen(workloads_conf_file) == 0)
  {
      if(tw_ismaster())
          printf("\n Usage: mpirun -np n ./model-net-dumpi-traces-dump --sync=1/2/3 --workload_type=type --workloads_conf_file = workloads.conf");
      tw_end();
      return -1;
  }


  FILE *name_file = fopen(workloads_conf_file, "r");
  if (!name_file)
  {
      printf("Coudld not open file %s \n", workloads_conf_file);
      exit(1);
  }
  else{
      int i=0;
      char ref = '\n';
      while(!feof(name_file))
      {
          ref = fscanf(name_file, "%d %s", &num_traces_of_job[i], file_name_of_job[i]);
          if(ref!=EOF){
              printf("\n%d traces of app %s \n", num_traces_of_job[i], file_name_of_job[i]);
              i++;
          }
      }
      fclose(name_file);

  }

  if(strlen(alloc_file) == 0)
  {
      if(tw_ismaster())
          printf("\n Usage: mpirun -np n ./codes-nw-test --sync=1/2/3 --workload_type=type --workloads_conf_file = workloads.conf --alloc_file=alloc.conf");
      tw_end();
      return -1;
  }


  jobmap_p.alloc_file = alloc_file;
  jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);


    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

   configuration_load(argv[2], MPI_COMM_WORLD, &config);

   nw_add_lp_type();
   model_net_register();

   net_ids = model_net_configure(&num_nets);
   assert(num_nets == 1);
   net_id = *net_ids;
   free(net_ids);


   codes_mapping_setup();

   num_net_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   
   num_nw_lps = codes_mapping_get_lp_count("MODELNET_GRP", 1, 
			"nw-lp", NULL, 1);	
   tw_run();

    long long total_bytes_sent, total_bytes_recvd;
    double max_run_time, avg_run_time;
   double max_comm_run_time, avg_comm_run_time;
    double total_avg_send_time, total_max_send_time;
     double total_avg_wait_time, total_max_wait_time;
     double total_avg_recv_time, total_max_recv_time;
	
    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_comm_time, &max_comm_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_time, &avg_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   MPI_Reduce(&avg_recv_time, &total_avg_recv_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_comm_time, &avg_comm_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_wait_time, &total_max_wait_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
   MPI_Reduce(&max_send_time, &total_max_send_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
   MPI_Reduce(&max_recv_time, &total_max_recv_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
   MPI_Reduce(&avg_wait_time, &total_avg_wait_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_send_time, &total_avg_send_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   if(!g_tw_mynode)
	printf("\n Total bytes sent %lld recvd %lld \n max runtime %lf ns avg runtime %lf \n max comm time %lf avg comm time %lf \n max send time %lf avg send time %lf \n max recv time %lf avg recv time %lf \n max wait time %lf avg wait time %lf \n", total_bytes_sent, total_bytes_recvd, 
			max_run_time, avg_run_time/num_net_traces,
			max_comm_run_time, avg_comm_run_time/num_net_traces,
			total_max_send_time, total_avg_send_time/num_net_traces,
			total_max_recv_time, total_avg_recv_time/num_net_traces,
			total_max_wait_time, total_avg_wait_time/num_net_traces);
   tw_end();
  
  return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
