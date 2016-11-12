# MPINetworkCost
A piece of code I wrote for measuring the relatively network communication costs among MPI processes. To ensure the correctness of the measured costs, each MPI process should be bound to a core or a hwthread during its lifecycle. This can be achieved via mpirun --bind-to option. 

Also, the code requires the installation of [hwloc](https://www.open-mpi.org/projects/hwloc/) libary. This is because I use the knowledge of the underlying hardware toplogy to speedup the measurement process. For example, to measure the relatively network communication costs among MPI ranks running on different CPU sockets, I first select a representative MPI rank for MPI ranks running on cores of the same socket and then measure the costs among the selected MPI ranks.  
