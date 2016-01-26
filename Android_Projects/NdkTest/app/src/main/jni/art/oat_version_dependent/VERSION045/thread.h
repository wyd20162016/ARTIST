/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_THREAD_H_
#define ART_RUNTIME_THREAD_H_

#include <setjmp.h>

#include "../../../util/macros.h"
#include "entrypoints/interpreter/interpreter_entrypoints.h"
#include "entrypoints/jni/jni_entrypoints.h"
#include "entrypoints/portable/portable_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "jvalue.h"
#include "../../stack.h"
#include "../../runtime_stats.h"
#include "../../throw_location.h"
#include "../../base/mutex.h"

namespace art
{
// Thread priorities. These must match the Thread.MIN_PRIORITY,
// Thread.NORM_PRIORITY, and Thread.MAX_PRIORITY constants.
    enum ThreadPriority {
        kMinThreadPriority = 1,
        kNormThreadPriority = 5,
        kMaxThreadPriority = 10,
    };

    enum ThreadFlag {
        kSuspendRequest   = 1,  // If set implies that suspend_count_ > 0 and the Thread should enter the
        // safepoint handler.
        kCheckpointRequest = 2  // Request that the thread do some checkpoint work and then continue.
    };

    static constexpr size_t kNumRosAllocThreadLocalSizeBrackets = 34;

// Thread's stack layout for implicit stack overflow checks:
//
//   +---------------------+  <- highest address of stack memory
//   |                     |
//   .                     .  <- SP
//   |                     |
//   |                     |
//   +---------------------+  <- stack_end
//   |                     |
//   |  Gap                |
//   |                     |
//   +---------------------+  <- stack_begin
//   |                     |
//   | Protected region    |
//   |                     |
//   +---------------------+  <- lowest address of stack memory
//
// The stack always grows down in memory.  At the lowest address is a region of memory
// that is set mprotect(PROT_NONE).  Any attempt to read/write to this region will
// result in a segmentation fault signal.  At any point, the thread's SP will be somewhere
// between the stack_end and the highest address in stack memory.  An implicit stack
// overflow check is a read of memory at a certain offset below the current SP (4K typically).
// If the thread's SP is below the stack_end address this will be a read into the protected
// region.  If the SP is above the stack_end address, the thread is guaranteed to have
// at least 4K of space.  Because stack overflow checks are only performed in generated code,
// if the thread makes a call out to a native function (through JNI), that native function
// might only have 4K of memory (if the SP is adjacent to stack_end).

    struct Thread {

        // 32 bits of atomically changed state and flags. Keeping as 32 bits allows and atomic CAS to
        // change from being Suspended to Runnable without a suspend request occurring.
        union PACKED(4) StateAndFlags {
            StateAndFlags() {}
            struct PACKED(4) {
                // Bitfield of flag values. Must be changed atomically so that flag values aren't lost. See
                // ThreadFlags for bit field meanings.
                volatile uint16_t flags;
                // Holds the ThreadState. May be changed non-atomically between Suspended (ie not Runnable)
                // transitions. Changing to Runnable requires that the suspend_request be part of the atomic
                // operation. If a thread is suspended and a suspend_request is present, a thread may not
                // change to Runnable as a GC or other operation is in progress.
                volatile uint16_t state;
            } as_struct;
            uint32_t as_atomic_int;  // Used to be atomic int, but has to have the same size
            volatile int32_t as_int;

            private:
            // gcc does not handle struct with volatile member assignments correctly.
            // See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=47409
            DISALLOW_COPY_AND_ASSIGN(StateAndFlags);
        };

        // Maximum number of checkpoint functions.
        static constexpr uint32_t kMaxCheckpoints = 3;

        /***********************************************************************************************/
        // Thread local storage. Fields are grouped by size to enable 32 <-> 64 searching to account for
        // pointer size differences. To encourage shorter encoding, more frequently used values appear
        // first if possible.
        /***********************************************************************************************/

