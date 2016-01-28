//
//  BugsnagKSCrashReport.m
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


#include "BugsnagKSCrashReport.h"

#include "BugsnagKSBacktrace_Private.h"
#include "BugsnagKSCrashReportFields.h"
#include "BugsnagKSCrashReportWriter.h"
#include "BugsnagKSDynamicLinker.h"
#include "BugsnagKSFileUtils.h"
#include "BugsnagKSJSONCodec.h"
#include "BugsnagKSMach.h"
#include "BugsnagKSObjC.h"
#include "BugsnagKSSignalInfo.h"
#include "BugsnagKSZombie.h"
#include "BugsnagKSString.h"
#include "BugsnagDemangle.h"

//#define BugsnagKSLogger_LocalLevel TRACE
#include "BugsnagKSLogger.h"

#include <mach-o/dyld.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>


#ifdef __arm64__
    #include <sys/_types/_ucontext64.h>
    #define UC_MCONTEXT uc_mcontext64
    typedef ucontext64_t SignalUserContext;
#else
    #define UC_MCONTEXT uc_mcontext
    typedef ucontext_t SignalUserContext;
#endif


// Note: Avoiding static functions due to linker issues.


// ============================================================================
#pragma mark - Constants -
// ============================================================================

/** Version number written to the report. */
#define kReportVersionMajor 3
#define kReportVersionMinor 0

/** Maximum depth allowed for a backtrace. */
#define kMaxBacktraceDepth 150

/** Default number of objects, subobjects, and ivars to record from a memory loc */
#define kDefaultMemorySearchDepth 15

/** Length at which we consider a backtrace to represent a stack overflow.
 * If it reaches this point, we start cutting off from the top of the stack
 * rather than the bottom.
 */
#define kStackOverflowThreshold 200

/** Maximum number of lines to print when printing a stack trace to the console.
 */
#define kMaxStackTracePrintLines 40

/** How far to search the stack (in pointer sized jumps) for notable data. */
#define kStackNotableSearchBackDistance 20
#define kStackNotableSearchForwardDistance 10

/** How much of the stack to dump (in pointer sized jumps). */
#define kStackContentsPushedDistance 20
#define kStackContentsPoppedDistance 10
#define kStackContentsTotalDistance (kStackContentsPushedDistance + kStackContentsPoppedDistance)

/** The minimum length for a valid string. */
#define kMinStringLength 4

/** Leave lots of room for C++ demangling */
#define DEMANGLE_BUFFER_LENGTH 2000


// ============================================================================
#pragma mark - Formatting -
// ============================================================================

#if defined(__LP64__)
    #define TRACE_FMT         "%-4d%-31s 0x%016lx %s + %lu"
    #define POINTER_FMT       "0x%016lx"
    #define POINTER_SHORT_FMT "0x%lx"
#else
    #define TRACE_FMT         "%-4d%-31s 0x%08lx %s + %lu"
    #define POINTER_FMT       "0x%08lx"
    #define POINTER_SHORT_FMT "0x%lx"
#endif


// ============================================================================
#pragma mark - JSON Encoding -
// ============================================================================

#define getJsonContext(REPORT_WRITER) ((BugsnagKSJSONEncodeContext*)((REPORT_WRITER)->context))

