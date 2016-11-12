/*
 * RankLatency.h
 *
 *  Created on: Jun 24, 2015
 *      Author: anz28
 */

#ifndef RANKLATENCY_H_
#define RANKLATENCY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <mpi.h>

float* hwloc_get_rank_comm_costs(MPI_Comm comm);
void hwloc_get_rank_assignment(MPI_Comm comm, int **rank2sockets, int **rank2maches);

#ifdef __cplusplus
}
#endif

#endif /* RANKLATENCY_H_ */
