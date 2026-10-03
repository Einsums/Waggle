//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

// The part of the OMPT interface (OpenMP 5.0, section 4) the OpenMP source uses, with the names
// and values of omp-tools.h, which declares them in the global namespace; here they are in a
// namespace of their own so a translation unit may include both, as the test that compares them
// does. Only the values the source reads are listed.

#include <Waggle/Config.hpp>

#include <cstdint>

WAGGLE_NAMESPACE_BEGIN

namespace ompt_types {

union ompt_data_t {
    uint64_t value;
    void    *ptr;
};

struct ompt_frame_t; // only ever passed through

using ompt_interface_fn_t    = void (*)();
using ompt_function_lookup_t = ompt_interface_fn_t (*)(char const *interface_function_name);
using ompt_initialize_t      = int (*)(ompt_function_lookup_t lookup, int initial_device_num, ompt_data_t *tool_data);
using ompt_finalize_t        = void (*)(ompt_data_t *tool_data);

struct ompt_start_tool_result_t {
    ompt_initialize_t initialize;
    ompt_finalize_t   finalize;
    ompt_data_t       tool_data;
};

enum ompt_callbacks_t : int {
    ompt_callback_thread_begin     = 1,
    ompt_callback_parallel_begin   = 3,
    ompt_callback_parallel_end     = 4,
    ompt_callback_implicit_task    = 7,
    ompt_callback_sync_region_wait = 16,
};

enum ompt_set_result_t : int {
    ompt_set_error            = 0,
    ompt_set_never            = 1,
    ompt_set_impossible       = 2,
    ompt_set_sometimes        = 3,
    ompt_set_sometimes_paired = 4,
    ompt_set_always           = 5,
};

enum ompt_thread_t : int {
    ompt_thread_initial = 1,
    ompt_thread_worker  = 2,
    ompt_thread_other   = 3,
    ompt_thread_unknown = 4,
};

enum ompt_scope_endpoint_t : int {
    ompt_scope_begin    = 1,
    ompt_scope_end      = 2,
    ompt_scope_beginend = 3,
};

enum ompt_sync_region_t : int {
    ompt_sync_region_barrier                    = 1, // deprecated in 5.1, still sent by older runtimes
    ompt_sync_region_barrier_implicit           = 2, // likewise
    ompt_sync_region_barrier_explicit           = 3,
    ompt_sync_region_barrier_implementation     = 4,
    ompt_sync_region_taskwait                   = 5,
    ompt_sync_region_taskgroup                  = 6,
    ompt_sync_region_reduction                  = 7,
    ompt_sync_region_barrier_implicit_workshare = 8,
    ompt_sync_region_barrier_implicit_parallel  = 9,
    ompt_sync_region_barrier_teams              = 10,
};

enum ompt_task_flag_t : int {
    ompt_task_initial  = 0x1,
    ompt_task_implicit = 0x2,
};

using ompt_callback_t     = void (*)();
using ompt_set_callback_t = ompt_set_result_t (*)(ompt_callbacks_t event, ompt_callback_t callback);

} // namespace ompt_types

WAGGLE_NAMESPACE_END