/** Used for writing hex string values. */
static const char g_hexNybbles[] =
{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

// ============================================================================
#pragma mark - Runtime Config -
// ============================================================================

static BugsnagKSCrash_IntrospectionRules* g_introspectionRules;


#pragma mark Callbacks

void bugsnag_kscrw_i_addBooleanElement(const BugsnagKSCrashReportWriter* const writer,
                               const char* const key,
                               const bool value)
{
    bugsnag_ksjson_addBooleanElement(getJsonContext(writer), key, value);
}

void bugsnag_kscrw_i_addFloatingPointElement(const BugsnagKSCrashReportWriter* const writer,
                                     const char* const key,
                                     const double value)
{
    bugsnag_ksjson_addFloatingPointElement(getJsonContext(writer), key, value);
}

void bugsnag_kscrw_i_addIntegerElement(const BugsnagKSCrashReportWriter* const writer,
                               const char* const key,
                               const long long value)
{
    bugsnag_ksjson_addIntegerElement(getJsonContext(writer), key, value);
}

void bugsnag_kscrw_i_addUIntegerElement(const BugsnagKSCrashReportWriter* const writer,
                                const char* const key,
                                const unsigned long long value)
{
    bugsnag_ksjson_addIntegerElement(getJsonContext(writer), key, (long long)value);
}

void bugsnag_kscrw_i_addStringElement(const BugsnagKSCrashReportWriter* const writer,
                              const char* const key,
                              const char* const value)
{
    bugsnag_ksjson_addStringElement(getJsonContext(writer), key, value, strlen(value));
}

void bugsnag_kscrw_i_addTextFileElement(const BugsnagKSCrashReportWriter* const writer,
                                const char* const key,
                                const char* const filePath)
{
    const int fd = open(filePath, O_RDONLY);
    if(fd < 0)
    {
        BugsnagKSLOG_ERROR("Could not open file %s: %s", filePath, strerror(errno));
        return;
    }

    if(bugsnag_ksjson_beginStringElement(getJsonContext(writer), key) != BugsnagKSJSON_OK)
    {
        BugsnagKSLOG_ERROR("Could not start string element");
        goto done;
    }

    char buffer[512];
    ssize_t bytesRead;
    for(bytesRead = read(fd, buffer, sizeof(buffer));
        bytesRead > 0;
        bytesRead = read(fd, buffer, sizeof(buffer)))
    {
        if(bugsnag_ksjson_appendStringElement(getJsonContext(writer),
                                      buffer,
                                      (size_t)bytesRead) != BugsnagKSJSON_OK)
        {
            BugsnagKSLOG_ERROR("Could not append string element");
            goto done;
        }
    }

done:
    bugsnag_ksjson_endStringElement(getJsonContext(writer));
    close(fd);
}

void bugsnag_kscrw_i_addDataElement(const BugsnagKSCrashReportWriter* const writer,
                            const char* const key,
                            const char* const value,
                            const size_t length)
{
    bugsnag_ksjson_addDataElement(getJsonContext(writer), key, value, length);
}

void bugsnag_kscrw_i_beginDataElement(const BugsnagKSCrashReportWriter* const writer,
                              const char* const key)
{
    bugsnag_ksjson_beginDataElement(getJsonContext(writer), key);
}

void bugsnag_kscrw_i_appendDataElement(const BugsnagKSCrashReportWriter* const writer,
                               const char* const value,
                               const size_t length)
{
    bugsnag_ksjson_appendDataElement(getJsonContext(writer), value, length);
}

void bugsnag_kscrw_i_endDataElement(const BugsnagKSCrashReportWriter* const writer)
{
    bugsnag_ksjson_endDataElement(getJsonContext(writer));
}

void bugsnag_kscrw_i_addUUIDElement(const BugsnagKSCrashReportWriter* const writer,
                            const char* const key,
                            const unsigned char* const value)
{
    if(value == NULL)
    {
        bugsnag_ksjson_addNullElement(getJsonContext(writer), key);
    }
    else
    {
        char uuidBuffer[37];
        const unsigned char* src = value;
        char* dst = uuidBuffer;
        for(int i = 0; i < 4; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 2; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 2; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 2; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 6; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }

        bugsnag_ksjson_addStringElement(getJsonContext(writer),
                                key,
                                uuidBuffer,
                                (size_t)(dst - uuidBuffer));
    }
}

void bugsnag_kscrw_i_addJSONElement(const BugsnagKSCrashReportWriter* const writer,
                            const char* const key,
                            const char* const jsonElement)
{
    int jsonResult = bugsnag_ksjson_addJSONElement(getJsonContext(writer),
                                           key,
                                           jsonElement,
                                           strlen(jsonElement));
    if(jsonResult != BugsnagKSJSON_OK)
    {
        char errorBuff[100];
        snprintf(errorBuff,
                 sizeof(errorBuff),
                 "Invalid JSON data: %s",
                 bugsnag_ksjson_stringForError(jsonResult));
        bugsnag_ksjson_beginObject(getJsonContext(writer), key);
        bugsnag_ksjson_addStringElement(getJsonContext(writer),
                                BugsnagKSCrashField_Error,
                                errorBuff,
                                strlen(errorBuff));
        bugsnag_ksjson_addStringElement(getJsonContext(writer),
                                BugsnagKSCrashField_JSONData,
                                jsonElement,
                                strlen(jsonElement));
        bugsnag_ksjson_endContainer(getJsonContext(writer));
    }
}

void bugsnag_kscrw_i_beginObject(const BugsnagKSCrashReportWriter* const writer,
                         const char* const key)
{
    bugsnag_ksjson_beginObject(getJsonContext(writer), key);
}

void bugsnag_kscrw_i_beginArray(const BugsnagKSCrashReportWriter* const writer,
                        const char* const key)
{
    bugsnag_ksjson_beginArray(getJsonContext(writer), key);
}

void bugsnag_kscrw_i_endContainer(const BugsnagKSCrashReportWriter* const writer)
{
    bugsnag_ksjson_endContainer(getJsonContext(writer));
}

int bugsnag_kscrw_i_addJSONData(const char* const data,
                        const size_t length,
                        void* const userData)
{
    const int fd = *((int*)userData);
    const bool success = bugsnag_ksfu_writeBytesToFD(fd, data, (ssize_t)length);
    return success ? BugsnagKSJSON_OK : BugsnagKSJSON_ERROR_CANNOT_ADD_DATA;
}


// ============================================================================
#pragma mark - Utility -
// ============================================================================

/** Check if a memory address points to a valid null terminated UTF-8 string.
 *
 * @param address The address to check.
 *
 * @return true if the address points to a string.
 */
bool bugsnag_kscrw_i_isValidString(const void* const address)
{
    if((void*)address == NULL)
    {
        return false;
    }

    char buffer[500];
    if((uintptr_t)address+sizeof(buffer) < (uintptr_t)address)
    {
        // Wrapped around the address range.
        return false;
    }
    if(bugsnag_ksmach_copyMem(address, buffer, sizeof(buffer)) != KERN_SUCCESS)
    {
        return false;
    }
    return bugsnag_ksstring_isNullTerminatedUTF8String(buffer,
                                               kMinStringLength,
                                               sizeof(buffer));
}

/** Get all parts of the machine state required for a dump.
 * This includes basic thread state, and exception registers.
 *
 * @param thread The thread to get state for.
 *
 * @param machineContextBuffer The machine context to fill out.
 */
bool bugsnag_kscrw_i_fetchMachineState(const thread_t thread,
                               STRUCT_MCONTEXT_L* const machineContextBuffer)
{
    if(!bugsnag_ksmach_threadState(thread, machineContextBuffer))
    {
        return false;
    }

    if(!bugsnag_ksmach_exceptionState(thread, machineContextBuffer))
    {
        return false;
    }

    return true;
}

/** Get the machine context for the specified thread.
 *
 * This function will choose how to fetch the machine context based on what kind
 * of thread it is (current, crashed, other), and what kind of crash occured.
 * It may store the context in machineContextBuffer unless it can be fetched
 * directly from memory. Do not count on machineContextBuffer containing
 * anything. Always use the return value.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to get a machine context for.
 *
 * @param machineContextBuffer A place to store the context, if needed.
 *
 * @return A pointer to the crash context, or NULL if not found.
 */
STRUCT_MCONTEXT_L* bugsnag_kscrw_i_getMachineContext(const BugsnagKSCrash_SentryContext* const crash,
                                            const thread_t thread,
                                            STRUCT_MCONTEXT_L* const machineContextBuffer)
{
    if(thread == crash->offendingThread)
    {
        if(crash->crashType == BugsnagKSCrashTypeSignal)
        {
            return ((SignalUserContext*)crash->signal.userContext)->UC_MCONTEXT;
        }
    }

    if(thread == bugsnag_ksmach_thread_self())
    {
        return NULL;
    }

    if(!bugsnag_kscrw_i_fetchMachineState(thread, machineContextBuffer))
    {
        BugsnagKSLOG_ERROR("Failed to fetch machine state for thread %d", thread);
        return NULL;
    }

    return machineContextBuffer;
}

/** Get the backtrace for the specified thread.
 *
 * This function will choose how to fetch the backtrace based on machine context
 * availability andwhat kind of crash occurred. It may store the backtrace in
 * backtraceBuffer unless it can be fetched directly from memory. Do not count
 * on backtraceBuffer containing anything. Always use the return value.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to get a machine context for.
 *
 * @param machineContext The machine context (can be NULL).
 *
 * @param backtraceBuffer A place to store the backtrace, if needed.
 *
 * @param backtraceLength In: The length of backtraceBuffer.
 *                        Out: The length of the backtrace.
 *
 * @param skippedEntries: Out: The number of entries that were skipped due to
 *                             stack overflow.
 *
 * @return The backtrace, or NULL if not found.
 */
uintptr_t* bugsnag_kscrw_i_getBacktrace(const BugsnagKSCrash_SentryContext* const crash,
                                const thread_t thread,
                                const STRUCT_MCONTEXT_L* const machineContext,
                                uintptr_t* const backtraceBuffer,
                                int* const backtraceLength,
                                int* const skippedEntries)
{
    if(thread == crash->offendingThread)
    {
        if(crash->crashType & (BugsnagKSCrashTypeCPPException | BugsnagKSCrashTypeNSException | BugsnagKSCrashTypeUserReported))
        {
            *backtraceLength = crash->stackTraceLength;
            return crash->stackTrace;
        }
    }

    if(machineContext == NULL)
    {
        return NULL;
    }

    int actualSkippedEntries = 0;
    int actualLength = bugsnag_ksbt_backtraceLength(machineContext);
    if(actualLength >= kStackOverflowThreshold)
    {
        actualSkippedEntries = actualLength - *backtraceLength;
    }

    *backtraceLength = bugsnag_ksbt_backtraceThreadState(machineContext,
                                                 backtraceBuffer,
                                                 actualSkippedEntries,
                                                 *backtraceLength);
    if(skippedEntries != NULL)
    {
        *skippedEntries = actualSkippedEntries;
    }
    return backtraceBuffer;
}

/** Check if the stack for the specified thread has overflowed.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to check.
 *
 * @return true if the thread's stack has overflowed.
 */
bool bugsnag_kscrw_i_isStackOverflow(const BugsnagKSCrash_SentryContext* const crash,
                             const thread_t thread)
{
    STRUCT_MCONTEXT_L concreteMachineContext;
    STRUCT_MCONTEXT_L* machineContext = bugsnag_kscrw_i_getMachineContext(crash,
                                                                 thread,
                                                                 &concreteMachineContext);
    if(machineContext == NULL)
    {
        return false;
    }

    return bugsnag_ksbt_isBacktraceTooLong(machineContext, kStackOverflowThreshold);
}


// ============================================================================
#pragma mark - Console Logging -
// ============================================================================

/** Print the crash type and location to the log.
 *
 * @param sentryContext The crash sentry context.
 */
void bugsnag_kscrw_i_logCrashType(const BugsnagKSCrash_SentryContext* const sentryContext)
{
    switch(sentryContext->crashType)
    {
        case BugsnagKSCrashTypeMachException:
        {
            int machExceptionType = sentryContext->mach.type;
            kern_return_t machCode = (kern_return_t)sentryContext->mach.code;
            const char* machExceptionName = bugsnag_ksmach_exceptionName(machExceptionType);
            const char* machCodeName = machCode == 0 ? NULL : bugsnag_ksmach_kernelReturnCodeName(machCode);
            BugsnagKSLOGBASIC_INFO("App crashed due to mach exception: [%s: %s] at %p",
                            machExceptionName, machCodeName, sentryContext->faultAddress);
            break;
        }
        case BugsnagKSCrashTypeCPPException:
        {
            BugsnagKSLOG_INFO("App crashed due to C++ exception: %s: %s",
                       sentryContext->CPPException.name,
                       sentryContext->crashReason);
            break;
        }
        case BugsnagKSCrashTypeNSException:
        {
            BugsnagKSLOGBASIC_INFO("App crashed due to NSException: %s: %s",
                            sentryContext->NSException.name,
                            sentryContext->crashReason);
            break;
        }
        case BugsnagKSCrashTypeSignal:
        {
            int sigNum = sentryContext->signal.signalInfo->si_signo;
            int sigCode = sentryContext->signal.signalInfo->si_code;
            const char* sigName = bugsnag_kssignal_signalName(sigNum);
            const char* sigCodeName = bugsnag_kssignal_signalCodeName(sigNum, sigCode);
            BugsnagKSLOGBASIC_INFO("App crashed due to signal: [%s, %s] at %08x",
                            sigName, sigCodeName, sentryContext->faultAddress);
            break;
        }
        case BugsnagKSCrashTypeMainThreadDeadlock:
        {
            BugsnagKSLOGBASIC_INFO("Main thread deadlocked");
            break;
        }
        case BugsnagKSCrashTypeUserReported:
        {
            BugsnagKSLOG_INFO("App crashed due to user specified exception: %s", sentryContext->crashReason);
            break;
        }
    }
}

/** Print a backtrace entry in the standard format to the log.
 *
 * @param entryNum The backtrace entry number.
 *
 * @param address The program counter value (instruction address).
 *
 * @param dlInfo Information about the nearest symbols to the address.
 */
void bugsnag_kscrw_i_logBacktraceEntry(const int entryNum,
                               const uintptr_t address,
                               const Dl_info* const dlInfo)
{
    char faddrBuff[20];
    char saddrBuff[20];
    char demangleBuff[DEMANGLE_BUFFER_LENGTH];

    const char* fname = bugsnag_ksfu_lastPathEntry(dlInfo->dli_fname);
    if(fname == NULL)
    {
        sprintf(faddrBuff, POINTER_FMT, (uintptr_t)dlInfo->dli_fbase);
        fname = faddrBuff;
    }

    uintptr_t offset = address - (uintptr_t)dlInfo->dli_saddr;
    const char* sname = dlInfo->dli_sname;
    if(sname != NULL)
    {
        if(bugsnag_safe_demangle(sname, demangleBuff, sizeof(demangleBuff)) == DEMANGLE_STATUS_SUCCESS)
        {
            sname = demangleBuff;
        }
    }
    else
    {
        sprintf(saddrBuff, POINTER_SHORT_FMT, (uintptr_t)dlInfo->dli_fbase);
        sname = saddrBuff;
        offset = address - (uintptr_t)dlInfo->dli_fbase;
    }

    BugsnagKSLOGBASIC_ALWAYS(TRACE_FMT, entryNum, fname, address, sname, offset);
}

/** Print a backtrace to the log.
 *
 * @param backtrace The backtrace to print.
 *
 * @param backtraceLength The length of the backtrace.
 */
void bugsnag_kscrw_i_logBacktrace(const uintptr_t* const backtrace,
                          const int backtraceLength,
                          const int skippedEntries)
{
    if(backtraceLength > 0)
    {
        Dl_info symbolicated[backtraceLength];
        bugsnag_ksbt_symbolicate(backtrace, symbolicated, backtraceLength, skippedEntries);

        for(int i = 0; i < backtraceLength; i++)
        {
            bugsnag_kscrw_i_logBacktraceEntry(i, backtrace[i], &symbolicated[i]);
        }
    }
}

/** Print the backtrace for the crashed thread to the log.
 *
 * @param crash The crash handler context.
 */
void bugsnag_kscrw_i_logCrashThreadBacktrace(const BugsnagKSCrash_SentryContext* const crash)
{
    thread_t thread = crash->offendingThread;
    STRUCT_MCONTEXT_L concreteMachineContext;
    uintptr_t concreteBacktrace[kMaxStackTracePrintLines];
    int backtraceLength = sizeof(concreteBacktrace) / sizeof(*concreteBacktrace);

    STRUCT_MCONTEXT_L* machineContext = bugsnag_kscrw_i_getMachineContext(crash,
                                                                 thread,
                                                                 &concreteMachineContext);

    int skippedEntries;
    uintptr_t* backtrace = bugsnag_kscrw_i_getBacktrace(crash,
                                                thread,
                                                machineContext,
                                                concreteBacktrace,
                                                &backtraceLength,
                                                &skippedEntries);

    if(backtrace != NULL)
    {
        bugsnag_kscrw_i_logBacktrace(backtrace, backtraceLength, skippedEntries);
    }
}


// ============================================================================
#pragma mark - Report Writing -
// ============================================================================

/** Write the contents of a memory location.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeMemoryContents(const BugsnagKSCrashReportWriter* const writer,
                                 const char* const key,
                                 const uintptr_t address,
                                 int* limit);

/** Write a string to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeNSStringContents(const BugsnagKSCrashReportWriter* const writer,
                                   const char* const key,
                                   const uintptr_t objectAddress,
                                   __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    char buffer[200];
    if(bugsnag_ksobjc_copyStringContents(object, buffer, sizeof(buffer)))
    {
        writer->addStringElement(writer, key, buffer);
    }
}

/** Write a URL to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeURLContents(const BugsnagKSCrashReportWriter* const writer,
                              const char* const key,
                              const uintptr_t objectAddress,
                              __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    char buffer[200];
    if(bugsnag_ksobjc_copyStringContents(object, buffer, sizeof(buffer)))
    {
        writer->addStringElement(writer, key, buffer);
    }
}

/** Write a date to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeDateContents(const BugsnagKSCrashReportWriter* const writer,
                               const char* const key,
                               const uintptr_t objectAddress,
                               __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    writer->addFloatingPointElement(writer, key, bugsnag_ksobjc_dateContents(object));
}

/** Write a number to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeNumberContents(const BugsnagKSCrashReportWriter* const writer,
                               const char* const key,
                               const uintptr_t objectAddress,
                               __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    writer->addFloatingPointElement(writer, key, bugsnag_ksobjc_numberAsFloat(object));
}

/** Write an array to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeArrayContents(const BugsnagKSCrashReportWriter* const writer,
                                const char* const key,
                                const uintptr_t objectAddress,
                                int* limit)
{
    const void* object = (const void*)objectAddress;
    uintptr_t firstObject;
    if(bugsnag_ksobjc_arrayContents(object, &firstObject, 1) == 1)
    {
        bugsnag_kscrw_i_writeMemoryContents(writer, key, firstObject, limit);
    }
}

/** Write out ivar information about an unknown object.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeUnknownObjectContents(const BugsnagKSCrashReportWriter* const writer,
                                        const char* const key,
                                        const uintptr_t objectAddress,
                                        int* limit)
{
    (*limit)--;
    const void* object = (const void*)objectAddress;
    BugsnagKSObjCIvar ivars[10];
    char s8;
    short s16;
    int sInt;
    long s32;
    long long s64;
    unsigned char u8;
    unsigned short u16;
    unsigned int uInt;
    unsigned long u32;
    unsigned long long u64;
    float f32;
    double f64;
    _Bool b;
    void* pointer;
    
    
    writer->beginObject(writer, key);
    {
        const void* class = bugsnag_ksobjc_isaPointer(object);
        size_t ivarCount = bugsnag_ksobjc_ivarList(class, ivars, sizeof(ivars)/sizeof(*ivars));
        *limit -= (int)ivarCount;
        for(size_t i = 0; i < ivarCount; i++)
        {
            BugsnagKSObjCIvar* ivar = &ivars[i];
            switch(ivar->type[0])
            {
                case 'c':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &s8);
                    writer->addIntegerElement(writer, ivar->name, s8);
                    break;
                case 'i':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &sInt);
                    writer->addIntegerElement(writer, ivar->name, sInt);
                    break;
                case 's':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &s16);
                    writer->addIntegerElement(writer, ivar->name, s16);
                    break;
                case 'l':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &s32);
                    writer->addIntegerElement(writer, ivar->name, s32);
                    break;
                case 'q':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &s64);
                    writer->addIntegerElement(writer, ivar->name, s64);
                    break;
                case 'C':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &u8);
                    writer->addUIntegerElement(writer, ivar->name, u8);
                    break;
                case 'I':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &uInt);
                    writer->addUIntegerElement(writer, ivar->name, uInt);
                    break;
                case 'S':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &u16);
                    writer->addUIntegerElement(writer, ivar->name, u16);
                    break;
                case 'L':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &u32);
                    writer->addUIntegerElement(writer, ivar->name, u32);
                    break;
                case 'Q':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &u64);
                    writer->addUIntegerElement(writer, ivar->name, u64);
                    break;
                case 'f':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &f32);
                    writer->addFloatingPointElement(writer, ivar->name, f32);
                    break;
                case 'd':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &f64);
                    writer->addFloatingPointElement(writer, ivar->name, f64);
                    break;
                case 'B':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &b);
                    writer->addBooleanElement(writer, ivar->name, b);
                    break;
                case '*':
                case '@':
                case '#':
                case ':':
                    bugsnag_ksobjc_ivarValue(object, ivar->index, &pointer);
                    bugsnag_kscrw_i_writeMemoryContents(writer, ivar->name, (uintptr_t)pointer, limit);
                    break;
                default:
                    BugsnagKSLOG_DEBUG("%s: Unknown ivar type [%s]", ivar->name, ivar->type);
            }
        }
    }
    writer->endContainer(writer);
}

bool bugsnag_kscrw_i_isRestrictedClass(const char* name)
{
    if(g_introspectionRules->restrictedClasses != NULL)
    {
        for(size_t i = 0; i < g_introspectionRules->restrictedClassesCount; i++)
        {
            if(strcmp(name, g_introspectionRules->restrictedClasses[i]) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

/** Write the contents of a memory location.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void bugsnag_kscrw_i_writeMemoryContents(const BugsnagKSCrashReportWriter* const writer,
                                 const char* const key,
                                 const uintptr_t address,
                                 int* limit)
{
    (*limit)--;
    const void* object = (const void*)address;
    const void* class;
    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, BugsnagKSCrashField_Address, address);
        const char* zombieClassName = bugsnag_kszombie_className(object);
        if(zombieClassName != NULL)
        {
            writer->addStringElement(writer, BugsnagKSCrashField_LastDeallocObject, zombieClassName);
        }
        switch(bugsnag_ksobjc_objectType(object))
        {
            case BugsnagKSObjCTypeUnknown:
                if(object == NULL)
                {
                    writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashMemType_NullPointer);
                }
                else if(bugsnag_kscrw_i_isValidString(object))
                {
                    writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashMemType_String);
                    writer->addStringElement(writer, BugsnagKSCrashField_Value, (const char*)object);
                }
                else
                {
                    writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashMemType_Unknown);
                }
                break;
            case BugsnagKSObjCTypeClass:
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashMemType_Class);
                writer->addStringElement(writer, BugsnagKSCrashField_Class, bugsnag_ksobjc_className(object));
                break;
            case BugsnagKSObjCTypeObject:
            {
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashMemType_Object);
                class = bugsnag_ksobjc_isaPointer(object);
                const char* className = bugsnag_ksobjc_className(class);
                writer->addStringElement(writer, BugsnagKSCrashField_Class, className);
                if(!bugsnag_kscrw_i_isRestrictedClass(className))
                {
                    switch(bugsnag_ksobjc_objectClassType(object))
                    {
                        case BugsnagKSObjCClassTypeString:
                            bugsnag_kscrw_i_writeNSStringContents(writer, BugsnagKSCrashField_Value, address, limit);
                            break;
                        case BugsnagKSObjCClassTypeURL:
                            bugsnag_kscrw_i_writeURLContents(writer, BugsnagKSCrashField_Value, address, limit);
                            break;
                        case BugsnagKSObjCClassTypeDate:
                            bugsnag_kscrw_i_writeDateContents(writer, BugsnagKSCrashField_Value, address, limit);
                            break;
                        case BugsnagKSObjCClassTypeArray:
                            if(*limit > 0)
                            {
                                bugsnag_kscrw_i_writeArrayContents(writer, BugsnagKSCrashField_FirstObject, address, limit);
                            }
                            break;
                        case BugsnagKSObjCClassTypeNumber:
                            bugsnag_kscrw_i_writeNumberContents(writer, BugsnagKSCrashField_Value, address, limit);
                            break;
                        case BugsnagKSObjCClassTypeDictionary:
                        case BugsnagKSObjCClassTypeException:
                            // TODO: Implement these.
                            if(*limit > 0)
                            {
                                bugsnag_kscrw_i_writeUnknownObjectContents(writer, BugsnagKSCrashField_Ivars, address, limit);
                            }
                            break;
                        case BugsnagKSObjCClassTypeUnknown:
                            if(*limit > 0)
                            {
                                bugsnag_kscrw_i_writeUnknownObjectContents(writer, BugsnagKSCrashField_Ivars, address, limit);
                            }
                            break;
                    }
                }
                break;
            }
            case BugsnagKSObjCTypeBlock:
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashMemType_Block);
                class = bugsnag_ksobjc_isaPointer(object);
                writer->addStringElement(writer, BugsnagKSCrashField_Class, bugsnag_ksobjc_className(class));
                break;
        }
    }
    writer->endContainer(writer);
}

/** Write the contents of a memory location only if it contains notable data.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 */
void bugsnag_kscrw_i_writeMemoryContentsIfNotable(const BugsnagKSCrashReportWriter* const writer,
                                          const char* const key,
                                          const uintptr_t address)
{
    const void* object = (const void*)address;
    if(object == NULL)
    {
        return;
    }
    
    if(bugsnag_ksobjc_objectType(object) == BugsnagKSObjCTypeUnknown &&
       bugsnag_kszombie_className(object) == NULL &&
       !bugsnag_kscrw_i_isValidString(object))
    {
        // Nothing notable about this memory location.
        return;
    }

    int limit = kDefaultMemorySearchDepth;
    bugsnag_kscrw_i_writeMemoryContents(writer, key, address, &limit);
}

/** Look for a hex value in a string and try to write whatever it references.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param string The string to search.
 */
void bugsnag_kscrw_i_writeAddressReferencedByString(const BugsnagKSCrashReportWriter* const writer,
                                            const char* const key,
                                            const char* string)
{
    uint64_t address = 0;
    if(string == NULL || !bugsnag_ksstring_extractHexValue(string, strlen(string), &address))
    {
        return;
    }
    
    int limit = kDefaultMemorySearchDepth;
    bugsnag_kscrw_i_writeMemoryContents(writer, key, (uintptr_t)address, &limit);
}

#pragma mark Backtrace

/** Write a backtrace entry to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param dlInfo Information about the nearest symbols to the address.
 */
void bugsnag_kscrw_i_writeBacktraceEntry(const BugsnagKSCrashReportWriter* const writer,
                                 const char* const key,
                                 const uintptr_t address,
                                 const Dl_info* const info)
{
    char demangleBuff[DEMANGLE_BUFFER_LENGTH];
    writer->beginObject(writer, key);
    {
        if(info->dli_fname != NULL)
        {
            writer->addStringElement(writer, BugsnagKSCrashField_ObjectName, bugsnag_ksfu_lastPathEntry(info->dli_fname));
        }
        writer->addUIntegerElement(writer, BugsnagKSCrashField_ObjectAddr, (uintptr_t)info->dli_fbase);
        if(info->dli_sname != NULL)
        {
            const char* sname = info->dli_sname;
            if(bugsnag_safe_demangle(sname, demangleBuff, sizeof(demangleBuff)) == DEMANGLE_STATUS_SUCCESS)
            {
                sname = demangleBuff;
            }
            writer->addStringElement(writer, BugsnagKSCrashField_SymbolName, sname);
        }
        writer->addUIntegerElement(writer, BugsnagKSCrashField_SymbolAddr, (uintptr_t)info->dli_saddr);
        writer->addUIntegerElement(writer, BugsnagKSCrashField_InstructionAddr, address);
    }
    writer->endContainer(writer);
}

/** Write a backtrace to the report.
 *
 * @param writer The writer to write the backtrace to.
 *
 * @param key The object key, if needed.
 *
 * @param backtrace The backtrace to write.
 *
 * @param backtraceLength Length of the backtrace.
 *
 * @param skippedEntries The number of entries that were skipped before the
 *                       beginning of backtrace.
 */
void bugsnag_kscrw_i_writeBacktrace(const BugsnagKSCrashReportWriter* const writer,
                            const char* const key,
                            const uintptr_t* const backtrace,
                            const int backtraceLength,
                            const int skippedEntries)
{
    writer->beginObject(writer, key);
    {
        writer->beginArray(writer, BugsnagKSCrashField_Contents);
        {
            if(backtraceLength > 0)
            {
                Dl_info symbolicated[backtraceLength];
                bugsnag_ksbt_symbolicate(backtrace, symbolicated, backtraceLength, skippedEntries);

                for(int i = 0; i < backtraceLength; i++)
                {
                    bugsnag_kscrw_i_writeBacktraceEntry(writer,
                                                NULL,
                                                backtrace[i],
                                                &symbolicated[i]);
                }
            }
        }
        writer->endContainer(writer);
        writer->addIntegerElement(writer, BugsnagKSCrashField_Skipped, skippedEntries);
    }
    writer->endContainer(writer);
}

#pragma mark Stack

/** Write a dump of the stack contents to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the stack from.
 *
 * @param isStackOverflow If true, the stack has overflowed.
 */
void bugsnag_kscrw_i_writeStackContents(const BugsnagKSCrashReportWriter* const writer,
                                const char* const key,
                                const STRUCT_MCONTEXT_L* const machineContext,
                                const bool isStackOverflow)
{
    uintptr_t sp = bugsnag_ksmach_stackPointer(machineContext);
    if((void*)sp == NULL)
    {
        return;
    }

    uintptr_t lowAddress = sp + (uintptr_t)(kStackContentsPushedDistance * (int)sizeof(sp) * bugsnag_ksmach_stackGrowDirection() * -1);
    uintptr_t highAddress = sp + (uintptr_t)(kStackContentsPoppedDistance * (int)sizeof(sp) * bugsnag_ksmach_stackGrowDirection());
    if(highAddress < lowAddress)
    {
        uintptr_t tmp = lowAddress;
        lowAddress = highAddress;
        highAddress = tmp;
    }
    writer->beginObject(writer, key);
    {
        writer->addStringElement(writer, BugsnagKSCrashField_GrowDirection, bugsnag_ksmach_stackGrowDirection() > 0 ? "+" : "-");
        writer->addUIntegerElement(writer, BugsnagKSCrashField_DumpStart, lowAddress);
        writer->addUIntegerElement(writer, BugsnagKSCrashField_DumpEnd, highAddress);
        writer->addUIntegerElement(writer, BugsnagKSCrashField_StackPtr, sp);
        writer->addBooleanElement(writer, BugsnagKSCrashField_Overflow, isStackOverflow);
        uint8_t stackBuffer[kStackContentsTotalDistance * sizeof(sp)];
        size_t copyLength = highAddress - lowAddress;
        if(bugsnag_ksmach_copyMem((void*)lowAddress, stackBuffer, copyLength) == KERN_SUCCESS)
        {
            writer->addDataElement(writer, BugsnagKSCrashField_Contents, (void*)stackBuffer, copyLength);
        }
        else
        {
            writer->addStringElement(writer, BugsnagKSCrashField_Error, "Stack contents not accessible");
        }
    }
    writer->endContainer(writer);
}

/** Write any notable addresses near the stack pointer (above and below).
 *
 * @param writer The writer.
 *
 * @param machineContext The context to retrieve the stack from.
 *
 * @param backDistance The distance towards the beginning of the stack to check.
 *
 * @param forwardDistance The distance past the end of the stack to check.
 */
void bugsnag_kscrw_i_writeNotableStackContents(const BugsnagKSCrashReportWriter* const writer,
                                       const STRUCT_MCONTEXT_L* const machineContext,
                                       const int backDistance,
                                       const int forwardDistance)
{
    uintptr_t sp = bugsnag_ksmach_stackPointer(machineContext);
    if((void*)sp == NULL)
    {
        return;
    }

    uintptr_t lowAddress = sp + (uintptr_t)(backDistance * (int)sizeof(sp) * bugsnag_ksmach_stackGrowDirection() * -1);
    uintptr_t highAddress = sp + (uintptr_t)(forwardDistance * (int)sizeof(sp) * bugsnag_ksmach_stackGrowDirection());
    if(highAddress < lowAddress)
    {
        uintptr_t tmp = lowAddress;
        lowAddress = highAddress;
        highAddress = tmp;
    }
    uintptr_t contentsAsPointer;
    char nameBuffer[40];
    for(uintptr_t address = lowAddress; address < highAddress; address += sizeof(address))
    {
        if(bugsnag_ksmach_copyMem((void*)address, &contentsAsPointer, sizeof(contentsAsPointer)) == KERN_SUCCESS)
        {
            sprintf(nameBuffer, "stack@%p", (void*)address);
            bugsnag_kscrw_i_writeMemoryContentsIfNotable(writer, nameBuffer, contentsAsPointer);
        }
    }
}


#pragma mark Registers

/** Write the contents of all regular registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void bugsnag_kscrw_i_writeBasicRegisters(const BugsnagKSCrashReportWriter* const writer,
                                 const char* const key,
                                 const STRUCT_MCONTEXT_L* const machineContext)
{
    char registerNameBuff[30];
    const char* registerName;
    writer->beginObject(writer, key);
    {
        const int numRegisters = bugsnag_ksmach_numRegisters();
        for(int reg = 0; reg < numRegisters; reg++)
        {
            registerName = bugsnag_ksmach_registerName(reg);
            if(registerName == NULL)
            {
                snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
                registerName = registerNameBuff;
            }
            writer->addUIntegerElement(writer, registerName,
                                       bugsnag_ksmach_registerValue(machineContext, reg));
        }
    }
    writer->endContainer(writer);
}

/** Write the contents of all exception registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void bugsnag_kscrw_i_writeExceptionRegisters(const BugsnagKSCrashReportWriter* const writer,
                                     const char* const key,
                                     const STRUCT_MCONTEXT_L* const machineContext)
{
    char registerNameBuff[30];
    const char* registerName;
    writer->beginObject(writer, key);
    {
        const int numRegisters = bugsnag_ksmach_numExceptionRegisters();
        for(int reg = 0; reg < numRegisters; reg++)
        {
            registerName = bugsnag_ksmach_exceptionRegisterName(reg);
            if(registerName == NULL)
            {
                snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
                registerName = registerNameBuff;
            }
            writer->addUIntegerElement(writer,registerName,
                                       bugsnag_ksmach_exceptionRegisterValue(machineContext, reg));
        }
    }
    writer->endContainer(writer);
}

/** Write all applicable registers.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 *
 * @param isCrashedThread If true, this context represents the crashing thread.
 */
void bugsnag_kscrw_i_writeRegisters(const BugsnagKSCrashReportWriter* const writer,
                            const char* const key,
                            const STRUCT_MCONTEXT_L* const machineContext,
                            const bool isCrashedContext)
{
    writer->beginObject(writer, key);
    {
        bugsnag_kscrw_i_writeBasicRegisters(writer, BugsnagKSCrashField_Basic, machineContext);
        if(isCrashedContext)
        {
            bugsnag_kscrw_i_writeExceptionRegisters(writer,
                                            BugsnagKSCrashField_Exception,
                                            machineContext);
        }
    }
    writer->endContainer(writer);
}

/** Write any notable addresses contained in the CPU registers.
 *
 * @param writer The writer.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void bugsnag_kscrw_i_writeNotableRegisters(const BugsnagKSCrashReportWriter* const writer,
                                   const STRUCT_MCONTEXT_L* const machineContext)
{
    char registerNameBuff[30];
    const char* registerName;
    const int numRegisters = bugsnag_ksmach_numRegisters();
    for(int reg = 0; reg < numRegisters; reg++)
    {
        registerName = bugsnag_ksmach_registerName(reg);
        if(registerName == NULL)
        {
            snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
            registerName = registerNameBuff;
        }
        bugsnag_kscrw_i_writeMemoryContentsIfNotable(writer,
                                             registerName,
                                             (uintptr_t)bugsnag_ksmach_registerValue(machineContext, reg));
    }
}

#pragma mark Thread-specific

/** Write any notable addresses in the stack or registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void bugsnag_kscrw_i_writeNotableAddresses(const BugsnagKSCrashReportWriter* const writer,
                                   const char* const key,
                                   const STRUCT_MCONTEXT_L* const machineContext)
{
    writer->beginObject(writer, key);
    {
        bugsnag_kscrw_i_writeNotableRegisters(writer, machineContext);
        bugsnag_kscrw_i_writeNotableStackContents(writer,
                                          machineContext,
                                          kStackNotableSearchBackDistance,
                                          kStackNotableSearchForwardDistance);
    }
    writer->endContainer(writer);
}

/** Write information about a thread to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to write about.
 *
 * @param index The thread's index relative to all threads.
 *
 * @paran If true, write any notable addresses found.
 */
void bugsnag_kscrw_i_writeThread(const BugsnagKSCrashReportWriter* const writer,
                         const char* const key,
                         const BugsnagKSCrash_SentryContext* const crash,
                         const thread_t thread,
                         const int index,
                         const bool writeNotableAddresses,
                         const bool searchThreadNames,
                         const bool searchQueueNames)
{
    bool isCrashedThread = thread == crash->offendingThread;
    char nameBuffer[128];
    STRUCT_MCONTEXT_L machineContextBuffer;
    uintptr_t backtraceBuffer[kMaxBacktraceDepth];
    int backtraceLength = sizeof(backtraceBuffer) / sizeof(*backtraceBuffer);
    int skippedEntries = 0;

    STRUCT_MCONTEXT_L* machineContext = bugsnag_kscrw_i_getMachineContext(crash,
                                                                 thread,
                                                                 &machineContextBuffer);

    uintptr_t* backtrace = bugsnag_kscrw_i_getBacktrace(crash,
                                                thread,
                                                machineContext,
                                                backtraceBuffer,
                                                &backtraceLength,
                                                &skippedEntries);

    writer->beginObject(writer, key);
    {
        if(backtrace != NULL)
        {
            bugsnag_kscrw_i_writeBacktrace(writer,
                                   BugsnagKSCrashField_Backtrace,
                                   backtrace,
                                   backtraceLength,
                                   skippedEntries);
        }
        if(machineContext != NULL)
        {
            bugsnag_kscrw_i_writeRegisters(writer,
                                   BugsnagKSCrashField_Registers,
                                   machineContext,
                                   isCrashedThread);
        }
        writer->addIntegerElement(writer, BugsnagKSCrashField_Index, index);
        if(searchThreadNames)
        {
            if(bugsnag_ksmach_getThreadName(thread, nameBuffer, sizeof(nameBuffer)) && nameBuffer[0] != 0)
            {
                writer->addStringElement(writer, BugsnagKSCrashField_Name, nameBuffer);
            }
        }
        if (searchQueueNames) {
            if(bugsnag_ksmach_getThreadQueueName(thread, nameBuffer, sizeof(nameBuffer)) && nameBuffer[0] != 0)
            {
                writer->addStringElement(writer,
                                         BugsnagKSCrashField_DispatchQueue,
                                         nameBuffer);
            }
        }
        writer->addBooleanElement(writer, BugsnagKSCrashField_Crashed, isCrashedThread);
        writer->addBooleanElement(writer,
                                  BugsnagKSCrashField_CurrentThread,
                                  thread == bugsnag_ksmach_thread_self());
        if(isCrashedThread && machineContext != NULL)
        {
            bugsnag_kscrw_i_writeStackContents(writer,
                                       BugsnagKSCrashField_Stack,
                                       machineContext,
                                       skippedEntries > 0);
            if(writeNotableAddresses)
            {
                bugsnag_kscrw_i_writeNotableAddresses(writer,
                                              BugsnagKSCrashField_NotableAddresses,
                                              machineContext);
            }
        }
    }
    writer->endContainer(writer);
}

/** Write information about all threads to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 */
void bugsnag_kscrw_i_writeAllThreads(const BugsnagKSCrashReportWriter* const writer,
                             const char* const key,
                             const BugsnagKSCrash_SentryContext* const crash,
                             bool writeNotableAddresses,
                             bool searchThreadNames,
                             bool searchQueueNames)
{
    const task_t thisTask = mach_task_self();
    thread_act_array_t threads;
    mach_msg_type_number_t numThreads;
    kern_return_t kr;

    if((kr = task_threads(thisTask, &threads, &numThreads)) != KERN_SUCCESS)
    {
        BugsnagKSLOG_ERROR("task_threads: %s", mach_error_string(kr));
        return;
    }

    // Fetch info for all threads.
    writer->beginArray(writer, key);
    {
        for(mach_msg_type_number_t i = 0; i < numThreads; i++)
        {
            bugsnag_kscrw_i_writeThread(writer, NULL, crash, threads[i], (int)i, writeNotableAddresses, searchThreadNames,
                                searchQueueNames);
        }
    }
    writer->endContainer(writer);

    // Clean up.
    for(mach_msg_type_number_t i = 0; i < numThreads; i++)
    {
        mach_port_deallocate(thisTask, threads[i]);
    }
    vm_deallocate(thisTask, (vm_address_t)threads, sizeof(thread_t) * numThreads);
}

/** Get the index of a thread.
 *
 * @param thread The thread.
 *
 * @return The thread's index, or -1 if it couldn't be determined.
 */
int bugsnag_kscrw_i_threadIndex(const thread_t thread)
{
    int index = -1;
    const task_t thisTask = mach_task_self();
    thread_act_array_t threads;
    mach_msg_type_number_t numThreads;
    kern_return_t kr;

    if((kr = task_threads(thisTask, &threads, &numThreads)) != KERN_SUCCESS)
    {
        BugsnagKSLOG_ERROR("task_threads: %s", mach_error_string(kr));
        return -1;
    }

    for(mach_msg_type_number_t i = 0; i < numThreads; i++)
    {
        if(threads[i] == thread)
        {
            index = (int)i;
            break;
        }
    }

    // Clean up.
    for(mach_msg_type_number_t i = 0; i < numThreads; i++)
    {
        mach_port_deallocate(thisTask, threads[i]);
    }
    vm_deallocate(thisTask, (vm_address_t)threads, sizeof(thread_t) * numThreads);

    return index;
}

#pragma mark Global Report Data

/** Write information about a binary image to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param index Which image to write about.
 */
void bugsnag_kscrw_i_writeBinaryImage(const BugsnagKSCrashReportWriter* const writer,
                              const char* const key,
                              const uint32_t index)
{
    const struct mach_header* header = _dyld_get_image_header(index);
    if(header == NULL)
    {
        return;
    }

    uintptr_t cmdPtr = bugsnag_ksdl_firstCmdAfterHeader(header);
    if(cmdPtr == 0)
    {
        return;
    }

    // Look for the TEXT segment to get the image size.
    // Also look for a UUID command.
    uint64_t imageSize = 0;
    uint64_t imageVmAddr = 0;
    uint8_t* uuid = NULL;

    for(uint32_t iCmd = 0; iCmd < header->ncmds; iCmd++)
    {
        struct load_command* loadCmd = (struct load_command*)cmdPtr;
        switch(loadCmd->cmd)
        {
            case LC_SEGMENT:
            {
                struct segment_command* segCmd = (struct segment_command*)cmdPtr;
                if(strcmp(segCmd->segname, SEG_TEXT) == 0)
                {
                    imageSize = segCmd->vmsize;
                    imageVmAddr = segCmd->vmaddr;
                }
                break;
            }
            case LC_SEGMENT_64:
            {
                struct segment_command_64* segCmd = (struct segment_command_64*)cmdPtr;
                if(strcmp(segCmd->segname, SEG_TEXT) == 0)
                {
                    imageSize = segCmd->vmsize;
                    imageVmAddr = segCmd->vmaddr;
                }
                break;
            }
            case LC_UUID:
            {
                struct uuid_command* uuidCmd = (struct uuid_command*)cmdPtr;
                uuid = uuidCmd->uuid;
                break;
            }
        }
        cmdPtr += loadCmd->cmdsize;
    }

    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, BugsnagKSCrashField_ImageAddress, (uintptr_t)header);
        writer->addUIntegerElement(writer, BugsnagKSCrashField_ImageVmAddress, imageVmAddr);
        writer->addUIntegerElement(writer, BugsnagKSCrashField_ImageSize, imageSize);
        writer->addStringElement(writer, BugsnagKSCrashField_Name, _dyld_get_image_name(index));
        writer->addUUIDElement(writer, BugsnagKSCrashField_UUID, uuid);
        writer->addIntegerElement(writer, BugsnagKSCrashField_CPUType, header->cputype);
        writer->addIntegerElement(writer, BugsnagKSCrashField_CPUSubType, header->cpusubtype);
    }
    writer->endContainer(writer);
}

