/*
 * benchmark_graph_algo.c
 *
 *  Created on: December 4, 2013
 *      Author: aousterh
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "algo_config.h"
#include "fp_ring.h"
#include "generate_requests.h"
#include "admissible.h"
#include "path_selection.h"
#include "platform.h"
#include "rdtsc.h"  // For timing

#define NUM_FRACTIONS_A 11
#define NUM_SIZES_A 1
#define NUM_FRACTIONS_P 11
#define NUM_CAPACITIES_P 4
#define NUM_RACKS_P 4
#define NUM_NODES_P 1024
#define PROCESSOR_SPEED 2.8
#define BIN_MEMPOOL_SIZE 2048
#define ADMITTED_TRAFFIC_MEMPOOL_SIZE	(51*1000)
#define ADMITTED_OUT_RING_LOG_SIZE		16
#define READY_PARTITIONS_Q_SIZE                 2

const double admissible_fractions [NUM_FRACTIONS_A] =
    {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 0.99};
const uint32_t admissible_sizes [NUM_SIZES_A] =
    {256, /*2048, 1024, 512, 128, 64, 32, 16*/};
const double path_fractions [NUM_FRACTIONS_P] =
    {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 0.99};
const uint16_t path_capacities [NUM_CAPACITIES_P] =
    {4, 8, 16, 32};  // inter-rack capacities (32 machines per rack)
const uint8_t path_num_racks [NUM_RACKS_P] =
    {32, 16, 8, 4};

enum benchmark_type {
    ADMISSIBLE,
    PATH_SELECTION_OVERSUBSCRIPTION,
    PATH_SELECTION_RACKS
};

// Runs one experiment. Returns the number of packets admitted.
uint32_t run_experiment(struct request_info *requests, uint32_t start_time, uint32_t end_time,
                        uint32_t num_requests, struct admissible_state *status,
                        struct request_info **next_request,
                        uint32_t *per_batch_times)
{
    struct admitted_traffic *admitted;
    struct fp_ring *queue_tmp;

    uint32_t b, start_b;
    uint8_t i;
    uint32_t num_admitted = 0;
    struct request_info *current_request = requests;

    assert(requests != NULL);

    uint64_t prev_time = current_time();
    start_b = start_time >> BATCH_SHIFT;

    for (b = (start_time >> BATCH_SHIFT); b < (end_time >> BATCH_SHIFT); b++) {
        // Issue all new requests for this batch
        while ((current_request->timeslot >> BATCH_SHIFT) == (b % (65536 >> BATCH_SHIFT)) &&
               current_request < requests + num_requests) {
        	add_backlog(status, current_request->src, current_request->dst,
                        current_request->backlog);
            current_request++;
        }
        flush_backlog(status);
 
        // Get admissible traffic
        get_admissible_traffic(status, 0, 0, 1, 0);
        handle_spent_demands(status);

        for (i = 0; i < ADMITTED_PER_BATCH; i++) {
        	/* get admitted traffic */
                fp_ring_dequeue(get_q_admitted_out(status), (void **)&admitted);
        	/* update statistics */
        	num_admitted += admitted->size;
        	/* return admitted traffic to core */
                fp_mempool_put(get_admitted_traffic_mempool(status), admitted);
        }

        // Record per-batch time
        uint64_t time_now = current_time();
        assert(time_now - prev_time < (0x1ULL << 32));
        per_batch_times[b - start_b] = time_now - prev_time;
        prev_time = time_now;
    }

    *next_request = current_request;

	return num_admitted;
}

// Runs the admissible algorithm for many timeslots, saving the admitted traffic for
// further benchmarking
void run_admissible(struct request_info *requests, uint32_t start_time, uint32_t end_time,
                    uint32_t num_requests, struct admissible_state *status,
                    struct request_info **next_request)
{
    struct admitted_traffic *admitted;
    struct fp_ring *queue_tmp;

    uint32_t b;
    uint8_t i;
    struct request_info *current_request = requests;
    uint32_t admitted_index = BATCH_SIZE;  // already BATCH_SIZE admitted structs in admitted_batch

    assert(requests != NULL);