        struct PACKED(4) tls_32bit_sized_values {
            // We have no control over the size of 'bool', but want our boolean fields
            // to be 4-byte quantities.
            typedef uint32_t bool32_t;

            explicit tls_32bit_sized_values(bool is_daemon) :
                    suspend_count(0), debug_suspend_count(0), thin_lock_thread_id(0), tid(0),
                    daemon((bool32_t) is_daemon), throwing_OutOfMemoryError((bool32_t)false), no_thread_suspension(0),
                    thread_exit_check_count(0), is_exception_reported_to_instrumentation_((bool32_t)false),
                    handling_signal_((bool32_t)false), padding_(0) {
            }

            union StateAndFlags state_and_flags;

            // A non-zero value is used to tell the current thread to enter a safe point
            // at the next poll.
            int suspend_count;

            // How much of 'suspend_count_' is by request of the debugger, used to set things right
            // when the debugger detaches. Must be <= suspend_count_.
            int debug_suspend_count;

            // Thin lock thread id. This is a small integer used by the thin lock implementation.
            // This is not to be confused with the native thread's tid, nor is it the value returned
            // by java.lang.Thread.getId --- this is a distinct value, used only for locking. One
            // important difference between this id and the ids visible to managed code is that these
            // ones get reused (to ensure that they fit in the number of bits available).
            uint32_t thin_lock_thread_id;

            // System thread id.
            uint32_t tid;

            // Is the thread a daemon?
            const bool32_t daemon;

            // A boolean telling us whether we're recursively throwing OOME.
            bool32_t throwing_OutOfMemoryError;

            // A positive value implies we're in a region where thread suspension isn't expected.
            uint32_t no_thread_suspension;

            // How many times has our pthread key's destructor been called?
            uint32_t thread_exit_check_count;

            // When true this field indicates that the exception associated with this thread has already
            // been reported to instrumentation.
            bool32_t is_exception_reported_to_instrumentation_;

            // True if signal is being handled by this thread.
            bool32_t handling_signal_;

            // Padding to make the size aligned to 8.  Remove this if we add another 32 bit field.
            int32_t padding_;
        } tls32_;

        struct PACKED(8) tls_64bit_sized_values {
            tls_64bit_sized_values() : trace_clock_base(0), deoptimization_return_value() {
            }

            // The clock base used for tracing.
            uint64_t trace_clock_base;

            // Return value used by deoptimization.
            JValue deoptimization_return_value;

            struct RuntimeStats stats;
        } tls64_;

        struct PACKED(4) tls_ptr_sized_values {
            tls_ptr_sized_values() : card_table(nullptr), exception(nullptr), stack_end(nullptr),
                    managed_stack(), suspend_trigger(nullptr), jni_env(nullptr), self(nullptr), opeer(nullptr),
                    jpeer(nullptr), stack_begin(nullptr), stack_size(0), throw_location(),
                    stack_trace_sample(nullptr), wait_next(nullptr), monitor_enter_object(nullptr),
                    top_handle_scope(nullptr), class_loader_override(nullptr), long_jump_context(nullptr),
                    instrumentation_stack(nullptr), debug_invoke_req(nullptr), single_step_control(nullptr),
                    deoptimization_shadow_frame(nullptr), shadow_frame_under_construction(nullptr), name(nullptr),
                    pthread_self(0), last_no_thread_suspension_cause(nullptr), thread_local_start(nullptr),
                    thread_local_pos(nullptr), thread_local_end(nullptr), thread_local_objects(0),
                    thread_local_alloc_stack_top(nullptr), thread_local_alloc_stack_end(nullptr) {
            }

            // The biased card table, see CardTable for details.
            byte* card_table;

            // The pending exception or NULL.
            mirror::Throwable* exception;

            // The end of this thread's stack. This is the lowest safely-addressable address on the stack.
            // We leave extra space so there's room for the code that throws StackOverflowError.
            byte* stack_end;

            // The top of the managed stack often manipulated directly by compiler generated code.
            ManagedStack managed_stack;

