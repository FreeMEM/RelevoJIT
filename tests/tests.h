/* SPDX-License-Identifier: MPL-2.0
 *
 * Every unit test, in the order they run. X(name) declares
 * `void test_name(void)`, defined in one of the test_*.c files.
 */
#define TEST_LIST(X)            \
    X(config_validation)        \
    X(create_defaults)          \
    X(map_pages)                \
    X(memory_big_endian)        \
    X(memory_host_pages)        \
    X(memory_page_crossing)     \
    X(address_mask)             \
    X(nop_budget)               \
    X(rts_to_zero_exits)        \
    X(rts_to_zero_option_off)   \
    X(set_sr_switches_stacks)   \
    X(illegal_exception_68000)  \
    X(illegal_exception_68010)  \
    X(line_a_host)              \
    X(line_f_host_stop)         \
    X(request_stop)             \
    X(independent_cpus)         \
    X(table_no_conflicts)       \
    X(address_error_frame)      \
    X(bus_error_unmapped)       \
    X(double_fault_halts)       \
    X(trace)                    \
    X(trace_not_after_illegal)  \
    X(interrupt_autovector)     \
    X(interrupt_nmi_edge)       \
    X(stop_until_interrupt)     \
    X(privilege_violation)      \
    X(reset_instruction)        \
    X(trap_and_rte)             \
    X(cycles)                   \
    X(divide_by_zero)           \
    X(json_values)              \
    X(json_nesting_and_skip)    \
    X(json_errors)              \
    X(sst_reader)               \
    X(sst_run_nop)

#define DECLARE_TEST(name) void test_##name(void);
TEST_LIST(DECLARE_TEST)
