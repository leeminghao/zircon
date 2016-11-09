// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <launchpad/launchpad.h>
#include <launchpad/vmo.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/debug.h>
#include <magenta/syscalls/exception.h>
#include <magenta/syscalls/port.h>
#include <mxio/util.h>
#include <test-utils/test-utils.h>
#include <unittest/unittest.h>

#include "utils.h"

// 0.5 seconds
#define WATCHDOG_DURATION_TICK ((int64_t) 500 * 1000 * 1000)
// 5 seconds
#define WATCHDOG_DURATION_TICKS 10

#define TEST_MEMORY_SIZE 8
#define TEST_DATA_ADJUST 0x10

// Do the segv recovery test a number of times to stress test the API.
#define NUM_SEGV_TRIES 4

#define NUM_EXTRA_THREADS 4

// Produce a backtrace of sufficient size to be interesting but not excessive.
#define TEST_SEGFAULT_DEPTH 4

static const char test_inferior_child_name[] = "inferior";
// The segfault child is not used by the test.
// It exists for debugging purposes.
static const char test_segfault_child_name[] = "segfault";

static bool done_tests = false;

static void test_memory_ops(mx_handle_t inferior, mx_handle_t thread)
{
    uint64_t test_data_addr = 0;
    mx_ssize_t ssize;
    uint8_t test_data[TEST_MEMORY_SIZE];

#ifdef __x86_64__
    test_data_addr = get_uint64_register(thread, offsetof(mx_x86_64_general_regs_t, r9));
#endif
#ifdef __aarch64__
    test_data_addr = get_uint64_register(thread, offsetof(mx_aarch64_general_regs_t, r[9]));
#endif

    ssize = read_inferior_memory(inferior, test_data_addr, test_data, sizeof(test_data));
    EXPECT_EQ(ssize, (mx_ssize_t) sizeof(test_data), "read_inferior_memory: short read");

    for (unsigned i = 0; i < sizeof(test_data); ++i) {
        EXPECT_EQ(test_data[i], i, "test_memory_ops");
    }

    for (unsigned i = 0; i < sizeof(test_data); ++i)
        test_data[i] += TEST_DATA_ADJUST;

    ssize = write_inferior_memory(inferior, test_data_addr, test_data, sizeof(test_data));
    EXPECT_EQ(ssize, (mx_ssize_t) sizeof(test_data), "write_inferior_memory: short write");

    // Note: Verification of the write is done in the inferior.
}

static void fix_inferior_segv(mx_handle_t thread)
{
    unittest_printf("Fixing inferior segv\n");

#ifdef __x86_64__
    // The segv was because r8 == 0, change it to a usable value.
    // See test_prep_and_segv.
    uint64_t rsp = get_uint64_register(thread, offsetof(mx_x86_64_general_regs_t, rsp));
    set_uint64_register(thread, offsetof(mx_x86_64_general_regs_t, r8), rsp);
#endif

#ifdef __aarch64__
    // The segv was because r8 == 0, change it to a usable value.
    // See test_prep_and_segv.
    uint64_t sp = get_uint64_register(thread, offsetof(mx_aarch64_general_regs_t, sp));
    set_uint64_register(thread, offsetof(mx_aarch64_general_regs_t, r[8]), sp);
#endif
}

// This exists so that we can use ASSERT_EQ which does a return on failure.

static bool wait_inferior_thread_worker(void* arg)
{
    mx_handle_t* args = arg;
    mx_handle_t inferior = args[0];
    mx_handle_t eport = args[1];
    int i;

    for (i = 0; i < NUM_SEGV_TRIES; ++i) {
        unittest_printf("wait-inf: waiting on inferior\n");

        mx_exception_packet_t packet;
        ASSERT_EQ(mx_port_wait(eport, MX_TIME_INFINITE, &packet, sizeof(packet)),
                  NO_ERROR, "mx_io_port_wait failed");
        unittest_printf("wait-inf: finished waiting, got exception 0x%x\n", packet.report.header.type);
        if (packet.report.header.type == MX_EXCP_GONE) {
            unittest_printf("wait-inf: inferior gone\n");
            break;
        } else if (MX_EXCP_IS_ARCH(packet.report.header.type)) {
            unittest_printf("wait-inf: got exception\n");
        } else {
            ASSERT_EQ(false, true, "wait-inf: unexpected exception type");
        }

        mx_koid_t tid = packet.report.context.tid;
        mx_handle_t thread;
        mx_status_t status = mx_object_get_child(inferior, tid, MX_RIGHT_SAME_RIGHTS, &thread);
        ASSERT_EQ(status, 0, "mx_debug_task_get_child failed");

        dump_inferior_regs(thread);

        // Do some tests that require a suspended inferior.
        test_memory_ops(inferior, thread);

        // Now correct the issue and resume the inferior.

        fix_inferior_segv(thread);
        // Useful for debugging, otherwise a bit too verbose.
        //dump_inferior_regs(thread);

        status = mx_task_resume(thread, MX_RESUME_EXCEPTION);
        tu_handle_close(thread);
        ASSERT_EQ(status, NO_ERROR, "mx_task_resume failed");
    }

    ASSERT_EQ(i, NUM_SEGV_TRIES, "segv tests terminated prematurely");

    return true;
}