/** Write information about all images to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
void bugsnag_kscrw_i_writeBinaryImages(const BugsnagKSCrashReportWriter* const writer,
                               const char* const key)
{
    const uint32_t imageCount = _dyld_image_count();

    writer->beginArray(writer, key);
    {
        for(uint32_t iImg = 0; iImg < imageCount; iImg++)
        {
            bugsnag_kscrw_i_writeBinaryImage(writer, NULL, iImg);
        }
    }
    writer->endContainer(writer);
}

/** Write information about system memory to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
void bugsnag_kscrw_i_writeMemoryInfo(const BugsnagKSCrashReportWriter* const writer,
                             const char* const key)
{
    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, BugsnagKSCrashField_Usable, bugsnag_ksmach_usableMemory());
        writer->addUIntegerElement(writer, BugsnagKSCrashField_Free, bugsnag_ksmach_freeMemory());
    }
    writer->endContainer(writer);
}

/** Write information about the error leading to the crash to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 */
void bugsnag_kscrw_i_writeError(const BugsnagKSCrashReportWriter* const writer,
                        const char* const key,
                        const BugsnagKSCrash_SentryContext* const crash)
{
    int machExceptionType = 0;
    kern_return_t machCode = 0;
    kern_return_t machSubCode = 0;
    int sigNum = 0;
    int sigCode = 0;
    const char* exceptionName = NULL;
    const char* crashReason = NULL;

    // Gather common info.
    switch(crash->crashType)
    {
        case BugsnagKSCrashTypeMainThreadDeadlock:
            break;
        case BugsnagKSCrashTypeMachException:
            machExceptionType = crash->mach.type;
            machCode = (kern_return_t)crash->mach.code;
            if(machCode == KERN_PROTECTION_FAILURE && crash->isStackOverflow)
            {
                // A stack overflow should return KERN_INVALID_ADDRESS, but
                // when a stack blasts through the guard pages at the top of the stack,
                // it generates KERN_PROTECTION_FAILURE. Correct for this.
                machCode = KERN_INVALID_ADDRESS;
            }
            machSubCode = (kern_return_t)crash->mach.subcode;

            sigNum = bugsnag_kssignal_signalForMachException(machExceptionType,
                                                     machCode);
            break;
        case BugsnagKSCrashTypeCPPException:
            machExceptionType = EXC_CRASH;
            sigNum = SIGABRT;
            crashReason = crash->crashReason;
            exceptionName = crash->CPPException.name;
            break;
        case BugsnagKSCrashTypeNSException:
            machExceptionType = EXC_CRASH;
            sigNum = SIGABRT;
            exceptionName = crash->NSException.name;
            crashReason = crash->crashReason;
            break;
        case BugsnagKSCrashTypeSignal:
            sigNum = crash->signal.signalInfo->si_signo;
            sigCode = crash->signal.signalInfo->si_code;
            machExceptionType = bugsnag_kssignal_machExceptionForSignal(sigNum);
            break;
        case BugsnagKSCrashTypeUserReported:
            machExceptionType = EXC_CRASH;
            sigNum = SIGABRT;
            crashReason = crash->crashReason;
            break;
    }

    const char* machExceptionName = bugsnag_ksmach_exceptionName(machExceptionType);
    const char* machCodeName = machCode == 0 ? NULL : bugsnag_ksmach_kernelReturnCodeName(machCode);
    const char* sigName = bugsnag_kssignal_signalName(sigNum);
    const char* sigCodeName = bugsnag_kssignal_signalCodeName(sigNum, sigCode);

    writer->beginObject(writer, key);
    {
        writer->beginObject(writer, BugsnagKSCrashField_Mach);
        {
            writer->addUIntegerElement(writer, BugsnagKSCrashField_Exception, (unsigned)machExceptionType);
            if(machExceptionName != NULL)
            {
                writer->addStringElement(writer, BugsnagKSCrashField_ExceptionName, machExceptionName);
            }
            writer->addUIntegerElement(writer, BugsnagKSCrashField_Code, (unsigned)machCode);
            if(machCodeName != NULL)
            {
                writer->addStringElement(writer, BugsnagKSCrashField_CodeName, machCodeName);
            }
            writer->addUIntegerElement(writer, BugsnagKSCrashField_Subcode, (unsigned)machSubCode);
        }
        writer->endContainer(writer);

        writer->beginObject(writer, BugsnagKSCrashField_Signal);
        {
            writer->addUIntegerElement(writer, BugsnagKSCrashField_Signal, (unsigned)sigNum);
            if(sigName != NULL)
            {
                writer->addStringElement(writer, BugsnagKSCrashField_Name, sigName);
            }
            writer->addUIntegerElement(writer, BugsnagKSCrashField_Code, (unsigned)sigCode);
            if(sigCodeName != NULL)
            {
                writer->addStringElement(writer, BugsnagKSCrashField_CodeName, sigCodeName);
            }
        }
        writer->endContainer(writer);

        writer->addUIntegerElement(writer, BugsnagKSCrashField_Address, crash->faultAddress);
        if(crashReason != NULL)
        {
            writer->addStringElement(writer, BugsnagKSCrashField_Reason, crashReason);
        }

        // Gather specific info.
        switch(crash->crashType)
        {
            case BugsnagKSCrashTypeMainThreadDeadlock:
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashExcType_Deadlock);
                break;
                
            case BugsnagKSCrashTypeMachException:
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashExcType_Mach);
                break;

            case BugsnagKSCrashTypeCPPException:
            {
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashExcType_CPPException);
                writer->beginObject(writer, BugsnagKSCrashField_CPPException);
                {
                    writer->addStringElement(writer, BugsnagKSCrashField_Name, exceptionName);
                }
                writer->endContainer(writer);
                break;
            }
            case BugsnagKSCrashTypeNSException:
            {
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashExcType_NSException);
                writer->beginObject(writer, BugsnagKSCrashField_NSException);
                {
                    writer->addStringElement(writer, BugsnagKSCrashField_Name, exceptionName);
                    bugsnag_kscrw_i_writeAddressReferencedByString(writer, BugsnagKSCrashField_ReferencedObject, crashReason);
                }
                writer->endContainer(writer);
                break;
            }
            case BugsnagKSCrashTypeSignal:
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashExcType_Signal);
                break;

            case BugsnagKSCrashTypeUserReported:
            {
                writer->addStringElement(writer, BugsnagKSCrashField_Type, BugsnagKSCrashExcType_User);
                writer->beginObject(writer, BugsnagKSCrashField_UserReported);
                {
                    writer->addStringElement(writer, BugsnagKSCrashField_Name, crash->userException.name);
                    if(crash->userException.lineOfCode != NULL)
                    {
                        writer->addStringElement(writer, BugsnagKSCrashField_LineOfCode, crash->userException.lineOfCode);
                    }
                    if(crash->userException.customStackTraceLength > 0)
                    {
                        writer->beginArray(writer, BugsnagKSCrashField_Backtrace);
                        {
                            for(int i = 0; i < crash->userException.customStackTraceLength; i++)
                            {
                                writer->addStringElement(writer, NULL, crash->userException.customStackTrace[i]);
                            }
                        }
                        writer->endContainer(writer);
                    }
                }
                writer->endContainer(writer);
                break;
            }
        }
    }
    writer->endContainer(writer);
}