    for (b = (start_time >> BATCH_SHIFT); b < (end_time >> BATCH_SHIFT); b++) {
        // Issue all new requests for this batch
        while ((current_request->timeslot >> BATCH_SHIFT) == (b % (65536 >> BATCH_SHIFT)) &&
               current_request < requests + num_requests) {
            add_backlog(status, current_request->src, current_request->dst,
                        current_request->backlog);
            current_request++;
        }
        flush_backlog(status);

        // Get admissible traffic
        get_admissible_traffic(status, 0, 0, 1, 0);
        handle_spent_demands(status);
    }

    *next_request = current_request;
}

void print_usage(char **argv) {
    printf("usage: %s benchmark_type\n", argv[0]);
    printf("\tbenchmark_type=0 for admissible traffic benchmark, benchmark_type=1 for path selection benchmark (vary oversubscription ratio), benchmark_type=2 for path selection (vary #racks)\n");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        print_usage(argv);
        return -1;
    }

    int type;
    sscanf(argv[1], "%d", &type);
    enum benchmark_type benchmark_type;
    if (type == 0)
        benchmark_type = ADMISSIBLE;
    else if (type == 1)
        benchmark_type = PATH_SELECTION_OVERSUBSCRIPTION;
    else if (type == 2)
        benchmark_type = PATH_SELECTION_RACKS;
    else {
        print_usage(argv);
        return -1;
    }

    // keep both durations an even number of batches so that bin pointers return to queue_0
    uint32_t warm_up_duration = ((10000 + 127) / 128) * 128;
    uint32_t duration = warm_up_duration + ((50000 + 127) / 128) * 128;
    double mean = 10; // Mean request size and inter-arrival time

    /* sanity checks */