static int wait_inferior_thread_func(void* arg)
{
    wait_inferior_thread_worker(arg);

    // We have to call thread_exit ourselves.
    mx_thread_exit();
}

static int watchdog_thread_func(void* arg)
{
    for (int i = 0; i < WATCHDOG_DURATION_TICKS; ++i) {
        mx_nanosleep(WATCHDOG_DURATION_TICK);
        if (done_tests)
            mx_thread_exit();
    }
    unittest_printf("WATCHDOG TIMER FIRED\n");
    // This should kill the entire process, not just this thread.
    exit(5);
}

static bool debugger_test(void)
{
    BEGIN_TEST;

    mx_handle_t pipe, inferior, eport;

    if (!setup_inferior(test_inferior_child_name, &pipe, &inferior, &eport))
        return false;

    mx_handle_t wait_inf_args[2] = { inferior, eport };
    thrd_t wait_inferior_thread;
    tu_thread_create_c11(&wait_inferior_thread, wait_inferior_thread_func, (void*) &wait_inf_args[0], "wait-inf thread");

    enum message msg;
    send_msg(pipe, MSG_CRASH);
    if (!recv_msg(pipe, &msg))
        return false;
    EXPECT_EQ(msg, MSG_RECOVERED_FROM_CRASH, "unexpected response from crash");

    if (!shutdown_inferior(pipe, inferior, eport))
        return false;

    unittest_printf("Waiting for wait-inf thread\n");
    int ret = thrd_join(wait_inferior_thread, NULL);
    EXPECT_EQ(ret, thrd_success, "thrd_join failed");
    unittest_printf("wait-inf thread done\n");

    END_TEST;
}

static bool debugger_thread_list_test(void)
{
    BEGIN_TEST;

    mx_handle_t pipe, inferior, eport;

    if (!setup_inferior(test_inferior_child_name, &pipe, &inferior, &eport))
        return false;

    enum message msg;
    send_msg(pipe, MSG_START_EXTRA_THREADS);
    if (!recv_msg(pipe, &msg))
        return false;
    EXPECT_EQ(msg, MSG_EXTRA_THREADS_STARTED, "unexpected response when starting extra threads");

    uint32_t buf_size = sizeof(mx_info_process_threads_t) + 100 * sizeof(mx_record_process_thread_t);
    mx_info_process_threads_t* threads = tu_malloc(buf_size);
    mx_size_t size;
    mx_status_t status = mx_object_get_info(inferior, MX_INFO_PROCESS_THREADS,
                                            sizeof(mx_record_process_thread_t), threads, buf_size, &size);
    ASSERT_EQ(status, NO_ERROR, "");

    // There should be at least 1+NUM_EXTRA_THREADS threads in the result.
    ASSERT_GE(size, sizeof(mx_info_header_t) + (1+NUM_EXTRA_THREADS) * sizeof(mx_record_process_thread_t), "mx_object_get_info failed");

    uint32_t num_threads = threads->hdr.count;

    // Verify each entry is valid.
    for (uint32_t i = 0; i < num_threads; ++i) {
        mx_koid_t koid = threads->rec[i].koid;
        unittest_printf("Looking up thread %llu\n", (long long) koid);
        mx_handle_t thread;
        status = mx_object_get_child(inferior, koid, MX_RIGHT_SAME_RIGHTS, &thread);
        EXPECT_EQ(status, 0, "mx_debug_task_get_child failed");
        mx_size_t size = 0;
        mx_info_handle_basic_t info;
        mx_object_get_info(thread, MX_INFO_HANDLE_BASIC, sizeof(mx_record_handle_basic_t), &info, sizeof(info), &size);
        EXPECT_EQ(size, sizeof(info), "mx_object_get_info failed");
        EXPECT_EQ(info.rec.type, (uint32_t) MX_OBJ_TYPE_THREAD, "not a thread");
    }

    if (!shutdown_inferior(pipe, inferior, eport))
        return false;

    END_TEST;
}

// This function is marked as no-inline to avoid duplicate label in case the
// function call is being inlined.
__NO_INLINE static bool test_prep_and_segv(void)
{
    uint8_t test_data[TEST_MEMORY_SIZE];
    for (unsigned i = 0; i < sizeof(test_data); ++i)
        test_data[i] = i;

#ifdef __x86_64__
    void* segv_pc;
    // Note: Fuchsia is always pic.
    __asm__ ("movq .Lsegv_here@GOTPCREL(%%rip),%0" : "=r" (segv_pc));
    unittest_printf("About to segv, pc 0x%lx\n", (long) segv_pc);

    // Set r9 to point to test_data so we can easily access it
    // from the parent process.
    __asm__ ("\
	movq %0,%%r9\n\
	movq $0,%%r8\n\
.Lsegv_here:\n\
	movq (%%r8),%%rax\
"
        : : "r" (&test_data[0]) : "rax", "r8", "r9");
#endif

#ifdef __aarch64__
    void* segv_pc;
    // Note: Fuchsia is always pic.
    __asm__ ("mov %0,.Lsegv_here" : "=r" (segv_pc));
    unittest_printf("About to segv, pc 0x%lx\n", (long) segv_pc);

    // Set r9 to point to test_data so we can easily access it
    // from the parent process.
    __asm__ ("\
	mov x9,%0\n\
	mov x8,0\n\
.Lsegv_here:\n\
	ldr x0,[x8]\
"
        : : "r" (&test_data[0]) : "x0", "x8", "x9");
#endif

    // On resumption test_data should have had TEST_DATA_ADJUST added to each element.
    // Note: This is the inferior process, it's not running under the test harness.
    for (unsigned i = 0; i < sizeof(test_data); ++i) {
        if (test_data[i] != i + TEST_DATA_ADJUST) {
            unittest_printf("test_prep_and_segv: bad data on resumption, test_data[%u] = 0x%x\n",
                            i, test_data[i]);
            return false;
        }
    }

    unittest_printf("Inferior successfully resumed!\n");

    return true;
}