/** Write information about app runtime, etc to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param state The persistent crash handler state.
 */
void bugsnag_kscrw_i_writeAppStats(const BugsnagKSCrashReportWriter* const writer,
                           const char* const key,
                           BugsnagKSCrash_State* state)
{
    writer->beginObject(writer, key);
    {
        writer->addBooleanElement(writer, BugsnagKSCrashField_AppActive,
                                  state->applicationIsActive);
        writer->addBooleanElement(writer, BugsnagKSCrashField_AppInFG,
                                  state->applicationIsInForeground);

        writer->addIntegerElement(writer, BugsnagKSCrashField_LaunchesSinceCrash,
                                  state->launchesSinceLastCrash);
        writer->addIntegerElement(writer, BugsnagKSCrashField_SessionsSinceCrash,
                                  state->sessionsSinceLastCrash);
        writer->addFloatingPointElement(writer,
                                        BugsnagKSCrashField_ActiveTimeSinceCrash,
                                        state->activeDurationSinceLastCrash);
        writer->addFloatingPointElement(writer,
                                        BugsnagKSCrashField_BGTimeSinceCrash,
                                        state->backgroundDurationSinceLastCrash);

        writer->addIntegerElement(writer, BugsnagKSCrashField_SessionsSinceLaunch,
                                  state->sessionsSinceLaunch);
        writer->addFloatingPointElement(writer,
                                        BugsnagKSCrashField_ActiveTimeSinceLaunch,
                                        state->activeDurationSinceLaunch);
        writer->addFloatingPointElement(writer,
                                        BugsnagKSCrashField_BGTimeSinceLaunch,
                                        state->backgroundDurationSinceLaunch);
    }
    writer->endContainer(writer);
}

