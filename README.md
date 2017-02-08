# MPINetworkCost
A piece of code I wrote for measuring the relative network communication costs among MPI processes. To ensure the correctness of the measured costs, each MPI process should be bound to a core or a hwthread during its lifecycle. This can be achieved via 			
		
		mpirun --bind-to option 

for OpenMPI 1.8.6 (or above). 

Also, the code requires the installation of [hwloc](https://www.open-mpi.org/projects/hwloc/) libary. This is because I use the knowledge of the underlying hardware toplogy to speedup the measurement process. That is, I organize the MPI ranks into three groups:

* MPI ranks runnig on cores of the same machine
  * MPI ranks running on cores of the same CPU socket
  * MPI ranks running on cores on the same machine but on different CPU sockets
* MPI ranks running on cores on the same machine

To measure the relatively inter-machine network communication costs (costs among MPI ranks running on different machines), I first select a representative MPI rank for MPI ranks running on the same machine and then measure the costs among the selected MPI ranks. 

Similary, for inter-socket network communication costs (costs among MPI ranks running on different sockets but on the same machine), I first pick a representative MPI rank for the ranks running on the same socket, and then measure the costs among the selected MPI ranks. Again, we only need one MPI rank for each socket. Note that each machine measures its own inter-socket network communiation costs independently at the same time.

As for the relative intra-socket network communication costs (costs among MPI ranks running on the same CPU socket), we simply measure the all pair-wise network communication costs among mpi ranks running on the same socket. In fact, we only need to measure one pair for all the ranks running on the same socket and use the cost for all the other pairs. 

Note that by doing this, we can significantly reduce the measuring time. For example, for a program with 100 processes running on five 20-core machines (one process per core). Each machine has two 10-core socket. The naive solution need to measure the relative network communication costs for (99+1) * 99 / 2 = 4950 process pairs. However, using the above proposed solution, we only need to measure:

* 10 process pairs for the relaitve inter-machine network communication costs
* 1 process pair for the relative inter-socket network communication costs (one pair per machine). Note that the measurement of inter-socket costs on different machines is performed in a parallel fashion. That is, the time it took to measure the costs is equvilaent to the slowest pair. 
* (9+1) * 9 / 2 = 45 process pairs for the relative intra-socket network communication costs (45 pairs per socket). However, the measurement of different sockets is again performed in a parallel fashion. 
