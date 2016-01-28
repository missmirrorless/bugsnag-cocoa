//
//  BugsnagKSCrashSentry_MachException.c
//
//  Created by Karl Stenerud on 2012-02-04.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//


#include "BugsnagKSCrashSentry_MachException.h"
#include "BugsnagKSCrashSentry_Private.h"
#include "BugsnagKSMach.h"

//#define BugsnagKSLogger_LocalLevel TRACE
#include "BugsnagKSLogger.h"

#include <pthread.h>


// ============================================================================
#pragma mark - Constants -
// ============================================================================

#define kThreadPrimary "BugsnagKSCrash Exception Handler (Primary)"
#define kThreadSecondary "BugsnagKSCrash Exception Handler (Secondary)"


// ============================================================================
#pragma mark - Types -
// ============================================================================

/** A mach exception message (according to ux_exception.c, xnu-1699.22.81).
 */
typedef struct
{
    /** Mach header. */
    mach_msg_header_t          header;

    // Start of the kernel processed data.

    /** Basic message body data. */
    mach_msg_body_t            body;

    /** The thread that raised the exception. */
    mach_msg_port_descriptor_t thread;

    /** The task that raised the exception. */
    mach_msg_port_descriptor_t task;

    // End of the kernel processed data.

    /** Network Data Representation. */
    NDR_record_t               NDR;

    /** The exception that was raised. */
    exception_type_t           exception;

    /** The number of codes. */
    mach_msg_type_number_t     codeCount;

    /** Exception code and subcode. */
    // ux_exception.c defines this as mach_exception_data_t for some reason.
    // But it's not actually a pointer; it's an embedded array.
    // On 32-bit systems, only the lower 32 bits of the code and subcode
    // are valid.
    mach_exception_data_type_t code[0];

    /** Padding to avoid RCV_TOO_LARGE. */
    char                       padding[512];
} MachExceptionMessage;

/** A mach reply message (according to ux_exception.c, xnu-1699.22.81).
 */
typedef struct
{
    /** Mach header. */
    mach_msg_header_t header;

    /** Network Data Representation. */
    NDR_record_t      NDR;

    /** Return code. */
    kern_return_t     returnCode;
} MachReplyMessage;


// ============================================================================
#pragma mark - Globals -
// ============================================================================

/** Flag noting if we've installed our custom handlers or not.
 * It's not fully thread safe, but it's safer than locking and slightly better
 * than nothing.
 */
static volatile sig_atomic_t g_installed = 0;

/** Holds exception port info regarding the previously installed exception
 * handlers.
 */
static struct
{
    exception_mask_t        masks[EXC_TYPES_COUNT];
    exception_handler_t     ports[EXC_TYPES_COUNT];
    exception_behavior_t    behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t   flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t  count;
} g_previousExceptionPorts;

/** Our exception port. */
static mach_port_t g_exceptionPort = MACH_PORT_NULL;

/** Primary exception handler thread. */
static pthread_t g_primaryPThread;
static thread_t g_primaryMachThread;

/** Secondary exception handler thread in case crash handler crashes. */
static pthread_t g_secondaryPThread;
static thread_t g_secondaryMachThread;

/** Context to fill with crash information. */
static BugsnagKSCrash_SentryContext* g_context;


// ============================================================================
#pragma mark - Utility -
// ============================================================================

// Avoiding static methods due to linker issue.

/** Get all parts of the machine state required for a dump.
 * This includes basic thread state, and exception registers.
 *
 * @param thread The thread to get state for.
 *
 * @param machineContext The machine context to fill out.
 */
bool bugsnag_ksmachexc_i_fetchMachineState(const thread_t thread,
                                   STRUCT_MCONTEXT_L* const machineContext)
{
    if(!bugsnag_ksmach_threadState(thread, machineContext))
    {
        return false;
    }

    if(!bugsnag_ksmach_exceptionState(thread, machineContext))
    {
        return false;
    }

    return true;
}

/** Restore the original mach exception ports.
 */