/** Write information about this process.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
void bugsnag_kscrw_i_writeProcessState(const BugsnagKSCrashReportWriter* const writer,
                               const char* const key)
{
    writer->beginObject(writer, key);
    {
        const void* excAddress = bugsnag_kszombie_lastDeallocedNSExceptionAddress();
        if(excAddress != NULL)
        {
            writer->beginObject(writer, BugsnagKSCrashField_LastDeallocedNSException);
            {
                writer->addUIntegerElement(writer, BugsnagKSCrashField_Address,
                                          (uintptr_t)excAddress);
                writer->addStringElement(writer, BugsnagKSCrashField_Name,
                                         bugsnag_kszombie_lastDeallocedNSExceptionName());
                writer->addStringElement(writer, BugsnagKSCrashField_Reason,
                                         bugsnag_kszombie_lastDeallocedNSExceptionReason());
                bugsnag_kscrw_i_writeAddressReferencedByString(writer,
                                                       BugsnagKSCrashField_ReferencedObject,
                                                       bugsnag_kszombie_lastDeallocedNSExceptionReason());
                bugsnag_kscrw_i_writeBacktrace(writer,
                                       BugsnagKSCrashField_Backtrace,
                                       bugsnag_kszombie_lastDeallocedNSExceptionCallStack(),
                                       (int)bugsnag_kszombie_lastDeallocedNSExceptionCallStackLength(),
                                       0);
            }
            writer->endContainer(writer);
        }
    }
    writer->endContainer(writer);
}

/** Write basic report information.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param type The report type.
 *
 * @param reportID The report ID.
 */
