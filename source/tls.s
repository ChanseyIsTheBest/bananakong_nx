/* TPIDR_EL0 accessors (writable from EL0 on AArch64). Kept in assembly so the
 * result does not depend on compiler-specific register-binding extensions. */
    .text
    .global nx_set_tpidr_el0
    .type   nx_set_tpidr_el0, %function
nx_set_tpidr_el0:
    msr tpidr_el0, x0
    ret

    .global nx_get_tpidr_el0
    .type   nx_get_tpidr_el0, %function
nx_get_tpidr_el0:
    mrs x0, tpidr_el0
    ret