void bugsnag_ksmachexc_i_restoreExceptionPorts(void)
{
    BugsnagKSLOG_DEBUG("Restoring original exception ports.");
    if(g_previousExceptionPorts.count == 0)
    {
        BugsnagKSLOG_DEBUG("Original exception ports were already restored.");
        return;
    }

    const task_t thisTask = mach_task_self();
    kern_return_t kr;

    // Reinstall old exception ports.
    for(mach_msg_type_number_t i = 0; i < g_previousExceptionPorts.count; i++)
    {
        BugsnagKSLOG_TRACE("Restoring port index %d", i);
        kr = task_set_exception_ports(thisTask,
                                      g_previousExceptionPorts.masks[i],
                                      g_previousExceptionPorts.ports[i],
                                      g_previousExceptionPorts.behaviors[i],
                                      g_previousExceptionPorts.flavors[i]);
        if(kr != KERN_SUCCESS)
        {
            BugsnagKSLOG_ERROR("task_set_exception_ports: %s",
                        mach_error_string(kr));
        }
    }
    BugsnagKSLOG_DEBUG("Exception ports restored.");
    g_previousExceptionPorts.count = 0;
}


// ============================================================================
#pragma mark - Handler -
// ============================================================================

/** Our exception handler thread routine.
 * Wait for an exception message, uninstall our exception port, record the
 * exception information, and write a report.
 */