void bugsnag_kscrw_i_writeReportInfo(const BugsnagKSCrashReportWriter* const writer,
                             const char* const key,
                             const char* const type,
                             const char* const reportID,
                             const char* const processName)
{
    writer->beginObject(writer, key);
    {
        writer->beginObject(writer, BugsnagKSCrashField_Version);
        {
            writer->addIntegerElement(writer, BugsnagKSCrashField_Major, kReportVersionMajor);
            writer->addIntegerElement(writer, BugsnagKSCrashField_Minor, kReportVersionMinor);
        }
        writer->endContainer(writer);

        writer->addStringElement(writer, BugsnagKSCrashField_ID, reportID);
        writer->addStringElement(writer, BugsnagKSCrashField_ProcessName, processName);
        writer->addIntegerElement(writer, BugsnagKSCrashField_Timestamp, time(NULL));
        writer->addStringElement(writer, BugsnagKSCrashField_Type, type);
    }
    writer->endContainer(writer);
}


#pragma mark Setup

/** Prepare a report writer for use.
 *
 * @oaram writer The writer to prepare.
 *
 * @param context JSON writer contextual information.
 */
void bugsnag_kscrw_i_prepareReportWriter(BugsnagKSCrashReportWriter* const writer,
                                 BugsnagKSJSONEncodeContext* const context)
{
    writer->addBooleanElement = bugsnag_kscrw_i_addBooleanElement;
    writer->addFloatingPointElement = bugsnag_kscrw_i_addFloatingPointElement;
    writer->addIntegerElement = bugsnag_kscrw_i_addIntegerElement;
    writer->addUIntegerElement = bugsnag_kscrw_i_addUIntegerElement;
    writer->addStringElement = bugsnag_kscrw_i_addStringElement;
    writer->addTextFileElement = bugsnag_kscrw_i_addTextFileElement;
    writer->addDataElement = bugsnag_kscrw_i_addDataElement;
    writer->beginDataElement = bugsnag_kscrw_i_beginDataElement;
    writer->appendDataElement = bugsnag_kscrw_i_appendDataElement;
    writer->endDataElement = bugsnag_kscrw_i_endDataElement;
    writer->addUUIDElement = bugsnag_kscrw_i_addUUIDElement;
    writer->addJSONElement = bugsnag_kscrw_i_addJSONElement;
    writer->beginObject = bugsnag_kscrw_i_beginObject;
    writer->beginArray = bugsnag_kscrw_i_beginArray;
    writer->endContainer = bugsnag_kscrw_i_endContainer;
    writer->context = context;
}

