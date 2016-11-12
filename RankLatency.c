/*
 * RankLatency.c
 *
 *  Created on: Jun 15, 2015
 *      Author: Angen Zheng
 */

#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <string.h>
#include <hwloc.h>

#include "RankLatency.h"

#define MAX_MSG_SIZE (1<<17)
#define SKIP_LARGE  10
#define LOOP_LARGE  100
#define LARGE_MESSAGE_SIZE 4096

#define SKIP 100		//1000
#define LOOP 1000		//10000

static float s_buf_original[MAX_MSG_SIZE];
static float r_buf_original[MAX_MSG_SIZE];

static double pingpongTest(MPI_Comm comm, int myRank, int src, int dest) {
	int i;
	int size;
	double cost = 0;
	char handshake = 'a';
	float *s_buf = s_buf_original;
	float *r_buf = r_buf_original;

	int msgLoop = LOOP;
	int msgSkip = SKIP;
	for (size = 64; size <= MAX_MSG_SIZE; size *= 2) {
		if (size > LARGE_MESSAGE_SIZE) {
			msgLoop = LOOP_LARGE;
			msgSkip = SKIP_LARGE;
		}

		MPI_Request req;
		if (myRank == src) {
			double st;
			MPI_Recv(&handshake, 1, MPI_CHAR, dest, 1, comm, MPI_STATUS_IGNORE); //handshaking
			for (i = 0; i < msgLoop + msgSkip; i++) {
				if(i == msgSkip){
					st = MPI_Wtime();
				}
//				MPI_Send(s_buf, size, MPI_FLOAT, dest, 1, comm);
				MPI_Isend(s_buf, size, MPI_FLOAT, dest, 1, comm, &req);
				MPI_Request_free(&req);
				MPI_Recv(r_buf, size, MPI_FLOAT, dest, 2, comm, MPI_STATUS_IGNORE);
			}
			double rt = (MPI_Wtime() - st) * 1000 * 1000 * 1000/ size / msgLoop;
			cost += rt;
		} else if (myRank == dest) {
			MPI_Send(&handshake, 1, MPI_CHAR, src, 1, comm); //handshaking
			for (i = 0; i < msgLoop + msgSkip; i++) {
				MPI_Recv(r_buf, size, MPI_FLOAT, src, 1, comm, MPI_STATUS_IGNORE);
//				MPI_Send(s_buf, size, MPI_FLOAT, src, 2, comm);
				MPI_Isend(s_buf, size, MPI_FLOAT, src, 2, comm, &req);
				MPI_Request_free(&req);
			}
		}
	}
	return cost ;
}

static float* _getRankCommCost(MPI_Comm comm) {
	int myRank, numProcs;
	MPI_Comm_rank(comm, &myRank);
	MPI_Comm_size(comm, &numProcs);

	int src, dest;
	float *_rankCommCost = (float *) calloc(numProcs * numProcs, sizeof(float));
	for (src = 0; src < numProcs; src++) {
		for (dest = src + 1; dest < numProcs; dest++) {
			if (myRank == src || myRank == dest) {
				double cost = pingpongTest(comm, myRank, src, dest);
				if (myRank == src) {
					_rankCommCost[src * numProcs + dest] = cost;
					_rankCommCost[dest * numProcs + src] = cost;
				}
			}
			MPI_Barrier(comm); //ensure that each time only one rank is profiling
		}
	}
	MPI_Allreduce(MPI_IN_PLACE, _rankCommCost, numProcs * numProcs, MPI_FLOAT, MPI_MAX, comm);
	return _rankCommCost;
}

