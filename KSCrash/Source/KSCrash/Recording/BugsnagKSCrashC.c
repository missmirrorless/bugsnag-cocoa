//
//  BugsnagKSCrashC.c
//
//  Created by Karl Stenerud on 2012-01-28.
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


#include "BugsnagKSCrashC.h"

#include "BugsnagKSCrashReport.h"
#include "BugsnagKSString.h"
#include "BugsnagKSMach.h"
#include "BugsnagKSObjC.h"
#include "BugsnagKSSignalInfo.h"
#include "BugsnagKSSystemInfoC.h"
#include "BugsnagKSZombie.h"
#include "BugsnagKSCrashSentry_Deadlock.h"
#include "BugsnagKSCrashSentry_User.h"

//#define BugsnagKSLogger_LocalLevel TRACE
#include "BugsnagKSLogger.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


// ============================================================================
#pragma mark - Globals -
// ============================================================================

/** True if BugsnagKSCrash has been installed. */
static volatile sig_atomic_t g_installed = 0;

/** Single, global crash context. */
static BugsnagKSCrash_Context g_crashReportContext =
{
    .config =
    {
        .handlingCrashTypes = BugsnagKSCrashTypeProductionSafe
    }
};

/** Path to store the next crash report. */
static char* g_crashReportFilePath;

/** Path to store the next crash report (only if the crash manager crashes). */
static char* g_recrashReportFilePath;

/** Path to store the state file. */
static char* g_stateFilePath;


// ============================================================================
#pragma mark - Utility -
// ============================================================================

static inline BugsnagKSCrash_Context* crashContext(void)
{
    return &g_crashReportContext;
}


// ============================================================================
#pragma mark - Callbacks -
// ============================================================================

// Avoiding static methods due to linker issue.

/** Called when a crash occurs.
 *
 * This function gets passed as a callback to a crash handler.
 */
void bugsnag_kscrash_i_onCrash(void)
{
    BugsnagKSLOG_DEBUG("Updating application state to note crash.");
    bugsnag_kscrashstate_notifyAppCrash();

    BugsnagKSCrash_Context* context = crashContext();

    if(context->config.printTraceToStdout)
    {
        bugsnag_kscrashreport_logCrash(context);
    }

    if(context->crash.crashedDuringCrashHandling)
    {
        bugsnag_kscrashreport_writeMinimalReport(context, g_recrashReportFilePath);
    }
    else
    {
        bugsnag_kscrashreport_writeStandardReport(context, g_crashReportFilePath);
    }
}


// ============================================================================
#pragma mark - API -
// ============================================================================

BugsnagKSCrashType bugsnag_kscrash_install(const char* const crashReportFilePath,
                            const char* const recrashReportFilePath,
                            const char* stateFilePath,
                            const char* crashID)
{
    BugsnagKSLOG_DEBUG("Installing crash reporter.");

    BugsnagKSCrash_Context* context = crashContext();

    if(g_installed)
    {
        BugsnagKSLOG_DEBUG("Crash reporter already installed.");
        return context->config.handlingCrashTypes;
    }
    g_installed = 1;

    bugsnag_ksmach_init();
    bugsnag_ksobjc_init();

    bugsnag_kscrash_reinstall(crashReportFilePath,
                      recrashReportFilePath,
                      stateFilePath,
                      crashID);


    BugsnagKSCrashType crashTypes = bugsnag_kscrash_setHandlingCrashTypes(context->config.handlingCrashTypes);

    context->config.systemInfoJSON = bugsnag_kssysteminfo_toJSON();
    context->config.processName = bugsnag_kssysteminfo_copyProcessName();

    BugsnagKSLOG_DEBUG("Installation complete.");
    return crashTypes;
}

void bugsnag_kscrash_reinstall(const char* const crashReportFilePath,
                       const char* const recrashReportFilePath,
                       const char* const stateFilePath,
                       const char* const crashID)
{
    BugsnagKSLOG_TRACE("reportFilePath = %s", crashReportFilePath);
    BugsnagKSLOG_TRACE("secondaryReportFilePath = %s", recrashReportFilePath);
    BugsnagKSLOG_TRACE("stateFilePath = %s", stateFilePath);
    BugsnagKSLOG_TRACE("crashID = %s", crashID);

    bugsnag_ksstring_replace((const char**)&g_stateFilePath, stateFilePath);
    bugsnag_ksstring_replace((const char**)&g_crashReportFilePath, crashReportFilePath);
    bugsnag_ksstring_replace((const char**)&g_recrashReportFilePath, recrashReportFilePath);
    BugsnagKSCrash_Context* context = crashContext();
    bugsnag_ksstring_replace(&context->config.crashID, crashID);

    if(!bugsnag_kscrashstate_init(g_stateFilePath, &context->state))
    {
        BugsnagKSLOG_ERROR("Failed to initialize persistent crash state");
    }
    context->state.appLaunchTime = mach_absolute_time();
}