            // In certain modes, setting this to 0 will trigger a SEGV and thus a suspend check.  It is
            // normally set to the address of itself.
            uintptr_t* suspend_trigger;

            // Every thread may have an associated JNI environment
            JNIEnvExt* jni_env;

            // Initialized to "this". On certain architectures (such as x86) reading off of Thread::Current
            // is easy but getting the address of Thread::Current is hard. This field can be read off of
            // Thread::Current to give the address.
            Thread* self;

            // Our managed peer (an instance of java.lang.Thread). The jobject version is used during thread
            // start up, until the thread is registered and the local opeer_ is used.
            mirror::Object* opeer;
            jobject jpeer;

            // The "lowest addressable byte" of the stack.
            byte* stack_begin;

            // Size of the stack.
            size_t stack_size;

            // The location the current exception was thrown from.
            struct ThrowLocation throw_location;

            // Pointer to previous stack trace captured by sampling profiler.
            std::vector* stack_trace_sample;

            // The next thread in the wait set this thread is part of or NULL if not waiting.
            Thread* wait_next;

            // If we're blocked in MonitorEnter, this is the object we're trying to lock.
            mirror::Object* monitor_enter_object;

            // Top of linked list of handle scopes or nullptr for none.
            void* top_handle_scope;

            // Needed to get the right ClassLoader in JNI_OnLoad, but also
            // useful for testing.
            void* class_loader_override;

            // Thread local, lazily allocated, long jump context. Used to deliver exceptions.
            Context* long_jump_context;

            // Additional stack used by method instrumentation to store method and return pc values.
            // Stored as a pointer since std::deque is not PACKED.
            std::deque* instrumentation_stack;

            // JDWP invoke-during-breakpoint support.
            DebugInvokeReq* debug_invoke_req;

            // JDWP single-stepping support.
            void* single_step_control;

            // Shadow frame stack that is used temporarily during the deoptimization of a method.
            struct ShadowFrame* deoptimization_shadow_frame;

            // Shadow frame stack that is currently under construction but not yet on the stack
            struct ShadowFrame* shadow_frame_under_construction;

            // A cached copy of the java.lang.Thread's name.
            std::string* name;

            // A cached pthread_t for the pthread underlying this Thread*.
            pthread_t pthread_self;

            // If no_thread_suspension_ is > 0, what is causing that assertion.
            const char* last_no_thread_suspension_cause;

            // Pending checkpoint function or NULL if non-pending. Installation guarding by
            // Locks::thread_suspend_count_lock_.
            Closure* checkpoint_functions[kMaxCheckpoints];

            // Entrypoint function pointers.
            // TODO: move this to more of a global offset table model to avoid per-thread duplication.
            InterpreterEntryPoints interpreter_entrypoints;
            JniEntryPoints jni_entrypoints;
            PortableEntryPoints portable_entrypoints;
            QuickEntryPoints quick_entrypoints;

            // Thread-local allocation pointer.
            byte* thread_local_start;
            byte* thread_local_pos;
            byte* thread_local_end;
            size_t thread_local_objects;

            // There are RosAlloc::kNumThreadLocalSizeBrackets thread-local size brackets per thread.
            void* rosalloc_runs[kNumRosAllocThreadLocalSizeBrackets];

            // Thread-local allocation stack data/routines.
            mirror::Object** thread_local_alloc_stack_top;
            mirror::Object** thread_local_alloc_stack_end;

            // Support for Mutex lock hierarchy bug detection.
            BaseMutex* held_mutexes[kLockLevelCount];

            // Recorded thread state for nested signals.
            jmp_buf* nested_signal_state;
        } tlsPtr_;

        // Guards the 'interrupted_' and 'wait_monitor_' members.
        void* wait_mutex_;

        // Condition variable waited upon during a wait.
        void* wait_cond_;
        // Pointer to the monitor lock we're currently waiting on or NULL if not waiting.
        void* wait_monitor_;

        // Thread "interrupted" status; stays raised until queried or thrown.
        bool interrupted_;
    };
}  // namespace art

#endif  // ART_RUNTIME_THREAD_H_