static int extra_thread_func(void* arg)
{
    unittest_printf("Extra thread started.\n");
    while (true)
        mx_nanosleep(1000 * 1000 * 1000);
    return 0;
}

// This returns "bool" because it uses ASSERT_*.

static bool msg_loop(mx_handle_t pipe)
{
    BEGIN_HELPER;

    bool my_done_tests = false;

    while (!done_tests && !my_done_tests)
    {
        enum message msg;
        ASSERT_TRUE(recv_msg(pipe, &msg), "Error while receiving msg");
        switch (msg)
        {
        case MSG_DONE:
            my_done_tests = true;
            break;
        case MSG_PING:
            send_msg(pipe, MSG_PONG);
            break;
        case MSG_CRASH:
            for (int i = 0; i < NUM_SEGV_TRIES; ++i) {
                if (!test_prep_and_segv())
                    exit(21);
            }
            send_msg(pipe, MSG_RECOVERED_FROM_CRASH);
            break;
        case MSG_START_EXTRA_THREADS:
            for (int i = 0; i < NUM_EXTRA_THREADS; ++i) {
                // For our purposes, we don't need to track the threads.
                // They'll be terminated when the process exits.
                thrd_t thread;
                tu_thread_create_c11(&thread, extra_thread_func, NULL, "extra-thread");
            }
            send_msg(pipe, MSG_EXTRA_THREADS_STARTED);
            break;
        default:
            unittest_printf("unknown message received: %d\n", msg);
            break;
        }
    }

    END_HELPER;
}

void test_inferior(void)
{
    mx_handle_t pipe = mxio_get_startup_handle(MX_HND_TYPE_USER0);
    unittest_printf("test_inferior: got handle %d\n", pipe);

    if (!msg_loop(pipe))
        exit(20);

    done_tests = true;
    unittest_printf("Inferior done\n");
    exit(1234);
}

// Compilers are getting too smart.
// These maintain the semantics we want even under optimization.

volatile int* crashing_ptr = (int*) 42;
volatile int crash_depth;

// This is used to cause fp != sp when the crash happens on arm64.
int leaf_stack_size = 10;

static int __NO_INLINE test_segfault_doit2(int*);

static int __NO_INLINE test_segfault_leaf(int n, int* p)
{
    volatile int x[n];
    x[0] = *p;
    *crashing_ptr = x[0];
    return 0;
}

static int __NO_INLINE test_segfault_doit1(int* p)
{
    if (crash_depth > 0)
    {
        int n = crash_depth;
        int use_stack[n];
        memset(use_stack, 0x99, n * sizeof(int));
        --crash_depth;
        return test_segfault_doit2(use_stack) + 99;
    }
    return test_segfault_leaf(leaf_stack_size, p) + 99;
}

static int __NO_INLINE test_segfault_doit2(int* p)
{
    return test_segfault_doit1(p) + *p;
}

// Produce a crash with a moderately interesting backtrace.

static int __NO_INLINE test_segfault(void)
{
    crash_depth = TEST_SEGFAULT_DEPTH;
    int i = 0;
    return test_segfault_doit1(&i);
}

BEGIN_TEST_CASE(debugger_tests)
RUN_TEST(debugger_test);
RUN_TEST(debugger_thread_list_test);
END_TEST_CASE(debugger_tests)

static void check_verbosity(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "v=", 2) == 0) {
            int verbosity = atoi(argv[i] + 2);
            unittest_set_verbosity_level(verbosity);
            break;
        }
    }
}

int main(int argc, char **argv)
{
    program_path = argv[0];

    if (argc >= 2 && strcmp(argv[1], test_inferior_child_name) == 0) {
        check_verbosity(argc, argv);
        test_inferior();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], test_segfault_child_name) == 0) {
        check_verbosity(argc, argv);
        return test_segfault();
    }

    thrd_t watchdog_thread;
    tu_thread_create_c11(&watchdog_thread, watchdog_thread_func, NULL, "watchdog-thread");

    bool success = unittest_run_all_tests(argc, argv);

    done_tests = true;
    thrd_join(watchdog_thread, NULL);
    return success ? 0 : -1;
}