//grouping ranks bind to the same machine into a single group
void hwloc_rank_split_by_mach(MPI_Comm comm, hwloc_topology_t mach_topo, int myRank, int numRanks, MPI_Comm *intraNodeComm){
	char hostname[64];
	hwloc_obj_t machine = hwloc_get_obj_by_type(mach_topo, HWLOC_OBJ_MACHINE, 0);
	char *hostnames = (char *) malloc(numRanks * 64 * sizeof(char));
	strcpy(hostname, hwloc_obj_get_info_by_name(machine, "HostName"));
	MPI_Allgather(hostname, 64, MPI_CHAR, hostnames, 64, MPI_CHAR, comm);

	int i;
	int count = 0;
	int curColor = 1;
	char *nextHostname = NULL;
	char *curHostname = hostnames;
	int *colors = (int *) calloc(numRanks, sizeof(int));
	do{
		for(i=0; i<numRanks; i++){
			if(colors[i]) continue;

			char *hname = hostnames + i * 64;
			if(strcmp(curHostname, hname) == 0){
				colors[i] = curColor;
				count ++;
			}else {
				nextHostname = hname;
			}
		}
		curColor ++;
		curHostname = nextHostname;
	}while(count < numRanks);
	free(hostnames);

	MPI_Comm_split(comm, colors[myRank], myRank, intraNodeComm);   //split ranks assigned to the same machine into groups
	free(colors);
}

void hwloc_get_rank_assignment(MPI_Comm comm, int **rank2Socket, int **rank2Machine){
	int myRank, numParts;
	MPI_Comm_rank(comm, &myRank);
	MPI_Comm_size(comm, &numParts);

	hwloc_topology_t topo;
	hwloc_topology_init(&topo);
	hwloc_topology_load(topo);

	MPI_Comm intraNodeComm;
	hwloc_rank_split_by_mach(comm, topo, myRank, numParts, &intraNodeComm);

	int intraNodeRank;
	MPI_Comm_rank(intraNodeComm, &intraNodeRank);

	MPI_Comm interNodeComm;
	int groupLeader = (intraNodeRank == 0);
	MPI_Comm_split(comm, groupLeader, myRank, &interNodeComm); //each machine select a leader rank as representative for the group

	int mach_id;
	int num_maches;
	int socket_id = 0;
	if(groupLeader){
		int num_sockets;
		MPI_Comm_rank(interNodeComm, &mach_id);
		MPI_Comm_size(interNodeComm, &num_maches);

		num_sockets = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_SOCKET);
		int *sockets = (int *) malloc(sizeof(int) * num_maches);
		MPI_Allgather(&num_sockets, 1, MPI_INT, sockets, 1, MPI_INT, interNodeComm);

		int i;
		for(i=0; i<mach_id; i++){
			socket_id += sockets[i];
		}
		free(sockets);
	}
	int send_count[] = {mach_id, socket_id};
	MPI_Bcast(send_count, 2, MPI_INT, 0, intraNodeComm);

	*rank2Machine = (int *) malloc(sizeof(int) * numParts);
	MPI_Allgather(send_count, 1, MPI_INT, *rank2Machine, 1, MPI_INT, comm);

	hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
	hwloc_get_last_cpu_location(topo, cpuset, HWLOC_CPUBIND_PROCESS);
//	hwloc_get_cpubind(topo, cpuset, HWLOC_CPUBIND_PROCESS);
	int os_index = hwloc_bitmap_first(cpuset);
	hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topo, os_index);
	hwloc_bitmap_free(cpuset);

	hwloc_obj_t socket = pu;
	do{
		socket = socket->parent;
	}while(socket->type != HWLOC_OBJ_SOCKET);
	send_count[1] += socket->logical_index;

	*rank2Socket = (int *) malloc(sizeof(int) * numParts);
	MPI_Allgather(send_count + 1, 1, MPI_INT, *rank2Socket, 1, MPI_INT, comm);
	hwloc_topology_destroy(topo);
}