#if (ALGO_N_CORES != 1)
#error "benchmark only supports ALGO_N_CORES == 1"
#endif

    if (ADMITTED_TRAFFIC_MEMPOOL_SIZE < duration - warm_up_duration) {
    	printf("need at least %u elements in admitted_traffic to run experiments, got %u\n",
    			duration- warm_up_duration, ADMITTED_TRAFFIC_MEMPOOL_SIZE);
    	exit(-1);
    }
    if ((1 << ADMITTED_OUT_RING_LOG_SIZE) <= duration - warm_up_duration) {
    	printf("need at least %u elements in q_admitted to run experiments, got %u\n",
    			duration- warm_up_duration, 1 << ADMITTED_OUT_RING_LOG_SIZE);
    	exit(-1);
    }

    uint16_t i, j, k;

    // Each experiment tries out a different combination of target network utilization
    // and number of nodes
    const double *fractions;
    const uint32_t *sizes;
    const uint16_t *capacities;
    const uint8_t *racks;
    uint8_t num_fractions;
    uint8_t num_parameter_2;
    if (benchmark_type == ADMISSIBLE) {
        // init fractions
        num_fractions = NUM_FRACTIONS_A;
        fractions = admissible_fractions;

        // init parameter 2 - sizes
        num_parameter_2 = NUM_SIZES_A;
        sizes = admissible_sizes;
    } else if (benchmark_type == PATH_SELECTION_OVERSUBSCRIPTION) {
        // init fractions
        num_fractions = NUM_FRACTIONS_P;
        fractions = path_fractions;

        // init parameter 2 - inter-rack capacities
        num_parameter_2 = NUM_CAPACITIES_P;
        capacities = path_capacities;
    } else {
        // init fractions
        num_fractions = NUM_FRACTIONS_P;
        fractions = path_fractions;

        // init parameter 2 - number of racks
        num_parameter_2 = NUM_RACKS_P;
        racks = path_num_racks;
    }

    // Data structures
    struct admissible_state *status;
    struct fp_ring *q_bin;
    struct fp_ring *q_head;
    struct fp_ring *q_admitted_out;
    struct fp_ring *q_spent;
    struct fp_mempool *bin_mempool;
    struct fp_mempool *admitted_traffic_mempool;
    struct fp_ring *q_new_demands[NUM_BIN_RINGS];
    struct fp_ring *q_ready_partitions[NUM_BIN_RINGS];

    /* init queues */
    q_bin = fp_ring_create(2 * FP_NODES_SHIFT);
    q_head = fp_ring_create(2 * FP_NODES_SHIFT);
    q_admitted_out = fp_ring_create(ADMITTED_OUT_RING_LOG_SIZE);
    q_spent = fp_ring_create(2 * FP_NODES_SHIFT);
    bin_mempool = fp_mempool_create(BIN_MEMPOOL_SIZE, bin_num_bytes(SMALL_BIN_SIZE));
    admitted_traffic_mempool = fp_mempool_create(ADMITTED_TRAFFIC_MEMPOOL_SIZE,
    		sizeof(struct admitted_traffic));
    for (i = 0; i < NUM_BIN_RINGS; i++) {
            q_new_demands[i] = fp_ring_create(BIN_RING_SHIFT);
            if (!q_new_demands[i]) exit(-1);
            q_ready_partitions[i] = fp_ring_create(READY_PARTITIONS_Q_SIZE);
            if (!q_ready_partitions[i]) exit(-1);
    }
    if (!q_bin) exit(-1);
    if (!q_head) exit(-1);
    if (!q_admitted_out) exit(-1);
    if (!q_spent) exit(-1);
    if (!bin_mempool) exit(-1);
    if (!admitted_traffic_mempool) exit(-1);

    /* init global status */
    status = create_admissible_state(false, 0, 0, 0, q_head, q_admitted_out,
                                     q_spent, bin_mempool,
                                     admitted_traffic_mempool,
                                     &q_bin, &q_new_demands[0],
                                     &q_ready_partitions[0]);
    if (status == NULL) {
        printf("Error initializing admissible_status!\n");
        exit(-1);
    }

    /* allocate space to record times */
    uint16_t num_batches = (duration - warm_up_duration) / BATCH_SIZE;
    uint32_t *per_batch_times = malloc(sizeof(uint64_t) * num_batches);
    assert(per_batch_times != NULL);
    
    uint16_t num_timeslots = duration - warm_up_duration;
    uint32_t *per_timeslot_times = malloc(sizeof(uint64_t) * num_timeslots);
    assert(per_timeslot_times != NULL);

    /* allocate space to record num admitted */
    uint16_t *per_timeslot_num_admitted = malloc(sizeof(uint16_t) * num_timeslots);
    assert(per_timeslot_num_admitted != NULL);

    if (benchmark_type == ADMISSIBLE)
        printf("target_utilization, nodes, time, observed_utilization, time/utilzn\n");
    else if (benchmark_type == PATH_SELECTION_OVERSUBSCRIPTION)
        printf("target_utilization, oversubscription_ratio, time, observed_utilization, time/utilzn, num_admitted\n"); 
    else
        printf("target_utilization, num_racks, time, observed_utilization, time/utilzn, num_admitted\n"); 

    for (i = 0; i < num_fractions; i++) {

        for (j = 0; j < num_parameter_2; j++) {
            double fraction = fractions[i];
            uint32_t num_nodes;
            uint8_t num_racks;
            uint16_t inter_rack_capacity;

            // Initialize data structures
            if (benchmark_type == ADMISSIBLE) {
                num_nodes = sizes[j];
                reset_admissible_state(status, false, 0, 0, num_nodes);
            }
            else if (benchmark_type == PATH_SELECTION_OVERSUBSCRIPTION) {
                num_nodes = NUM_NODES_P;
                inter_rack_capacity = capacities[j];
                reset_admissible_state(status, true, inter_rack_capacity, 0,
                                       num_nodes);
                fraction = fraction * ((double) inter_rack_capacity) / MAX_NODES_PER_RACK;
            } else if (benchmark_type == PATH_SELECTION_RACKS) {
                num_racks = racks[j];
                num_nodes = MAX_NODES_PER_RACK * num_racks;
                inter_rack_capacity = MAX_NODES_PER_RACK;
                reset_admissible_state(status, false, 0, 0, num_nodes);
            }

            struct bin *b;
            while (fp_ring_dequeue(q_bin, (void **)&b) == 0) {
            	if (b != NULL)
                        fp_mempool_put(bin_mempool, b);
            }

            // Allocate enough space for new requests
            // (this is sufficient for <= 1 request per node per timeslot)
            uint32_t max_requests = duration * num_nodes;
            struct request_info *requests = malloc(max_requests * sizeof(struct request_info));

            // Generate new requests
            uint32_t num_requests = generate_requests_poisson(requests, max_requests, num_nodes,
                                                              duration, fraction, mean);

            // Issue/process some requests. This is a warm-up period so that there are pending
            // requests once we start timing
            struct request_info *next_request;
            run_experiment(requests, 0, warm_up_duration, num_requests,
                           status, &next_request, per_batch_times);
   
            if (benchmark_type == ADMISSIBLE) {
                // Start timining
                uint64_t start_time = current_time();

                // Run the experiment
                uint32_t num_admitted = run_experiment(next_request, warm_up_duration, duration,
                                                       num_requests - (next_request - requests),
                                                       status, &next_request, per_batch_times);
                uint64_t end_time = current_time();

                double utilzn = ((double) num_admitted) / ((duration - warm_up_duration) * num_nodes);
                double time_per_experiment = (end_time - start_time)/ (PROCESSOR_SPEED * 1000 * num_batches * BATCH_SIZE);
				printf("%f, %d, %f, %f, %f\n", fraction, num_nodes, time_per_experiment,
					   utilzn, time_per_experiment / utilzn);


                // Print stats - percent of network capacity utilized and computation time
                // per admitted timeslot (in microseconds) for different numbers of nodes
                uint16_t b;
                for (b = 0; b < num_batches; b++) {
                    double time_per_experiment = per_batch_times[b] / (PROCESSOR_SPEED * 1000 * BATCH_SIZE);

//                    printf("%f, %d, %f, %f, %f\n", fraction, num_nodes, time_per_experiment,
//                           utilzn, time_per_experiment / utilzn);
                }
            }
            else if (benchmark_type == PATH_SELECTION_OVERSUBSCRIPTION ||
                     benchmark_type == PATH_SELECTION_RACKS) {
                // Run the admissible algorithm to generate admitted traffic
                run_admissible(next_request, warm_up_duration, duration,
                                                       num_requests - (next_request - requests),
                                                       status, &next_request);

                // Start timining
                uint64_t start_time = current_time();

                uint32_t num_admitted = 0;
                uint64_t prev_time = current_time();
                for (k = 0; k < duration - warm_up_duration; k++) {
                    struct admitted_traffic *admitted;

                    /* get admitted traffic */
                    fp_ring_dequeue(get_q_admitted_out(status), (void **)&admitted);
                    /* update statistics */
                    num_admitted += admitted->size;

                    /* supply a new admitted traffic to core */
                    select_paths(admitted, num_nodes / MAX_NODES_PER_RACK);
                    
                    // Record time
                    uint64_t time_now = current_time();
                    assert(time_now - prev_time < (0x1ULL << 32));
                    per_timeslot_times[k] = time_now - prev_time;
                    prev_time = time_now;

                    // Record num admitted
                    per_timeslot_num_admitted[k] = admitted->size;

                    /* free back the admitted_traffic */
                    fp_mempool_put(get_admitted_traffic_mempool(status), admitted);
                }

                uint64_t end_time = current_time();
                double time_per_experiment = (end_time - start_time) / (PROCESSOR_SPEED * 1000 *
                                                                        (duration - warm_up_duration));
                // utilization of inter-rack links
                uint16_t max_capacity_per_timeslot = inter_rack_capacity * num_nodes / MAX_NODES_PER_RACK;
                double utilzn = ((double) num_admitted) / ((duration - warm_up_duration) * max_capacity_per_timeslot);

                if (benchmark_type == PATH_SELECTION_OVERSUBSCRIPTION) {
                    uint16_t oversubscription_ratio = MAX_NODES_PER_RACK / inter_rack_capacity;

                    // Print stats - percent of network capacity utilized and computation time
                    // per admitted timeslot (in microseconds) for different numbers of nodes
                    uint16_t t;
                    for (t = 0; t < num_timeslots; t++) {
                        double time_per_experiment = per_timeslot_times[t] / (PROCESSOR_SPEED * 1000);
                        printf("%f, %d, %f, %f, %f, %d\n", fraction, oversubscription_ratio, time_per_experiment,
                               utilzn, time_per_experiment / utilzn, per_timeslot_num_admitted[t]);
                    }
                } else {
                    // Print stats - percent of network capacity utilized and computation time
                    // per admitted timeslot (in microseconds) for different numbers of nodes
                    uint16_t t;
                    for (t = 0; t < num_timeslots; t++) {
                        double time_per_experiment = per_timeslot_times[t] / (PROCESSOR_SPEED * 1000);
                        printf("%f, %d, %f, %f, %f, %d\n", fraction, num_racks, time_per_experiment,
                               utilzn, time_per_experiment / utilzn, per_timeslot_num_admitted[t]);
                    }
                }
            }
        }
    }

	/* TODO: memory to free up, but won't worry about it now */
    free(status);
    free(per_batch_times);
    free(per_timeslot_times);
    free(per_timeslot_num_admitted);
}
