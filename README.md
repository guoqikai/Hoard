# Hoard: a Scalable Memory Allocator
A C implementation of the Hoard memory allocator. The implementation is located in allocators/horad_alloc.

## Implementation details:
1. pointers to super-block reference and owner heap are stored at the beginning of each super-block, so when we free a block of memory we can shift to the head of its super-block and find its owner. The whole operation takes constant time. However, this approach might lead to internal fragmentation in some scenarios. To sovle this we could store these pointers at the top of the heap, but it leads to worse spacial locality.
2. In my implementation, one bin is reserved for completely full super-blocks, so we can avoid checking if a super-block is full when we allocate block.

## Benchmark results:
This Hoard implementation is compared with the memory allocator in standard library(libc), and a pool-based subpage allocator(kheap, also called kmalloc)   
![result](./results/cache-scratch_page-0001.jpg)
![result](./results/cache-thrash_page-0001.jpg)
![result](./results/linux-scalability_page-0001.jpg)
![result](./results/phong_page-0001.jpg)
![result](./results/threadtest_page-0001.jpg)

## How to run the benchmark:
1. cd into top directory of the project, run `export TOPDIR=$(pwd)`
2. run `make all`
3. cd into the benchmark directory, where you can run `./runall ./` to run all benchmarks or `./runbench` to run a particular benchmark. The whole benchmarking process took about 40min on my machine. Notice that you need to have at least 8 physical cores on you machine to run this benchmark.

## Reference:
Berger, E. D.; McKinley, K. S.; Blumofe, R. D.; Wilson, P. R. (November 2000). [Hoard: A Scalable Memory Allocator for Multithreaded Applications](https://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf)