BugsnagKSCrashType bugsnag_kscrash_setHandlingCrashTypes(BugsnagKSCrashType crashTypes)
{
    if((crashTypes & BugsnagKSCrashTypeDebuggerUnsafe) && bugsnag_ksmach_isBeingTraced())
    {
        BugsnagKSLOGBASIC_WARN("BugsnagKSCrash: App is running in a debugger. The following crash types have been disabled:");
        BugsnagKSCrashType disabledCrashTypes = crashTypes & BugsnagKSCrashTypeDebuggerUnsafe;
        for(int i = 0; i < 31; i++)
        {
            BugsnagKSCrashType type = 1 << i;
            if(disabledCrashTypes & type)
            {
                BugsnagKSLOGBASIC_WARN("* %s", bugsnag_kscrashtype_name(type));
            }
        }

        crashTypes &= BugsnagKSCrashTypeDebuggerSafe;
    }

    BugsnagKSCrash_Context* context = crashContext();
    context->config.handlingCrashTypes = crashTypes;

    if(g_installed)
    {
        bugsnag_kscrashsentry_uninstall(~crashTypes);
        crashTypes = bugsnag_kscrashsentry_installWithContext(&context->crash, crashTypes, bugsnag_kscrash_i_onCrash);
    }
    return crashTypes;
}

void bugsnag_kscrash_setUserInfoJSON(const char* const userInfoJSON)
{
    BugsnagKSLOG_TRACE("set userInfoJSON to %p", userInfoJSON);
    BugsnagKSCrash_Context* context = crashContext();
    bugsnag_ksstring_replace(&context->config.userInfoJSON, userInfoJSON);
}

void bugsnag_kscrash_setZombieCacheSize(size_t zombieCacheSize)
{
    bugsnag_kszombie_uninstall();
    if(zombieCacheSize > 0)
    {
        bugsnag_kszombie_install(zombieCacheSize);
    }
}

void bugsnag_kscrash_setDeadlockWatchdogInterval(double deadlockWatchdogInterval)
{
    bugsnag_kscrashsentry_setDeadlockHandlerWatchdogInterval(deadlockWatchdogInterval);
}

void bugsnag_kscrash_setPrintTraceToStdout(bool printTraceToStdout)
{
    crashContext()->config.printTraceToStdout = printTraceToStdout;
}

void bugsnag_kscrash_setSearchThreadNames(bool shouldSearchThreadNames)
{
    crashContext()->config.searchThreadNames = shouldSearchThreadNames;
}

void bugsnag_kscrash_setSearchQueueNames(bool shouldSearchQueueNames)
{
    crashContext()->config.searchQueueNames = shouldSearchQueueNames;
}

void bugsnag_kscrash_setIntrospectMemory(bool introspectMemory)
{
    crashContext()->config.introspectionRules.enabled = introspectMemory;
}

void bugsnag_kscrash_setDoNotIntrospectClasses(const char** doNotIntrospectClasses, size_t length)
{
    const char** oldClasses = crashContext()->config.introspectionRules.restrictedClasses;
    size_t oldClassesLength = crashContext()->config.introspectionRules.restrictedClassesCount;
    const char** newClasses = nil;
    size_t newClassesLength = 0;
    
    if(doNotIntrospectClasses != nil && length > 0)
    {
        newClassesLength = length;
        newClasses = malloc(sizeof(*newClasses) * newClassesLength);
        if(newClasses == nil)
        {
            BugsnagKSLOG_ERROR("Could not allocate memory");
            return;
        }
        
        for(size_t i = 0; i < newClassesLength; i++)
        {
            newClasses[i] = strdup(doNotIntrospectClasses[i]);
        }
    }

    crashContext()->config.introspectionRules.restrictedClasses = newClasses;
    crashContext()->config.introspectionRules.restrictedClassesCount = newClassesLength;

    if(oldClasses != nil)
    {
        for(size_t i = 0; i < oldClassesLength; i++)
        {
            free((void*)oldClasses[i]);
        }
        free(oldClasses);
    }
}

void bugsnag_kscrash_setSuspendThreadsForUserReported(bool suspendThreadsForUserReported)
{
    crashContext()->crash.suspendThreadsForUserReported = suspendThreadsForUserReported;
}

void bugsnag_kscrash_setCrashNotifyCallback(const BugsnagKSReportWriteCallback onCrashNotify)
{
    BugsnagKSLOG_TRACE("Set onCrashNotify to %p", onCrashNotify);
    crashContext()->config.onCrashNotify = onCrashNotify;
}

void bugsnag_kscrash_reportUserException(const char* name,
                                 const char* reason,
                                 const char* lineOfCode,
                                 const char** stackTrace,
                                 size_t stackTraceCount,
                                 bool terminateProgram)
{
    bugsnag_kscrashsentry_reportUserException(name,
                                      reason,
                                      lineOfCode,
                                      stackTrace,
                                      stackTraceCount,
                                      terminateProgram);
}