/** Open the crash report file.
 *
 * @param path The path to the file.
 *
 * @return The file descriptor, or -1 if an error occurred.
 */
int bugsnag_kscrw_i_openCrashReportFile(const char* const path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if(fd < 0)
    {
        BugsnagKSLOG_ERROR("Could not open crash report file %s: %s",
                    path,
                    strerror(errno));
    }
    return fd;
}

/** Record whether the crashed thread had a stack overflow or not.
 *
 * @param crashContext the context.
 */
void bugsnag_kscrw_i_updateStackOverflowStatus(BugsnagKSCrash_Context* const crashContext)
{
    // TODO: This feels weird. Shouldn't be mutating the context.
    if(bugsnag_kscrw_i_isStackOverflow(&crashContext->crash, crashContext->crash.offendingThread))
    {
        BugsnagKSLOG_TRACE("Stack overflow detected.");
        crashContext->crash.isStackOverflow = true;
    }
}

void bugsnag_kscrw_i_callUserCrashHandler(BugsnagKSCrash_Context* const crashContext,
                                  BugsnagKSCrashReportWriter* writer)
{
    crashContext->config.onCrashNotify(writer);
}

// ============================================================================
#pragma mark - Main API -
// ============================================================================

void bugsnag_kscrashreport_writeMinimalReport(BugsnagKSCrash_Context* const crashContext,
                                      const char* const path)
{
    BugsnagKSLOG_INFO("Writing minimal crash report to %s", path);

    int fd = bugsnag_kscrw_i_openCrashReportFile(path);
    if(fd < 0)
    {
        return;
    }

    g_introspectionRules = &crashContext->config.introspectionRules;
    
    bugsnag_kscrw_i_updateStackOverflowStatus(crashContext);

    BugsnagKSJSONEncodeContext jsonContext;
    jsonContext.userData = &fd;
    BugsnagKSCrashReportWriter concreteWriter;
    BugsnagKSCrashReportWriter* writer = &concreteWriter;
    bugsnag_kscrw_i_prepareReportWriter(writer, &jsonContext);

    bugsnag_ksjson_beginEncode(getJsonContext(writer),
                       true,
                       bugsnag_kscrw_i_addJSONData,
                       &fd);

    writer->beginObject(writer, BugsnagKSCrashField_Report);
    {
        bugsnag_kscrw_i_writeReportInfo(writer,
                                BugsnagKSCrashField_Report,
                                BugsnagKSCrashReportType_Minimal,
                                crashContext->config.crashID,
                                crashContext->config.processName);

        writer->beginObject(writer, BugsnagKSCrashField_Crash);
        {
            bugsnag_kscrw_i_writeThread(writer,
                                BugsnagKSCrashField_CrashedThread,
                                &crashContext->crash,
                                crashContext->crash.offendingThread,
                                bugsnag_kscrw_i_threadIndex(crashContext->crash.offendingThread),
                                false, false, false);
            bugsnag_kscrw_i_writeError(writer, BugsnagKSCrashField_Error, &crashContext->crash);
        }
        writer->endContainer(writer);
    }
    writer->endContainer(writer);

    bugsnag_ksjson_endEncode(getJsonContext(writer));

    close(fd);
}

