// Starts the vendor GPU data producer (/vendor/lib64/libgpudataproducer.so) the way Android GPU Inspector does, so that Perfetto gets
// the driver's gpu.counters and gpu.renderstages data sources (Android GPU profiling: graphics.gpu.profiler.support=true). Run from
// /data/local/tmp as the shell user (that directory uses the linker's unrestricted namespace): `./gpuprod_launch &`. Probe only.
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *lib = argc > 1 ? argv[1] : "libgpudataproducer.so";
    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
    void (*start)(void) = (void (*)(void))dlsym(h, "start");
    if (!start) { fprintf(stderr, "dlsym start: %s\n", dlerror()); return 2; }
    fprintf(stderr, "starting the producer from %s\n", lib); fflush(stderr);
    start();   // the vendor producer may block here or return after starting its own threads
    fprintf(stderr, "start() returned; keeping the process alive\n"); fflush(stderr);
    for (;;) pause();
}
