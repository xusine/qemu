#include "qemu/osdep.h"
#include "sysemu/quantum.h"
#include "qemu/option.h"

uint64_t quantum_size = 0;

void quantum_configure(QemuOpts *opts, Error **errp) {
    uint64_t quantum_size_tmp = qemu_opt_get_number(opts, "size", 0);

    // make it as a global value.
    quantum_size = quantum_size_tmp;
}