void* bugsnag_ksmachexc_i_handleExceptions(void* const userData)
{
    MachExceptionMessage exceptionMessage = {{0}};
    MachReplyMessage replyMessage = {{0}};

    const char* threadName = (const char*) userData;
    pthread_setname_np(threadName);
    if(threadName == kThreadSecondary)
    {
        BugsnagKSLOG_DEBUG("This is the secondary thread. Suspending.");
        thread_suspend(bugsnag_ksmach_thread_self());
    }

    for(;;)
    {
        BugsnagKSLOG_DEBUG("Waiting for mach exception");

        // Wait for a message.
        kern_return_t kr = mach_msg(&exceptionMessage.header,
                                    MACH_RCV_MSG,
                                    0,
                                    sizeof(exceptionMessage),
                                    g_exceptionPort,
                                    MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if(kr == KERN_SUCCESS)
        {
            break;
        }

        // Loop and try again on failure.
        BugsnagKSLOG_ERROR("mach_msg: %s", mach_error_string(kr));
    }

    BugsnagKSLOG_DEBUG("Trapped mach exception code 0x%x, subcode 0x%x",
                exceptionMessage.code[0], exceptionMessage.code[1]);
    if(g_installed)
    {
        bool wasHandlingCrash = g_context->handlingCrash;
        bugsnag_kscrashsentry_beginHandlingCrash(g_context);

        BugsnagKSLOG_DEBUG("Exception handler is installed. Continuing exception handling.");

        BugsnagKSLOG_DEBUG("Suspending all threads");
        bugsnag_kscrashsentry_suspendThreads();

        // Switch to the secondary thread if necessary, or uninstall the handler
        // to avoid a death loop.
        if(bugsnag_ksmach_thread_self() == g_primaryMachThread)
        {
            BugsnagKSLOG_DEBUG("This is the primary exception thread. Activating secondary thread.");
            if(thread_resume(g_secondaryMachThread) != KERN_SUCCESS)
            {
                BugsnagKSLOG_DEBUG("Could not activate secondary thread. Restoring original exception ports.");
                bugsnag_ksmachexc_i_restoreExceptionPorts();
            }
        }
        else
        {
            BugsnagKSLOG_DEBUG("This is the secondary exception thread. Restoring original exception ports.");
            bugsnag_ksmachexc_i_restoreExceptionPorts();
        }

        if(wasHandlingCrash)
        {
            BugsnagKSLOG_INFO("Detected crash in the crash reporter. Restoring original handlers.");
            // The crash reporter itself crashed. Make a note of this and
            // uninstall all handlers so that we don't get stuck in a loop.
            g_context->crashedDuringCrashHandling = true;
            bugsnag_kscrashsentry_uninstall(BugsnagKSCrashTypeAsyncSafe);
        }

        // Fill out crash information
        BugsnagKSLOG_DEBUG("Fetching machine state.");
        STRUCT_MCONTEXT_L machineContext;
        if(bugsnag_ksmachexc_i_fetchMachineState(exceptionMessage.thread.name, &machineContext))
        {
            if(exceptionMessage.exception == EXC_BAD_ACCESS)
            {
                g_context->faultAddress = bugsnag_ksmach_faultAddress(&machineContext);
            }
            else
            {
                g_context->faultAddress = bugsnag_ksmach_instructionAddress(&machineContext);
            }
        }

        BugsnagKSLOG_DEBUG("Filling out context.");
        g_context->crashType = BugsnagKSCrashTypeMachException;
        g_context->offendingThread = exceptionMessage.thread.name;
        g_context->registersAreValid = true;
        g_context->mach.type = exceptionMessage.exception;
        g_context->mach.code = exceptionMessage.code[0];
        g_context->mach.subcode = exceptionMessage.code[1];


        BugsnagKSLOG_DEBUG("Calling main crash handler.");
        g_context->onCrash();


        BugsnagKSLOG_DEBUG("Crash handling complete. Restoring original handlers.");
        bugsnag_kscrashsentry_uninstall(BugsnagKSCrashTypeAsyncSafe);
        bugsnag_kscrashsentry_resumeThreads();
    }

    BugsnagKSLOG_DEBUG("Replying to mach exception message.");
    // Send a reply saying "I didn't handle this exception".
    replyMessage.header = exceptionMessage.header;
    replyMessage.NDR = exceptionMessage.NDR;
    replyMessage.returnCode = KERN_FAILURE;

    mach_msg(&replyMessage.header,
             MACH_SEND_MSG,
             sizeof(replyMessage),
             0,
             MACH_PORT_NULL,
             MACH_MSG_TIMEOUT_NONE,
             MACH_PORT_NULL);

    return NULL;
}


// ============================================================================
#pragma mark - API -
// ============================================================================

bool bugsnag_kscrashsentry_installMachHandler(BugsnagKSCrash_SentryContext* const context)
{
    BugsnagKSLOG_DEBUG("Installing mach exception handler.");

    bool attributes_created = false;
    pthread_attr_t attr;

    kern_return_t kr;
    int error;

    const task_t thisTask = mach_task_self();
    exception_mask_t mask = EXC_MASK_BAD_ACCESS |
    EXC_MASK_BAD_INSTRUCTION |
    EXC_MASK_ARITHMETIC |
    EXC_MASK_SOFTWARE |
    EXC_MASK_BREAKPOINT;

    if(g_installed)
    {
        BugsnagKSLOG_DEBUG("Exception handler already installed.");
        return true;
    }
    g_installed = 1;

    if(bugsnag_ksmach_isBeingTraced())
    {
        // Different debuggers hook into different exception types.
        // For example, GDB uses EXC_BAD_ACCESS for single stepping,
        // and LLDB uses EXC_SOFTWARE to stop a debug session.
        // Because of this, it's safer to not hook into the mach exception
        // system at all while being debugged.
        BugsnagKSLOG_WARN("Process is being debugged. Not installing handler.");
        goto failed;
    }

    g_context = context;

    BugsnagKSLOG_DEBUG("Backing up original exception ports.");
    kr = task_get_exception_ports(thisTask,
                                  mask,
                                  g_previousExceptionPorts.masks,
                                  &g_previousExceptionPorts.count,
                                  g_previousExceptionPorts.ports,
                                  g_previousExceptionPorts.behaviors,
                                  g_previousExceptionPorts.flavors);
    if(kr != KERN_SUCCESS)
    {
        BugsnagKSLOG_ERROR("task_get_exception_ports: %s", mach_error_string(kr));
        goto failed;
    }

    if(g_exceptionPort == MACH_PORT_NULL)
    {
        BugsnagKSLOG_DEBUG("Allocating new port with receive rights.");
        kr = mach_port_allocate(thisTask,
                                MACH_PORT_RIGHT_RECEIVE,
                                &g_exceptionPort);
        if(kr != KERN_SUCCESS)
        {
            BugsnagKSLOG_ERROR("mach_port_allocate: %s", mach_error_string(kr));
            goto failed;
        }

        BugsnagKSLOG_DEBUG("Adding send rights to port.");
        kr = mach_port_insert_right(thisTask,
                                    g_exceptionPort,
                                    g_exceptionPort,
                                    MACH_MSG_TYPE_MAKE_SEND);
        if(kr != KERN_SUCCESS)
        {
            BugsnagKSLOG_ERROR("mach_port_insert_right: %s", mach_error_string(kr));
            goto failed;
        }
    }

    BugsnagKSLOG_DEBUG("Installing port as exception handler.");
    kr = task_set_exception_ports(thisTask,
                                  mask,
                                  g_exceptionPort,
                                  EXCEPTION_DEFAULT,
                                  THREAD_STATE_NONE);
    if(kr != KERN_SUCCESS)
    {
        BugsnagKSLOG_ERROR("task_set_exception_ports: %s", mach_error_string(kr));
        goto failed;
    }

    BugsnagKSLOG_DEBUG("Creating secondary exception thread (suspended).");
    pthread_attr_init(&attr);
    attributes_created = true;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    error = pthread_create(&g_secondaryPThread,
                           &attr,
                           &bugsnag_ksmachexc_i_handleExceptions,
                           kThreadSecondary);
    if(error != 0)
    {
        BugsnagKSLOG_ERROR("pthread_create_suspended_np: %s", strerror(error));
        goto failed;
    }
    g_secondaryMachThread = pthread_mach_thread_np(g_secondaryPThread);
    context->reservedThreads[BugsnagKSCrashReservedThreadTypeMachSecondary] = g_secondaryMachThread;

    BugsnagKSLOG_DEBUG("Creating primary exception thread.");
    error = pthread_create(&g_primaryPThread,
                           &attr,
                           &bugsnag_ksmachexc_i_handleExceptions,
                           kThreadPrimary);
    if(error != 0)
    {
        BugsnagKSLOG_ERROR("pthread_create: %s", strerror(error));
        goto failed;
    }
    pthread_attr_destroy(&attr);
    g_primaryMachThread = pthread_mach_thread_np(g_primaryPThread);
    context->reservedThreads[BugsnagKSCrashReservedThreadTypeMachPrimary] = g_primaryMachThread;

    BugsnagKSLOG_DEBUG("Mach exception handler installed.");
    return true;


failed:
    BugsnagKSLOG_DEBUG("Failed to install mach exception handler.");
    if(attributes_created)
    {
        pthread_attr_destroy(&attr);
    }
    bugsnag_kscrashsentry_uninstallMachHandler();
    return false;
}


void bugsnag_kscrashsentry_uninstallMachHandler(void)
{
    BugsnagKSLOG_DEBUG("Uninstalling mach exception handler.");

    if(!g_installed)
    {
        BugsnagKSLOG_DEBUG("Mach exception handler was already uninstalled.");
        return;
    }

    // NOTE: Do not deallocate the exception port. If a secondary crash occurs
    // it will hang the process.

    bugsnag_ksmachexc_i_restoreExceptionPorts();

    thread_t thread_self = bugsnag_ksmach_thread_self();

    if(g_primaryPThread != 0 && g_primaryMachThread != thread_self)
    {
        BugsnagKSLOG_DEBUG("Cancelling primary exception thread.");
        if(g_context->handlingCrash)
        {
            thread_terminate(g_primaryMachThread);
        }
        else
        {
            pthread_cancel(g_primaryPThread);
        }
        g_primaryMachThread = 0;
        g_primaryPThread = 0;
    }
    if(g_secondaryPThread != 0 && g_secondaryMachThread != thread_self)
    {
        BugsnagKSLOG_DEBUG("Cancelling secondary exception thread.");
        if(g_context->handlingCrash)
        {
            thread_terminate(g_secondaryMachThread);
        }
        else
        {
            pthread_cancel(g_secondaryPThread);
        }
        g_secondaryMachThread = 0;
        g_secondaryPThread = 0;
    }

    BugsnagKSLOG_DEBUG("Mach exception handlers uninstalled.");
    g_installed = 0;
}