float* hwloc_get_rank_comm_costs(MPI_Comm comm){
	int myRank, numParts;
	MPI_Comm_rank(comm, &myRank);
	MPI_Comm_size(comm, &numParts);

	int *rank2sockets, *rank2maches;
	hwloc_get_rank_assignment(comm, &rank2sockets, &rank2maches);

	MPI_Comm intraNodeComm, intraSocketComm;
	MPI_Comm interNodeComm, interSocketComm;
	MPI_Comm_split(comm, rank2maches[myRank], myRank, &intraNodeComm);
	MPI_Comm_split(intraNodeComm, rank2sockets[myRank], myRank, &intraSocketComm);

	int intraNodeRank, intraSocketRank;
	MPI_Comm_rank(intraNodeComm, &intraNodeRank);
	MPI_Comm_rank(intraSocketComm, &intraSocketRank);
	MPI_Comm_split(comm, intraNodeRank == 0, myRank, &interNodeComm);
	MPI_Comm_split(intraNodeComm, intraSocketRank == 0, myRank, &interSocketComm);

	int socket_id, mach_id;
	MPI_Comm_rank(interSocketComm, &socket_id);
	MPI_Comm_rank(interNodeComm, &mach_id);
	MPI_Bcast(&socket_id, 1, MPI_INT, 0, intraSocketComm);	//tell each rank its socket_id
	MPI_Bcast(&mach_id, 1, MPI_INT, 0, intraNodeComm);		//tell each rank its node_id

	MPI_Allgather(&socket_id, 1, MPI_INT, rank2sockets, 1, MPI_INT, comm);
	MPI_Allgather(&mach_id, 1, MPI_INT, rank2maches, 1, MPI_INT, comm);

	int i, j;
	float *partNetCommCost = calloc(numParts * numParts, sizeof(float));

	//measuring intra-node network communication costs
	int numIntraSocketRanks;
	MPI_Comm_size(intraSocketComm, &numIntraSocketRanks);
	int *globalRank = (int *) malloc(sizeof(int) * numIntraSocketRanks);
	MPI_Allgather(&myRank, 1, MPI_INT, globalRank, 1, MPI_INT, intraSocketComm);
	float *intraSocketCommCosts = _getRankCommCost(intraSocketComm);
	for(i=0; i<numIntraSocketRanks; i++){
		int srcRank = globalRank[i];
		for(j=0; j<numIntraSocketRanks; j++){
			int destRank = globalRank[j];
			partNetCommCost[srcRank * numParts + destRank] = intraSocketCommCosts[i * numIntraSocketRanks + j];
		}
	}
	free(intraSocketCommCosts);
	free(globalRank);

	MPI_Allreduce(MPI_IN_PLACE, partNetCommCost, numParts * numParts, MPI_FLOAT, MPI_MAX, interSocketComm);
	if(intraSocketRank == 0){//measuring inter-socket network communication costs
		int numSockets;
		MPI_Comm_size(interSocketComm, &numSockets);
		float *interSocketCommCosts = _getRankCommCost(interSocketComm);
		for(i=0; i<numParts; i++){
			int srcMach = rank2maches[i];
			if(srcMach == mach_id){
				int srcSocket = rank2sockets[i];
				for(j=0; j<numParts; j++){
					int destMach = rank2maches[j];
					int destSocket = rank2sockets[j];
					if(destMach == mach_id && srcSocket != destSocket){
						partNetCommCost[i * numParts + j] = interSocketCommCosts[srcSocket * numSockets + destSocket];
					}
				}
			}
		}
		free(interSocketCommCosts);
	}

	MPI_Barrier(comm);
	if(intraNodeRank == 0){//measuring inter-node network communication costs
		int numMaches;
		MPI_Comm_size(interNodeComm, &numMaches);
		float *interNodeCommCosts = _getRankCommCost(interNodeComm);
		for(i=0; i<numParts; i++){
			int srcMach = rank2maches[i];
			for(j=0; j<numParts; j++){
				int destMach = rank2maches[j];
				if(srcMach != destMach){
					partNetCommCost[i * numParts + j] = interNodeCommCosts[srcMach * numMaches + destMach];
				}
			}
		}
		free(interNodeCommCosts);
	}
	free(rank2sockets);
	free(rank2maches);

	MPI_Allreduce(MPI_IN_PLACE, partNetCommCost, numParts * numParts, MPI_FLOAT, MPI_MAX, comm);

	//normalization
	float min = FLT_MAX;
	for(i=0; i<numParts; i++){
	   for(j=i+1; j<numParts; j++){
		   float cost = partNetCommCost[i * numParts + j];
		   if(min > cost){
			   min = cost;
		   }
	   }
	}
	for(i=0; i<numParts; i++){
	   for(j=0; j<numParts; j++){
		   if(i == j) continue;
		   int cost = partNetCommCost[i * numParts + j] / min * 10;
		   partNetCommCost[i * numParts + j] = cost / 10.0;
	   }
	}
	return partNetCommCost;
}



























