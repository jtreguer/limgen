// Holds an amount of video memory until it is stopped, so the benchmark can
// measure how a model behaves when another program already uses part of the
// GPU (docs/models.md section 6). NVIDIA only.
//
// Build: nvcc -O2 -o build/bench/hold-vram bench/hold-vram.cu
// Usage: hold-vram <GiB>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2 || atof(argv[1]) <= 0) {
        fprintf(stderr, "usage: hold-vram <GiB>\n");
        return 2;
    }
    const size_t bytes = static_cast<size_t>(atof(argv[1]) * 1024 * 1024 * 1024);
    void* memory = nullptr;
    if (cudaMalloc(&memory, bytes) != cudaSuccess) {
        fprintf(stderr, "hold-vram: cannot allocate %s GiB\n", argv[1]);
        return 1;
    }
    // Writing to the memory makes the driver commit it, so it shows up as used.
    cudaMemset(memory, 0, bytes);
    cudaDeviceSynchronize();
    printf("hold-vram: holding %s GiB\n", argv[1]);
    fflush(stdout);
    pause();
    return 0;
}