void bugsnag_kscrashreport_writeStandardReport(BugsnagKSCrash_Context* const crashContext,
                                       const char* const path)
{
    BugsnagKSLOG_INFO("Writing crash report to %s", path);

    int fd = bugsnag_kscrw_i_openCrashReportFile(path);
    if(fd < 0)
    {
        return;
    }
    
    g_introspectionRules = &crashContext->config.introspectionRules;

    bugsnag_kscrw_i_updateStackOverflowStatus(crashContext);

    BugsnagKSJSONEncodeContext jsonContext;
    jsonContext.userData = &fd;
    BugsnagKSCrashReportWriter concreteWriter;
    BugsnagKSCrashReportWriter* writer = &concreteWriter;
    bugsnag_kscrw_i_prepareReportWriter(writer, &jsonContext);

    bugsnag_ksjson_beginEncode(getJsonContext(writer), true, bugsnag_kscrw_i_addJSONData, &fd);

    writer->beginObject(writer, BugsnagKSCrashField_Report);
    {
        bugsnag_kscrw_i_writeReportInfo(writer,
                                BugsnagKSCrashField_Report,
                                BugsnagKSCrashReportType_Standard,
                                crashContext->config.crashID,
                                crashContext->config.processName);

        bugsnag_kscrw_i_writeBinaryImages(writer, BugsnagKSCrashField_BinaryImages);

        bugsnag_kscrw_i_writeProcessState(writer, BugsnagKSCrashField_ProcessState);

        if(crashContext->config.systemInfoJSON != NULL)
        {
            bugsnag_kscrw_i_addJSONElement(writer, BugsnagKSCrashField_System, crashContext->config.systemInfoJSON);
        }

        writer->beginObject(writer, BugsnagKSCrashField_SystemAtCrash);
        {
            bugsnag_kscrw_i_writeMemoryInfo(writer, BugsnagKSCrashField_Memory);
            bugsnag_kscrw_i_writeAppStats(writer, BugsnagKSCrashField_AppStats, &crashContext->state);
        }
        writer->endContainer(writer);

        if(crashContext->config.userInfoJSON != NULL)
        {
            bugsnag_kscrw_i_addJSONElement(writer, BugsnagKSCrashField_User, crashContext->config.userInfoJSON);
        }

        writer->beginObject(writer, BugsnagKSCrashField_Crash);
        {
            bugsnag_kscrw_i_writeAllThreads(writer,
                                    BugsnagKSCrashField_Threads,
                                    &crashContext->crash,
                                    crashContext->config.introspectionRules.enabled,
                                    crashContext->config.searchThreadNames,
                                    crashContext->config.searchQueueNames);
            bugsnag_kscrw_i_writeError(writer, BugsnagKSCrashField_Error, &crashContext->crash);
        }
        writer->endContainer(writer);

        if(crashContext->config.onCrashNotify != NULL)
        {
            writer->beginObject(writer, BugsnagKSCrashField_UserAtCrash);
            {
                bugsnag_kscrw_i_callUserCrashHandler(crashContext, writer);
            }
            writer->endContainer(writer);
        }
    }
    writer->endContainer(writer);
    
    bugsnag_ksjson_endEncode(getJsonContext(writer));
    
    close(fd);
}

void bugsnag_kscrashreport_logCrash(const BugsnagKSCrash_Context* const crashContext)
{
    const BugsnagKSCrash_SentryContext* crash = &crashContext->crash;
    bugsnag_kscrw_i_logCrashType(crash);
    bugsnag_kscrw_i_logCrashThreadBacktrace(&crashContext->crash);
}