//void hwloc_get_rank_pu_mapping(
//		MPI_Comm comm, MPI_Comm interNodeComm, MPI_Comm intraNodeComm,
//		hwloc_topology_t topo, int numParts,
//		int *my_pu_id, int **rank2pu, int *pu2rank){
//	int mach_id;
//	int num_maches;
//	int pu_id = 0;
//
//	int i;
//	int intraNodeRank;
//	int numIntraNodeRanks;
//	MPI_Comm_rank(intraNodeComm, &intraNodeRank);
//	MPI_Comm_size(intraNodeComm, &numIntraNodeRanks);
//
//	if(intraNodeRank == 0){
//		MPI_Comm_rank(interNodeComm, &mach_id);
//		MPI_Comm_size(interNodeComm, &num_maches);
//
//		int num_pu = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_PU);
//		int *pu = (int *) malloc(sizeof(int) * num_maches);
//		MPI_Allgather(&num_pu, 1, MPI_INT, pu, 1, MPI_INT, interNodeComm);
//
//		for(i=0; i<mach_id; i++){
//			pu_id += pu[i];
//		}
//		free(pu);
//	}
//	MPI_Bcast(&pu_id, 1, MPI_INT, 0, intraNodeComm);
//
//	hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
//	hwloc_get_last_cpu_location(topo, cpuset, HWLOC_CPUBIND_PROCESS);
//	int os_index = hwloc_bitmap_first(cpuset);
//	hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topo, os_index);
//	hwloc_bitmap_free(cpuset);
//	*my_pu_id = pu_id + pu->logical_index;
//
//	*rank2pu = (int *) malloc(sizeof(int) * numParts);
//	*pu2rank = (int *) malloc(sizeof(int) * numIntraNodeRanks);
//	MPI_Allgather(&(pu->logical_index), 1, MPI_INT, rank2pu, 1, MPI_INT, intraNodeComm);
//	for(i=0; i<numIntraNodeRanks; i++){
//		pu2rank[rank2pu[i]] = i;
//	}
//	MPI_Allgather(my_pu_id, 1, MPI_INT, rank2pu, 1, MPI_INT, comm);
//}

//void hwloc_get_pu2rank_mapping(MPI_Comm intraNodeComm, hwloc_topology_t topo, int num_pu, int **pu2rank){
//	hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
//	hwloc_get_last_cpu_location(topo, cpuset, HWLOC_CPUBIND_PROCESS);
//	int os_index = hwloc_bitmap_first(cpuset);
//	hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topo, os_index);
//	hwloc_bitmap_free(cpuset);
//
//	int i;
//	int *rank2pu = (int *) malloc(sizeof(int) * num_pu);
//	*pu2rank = (int *) malloc(sizeof(int) * num_pu);
//	MPI_Allgather(&(pu->logical_index), 1, MPI_INT, rank2pu, 1, MPI_INT, intraNodeComm);
//	for(i=0; i<num_pu; i++){
//		pu2rank[rank2pu[i]] = i;
//	}
//	free(rank2pu);
//}
//
//void hwloc_group_id_assignment(int group_id, int *pu2rank, int *rank2groups){
//	hwloc_obj_t obj;
//	for(obj = obj->first_child; obj; obj = obj->next_sibling){
//		hwloc_obj_t pu = obj;
//		while(pu->type != HWLOC_OBJ_PU){
//			pu = obj->first_child;
//		}
//		rank2groups[pu2rank[pu->logical_index]] = group_id;
//	}
//}
//
//
////grouping ranks of each machine into groups based on the machine topology
//void hwloc_intra_node_rank_grouping(hwloc_obj_t mach, int group_id, int *pu2rank, int *rank2groups){
//	hwloc_obj_t obj;
//	for(obj = mach->first_child; obj; obj = obj->next_cousin){
//		if(obj->arity > 1){
//			hwloc_intra_node_rank_grouping(obj, group_id);
//			group_id ++;
//		}else{
//			hwloc_obj_t parent = obj->parent;
//			hwloc_group_id_assignment(group_id, pu2rank, rank2groups);
//		}
//	}
//}
//
//void hwloc_get_rank_comm_costsv2(MPI_Comm comm){
//	int myRank, numParts;
//	MPI_Comm_rank(comm, &myRank);
//	MPI_Comm_size(comm, &numParts);
//
//	hwloc_topology_t topo;
//	hwloc_topology_init(&topo);
//	hwloc_topology_load(topo);
//
//	MPI_Comm intraNodeComm;
//	hwloc_rank_split_by_mach(comm, topo, myRank, numParts, &intraNodeComm);
//
//	int intraNodeRank;
//	int numIntraNodeRanks;
//	MPI_Comm_rank(intraNodeComm, &intraNodeRank);
//	MPI_Comm_size(intraNodeComm, &numIntraNodeRanks);
//
//	MPI_Comm interNodeComm;
//	int groupLeader = (intraNodeRank == 0);
//	MPI_Comm_split(comm, groupLeader, myRank, &interNodeComm); //each machine select a leader rank as representative for the group
//
//	int *pu2rank;
//	hwloc_get_pu2rank_mapping(intraNodeComm, topo, numIntraNodeRanks, &pu2rank);
//
//	int *rank2groups = (int *) malloc(sizeof(int) * numParts);
//	int *globalRanks = (int *) malloc(sizeof(int) * numIntraNodeRanks);
//	MPI_Allgather(&myRank, 1, MPI_INT, globalRanks, 1, MPI_INT, intraNodeComm);
//	if(groupLeader){
//		float *interNodeCommCosts = getRankCommCost(interNodeComm);
//
//		free(interNodeCommCosts);
//	}else if(intraNodeRank == 1){
//
//	}
//	free(rank2groups);
//	free(globalRanks);
//	free(pu2rank);
//	hwloc_topology_destroy(topo);
//}

// float* getRankCommCost(MPI_Comm comm) {
// 	int myRank, numProcs;
// 	MPI_Comm_rank(comm, &myRank);
// 	MPI_Comm_size(comm, &numProcs);

// 	int src, dest;
// 	double minCost = FLT_MAX;
// 	float *_rankCommCost = (float *) calloc(numProcs * numProcs, sizeof(float));
// 	for (src = 0; src < numProcs; src++) {
// 		for (dest = src + 1; dest < numProcs; dest++) {
// 			if (myRank == src || myRank == dest) {
// 				double cost = pingpongTest(comm, myRank, src, dest);
// 				if (myRank == src) {
// 					_rankCommCost[src * numProcs + dest] = cost;
// 					_rankCommCost[dest * numProcs + src] = cost;
// 					if (minCost > cost) {
// 						minCost = cost;
// 					}
// 				}
// 			}
// 			MPI_Barrier(comm); //ensure that each time only one rank is profiling
// 		}
// 	}
	
// 	//normalization
// 	MPI_Allreduce(MPI_IN_PLACE, &minCost, 1, MPI_DOUBLE, MPI_MIN, comm);
// 	if ((minCost) == 0)
// 		minCost = 1;

// 	float *n_rankCommCost = (float *) calloc(numProcs * numProcs, sizeof(float));
// 	for (src = 0; src < numProcs; src++) {
// 		for (dest = src + 1; dest < numProcs; dest++) {
// 			int ncost = _rankCommCost[src * numProcs + dest] * 10 / minCost;
// 			n_rankCommCost[src * numProcs + dest] = ncost / 10.0;
// 			n_rankCommCost[dest * numProcs + src] = ncost / 10.0;
// 		}
// 	}
// 	free(_rankCommCost);
	
// 	MPI_Allreduce(MPI_IN_PLACE, n_rankCommCost, numProcs * numProcs, MPI_FLOAT, MPI_MAX, comm);

// 	if(myRank == 0){
// 		int i, j;
// 		for(i=0; i<numProcs; i++){
// 			for(j=0; j<numProcs; j++){
// 				printf("%f\t", n_rankCommCost[i * numProcs + j]);
// 			}
// 			printf("%s", "\n");
// 		}
// 	}

// 	return n_rankCommCost;
// }
