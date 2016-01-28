//
//  BugsnagKSCrash.m
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


#import "BugsnagKSCrashAdvanced.h"

#import "ARCSafe_MemMgmt.h"
#import "BugsnagKSCrashC.h"
#import "BugsnagKSCrashCallCompletion.h"
#import "BugsnagKSCrashState.h"
#import "BugsnagKSJSONCodecObjC.h"
#import "BugsnagKSSingleton.h"
#import "NSError+SimpleConstructor.h"

//#define BugsnagKSLogger_LocalLevel TRACE
#import "BugsnagKSLogger.h"

#ifdef __IPHONE_OS_VERSION_MAX_ALLOWED
#import <UIKit/UIKit.h>
#endif


// ============================================================================
#pragma mark - Default Constants -
// ============================================================================

/** The maximum number of reports to keep on disk. */
#ifndef BugsnagKSCRASH_MaxStoredReports
    #define BugsnagKSCRASH_MaxStoredReports 5
#endif

/** The directory under "Caches" to store the crash reports. */
#ifndef BugsnagKSCRASH_ReportFilesDirectory
    #define BugsnagKSCRASH_ReportFilesDirectory @"BugsnagKSCrashReports"
#endif


// ============================================================================
#pragma mark - Constants -
// ============================================================================

#define kCrashLogFilenameSuffix "-CrashLog.txt"
#define kCrashStateFilenameSuffix "-CrashState.json"


// ============================================================================
#pragma mark - Globals -
// ============================================================================

@interface BugsnagKSCrash ()

@property(nonatomic,readwrite,retain) NSString* bundleName;
@property(nonatomic,readwrite,retain) NSString* nextCrashID;
@property(nonatomic,readonly,retain) NSString* crashReportPath;
@property(nonatomic,readonly,retain) NSString* recrashReportPath;
@property(nonatomic,readonly,retain) NSString* stateFilePath;

// Mirrored from BugsnagKSCrashAdvanced.h to provide ivars
@property(nonatomic,readwrite,retain) id<BugsnagKSCrashReportFilter> sink;
@property(nonatomic,readwrite,retain) NSString* logFilePath;
@property(nonatomic,readwrite,retain) BugsnagKSCrashReportStore* crashReportStore;
@property(nonatomic,readwrite,assign) BugsnagKSReportWriteCallback onCrash;
@property(nonatomic,readwrite,assign) bool printTraceToStdout;

@end


@implementation BugsnagKSCrash

// ============================================================================
#pragma mark - Properties -
// ============================================================================

@synthesize sink = _sink;
@synthesize userInfo = _userInfo;
@synthesize deleteBehaviorAfterSendAll = _deleteBehaviorAfterSendAll;
@synthesize handlingCrashTypes = _handlingCrashTypes;
@synthesize zombieCacheSize = _zombieCacheSize;
@synthesize deadlockWatchdogInterval = _deadlockWatchdogInterval;
@synthesize printTraceToStdout = _printTraceToStdout;
@synthesize onCrash = _onCrash;
@synthesize crashReportStore = _crashReportStore;
@synthesize bundleName = _bundleName;
@synthesize logFilePath = _logFilePath;
@synthesize nextCrashID = _nextCrashID;
@synthesize searchThreadNames = _searchThreadNames;
@synthesize searchQueueNames = _searchQueueNames;
@synthesize introspectMemory = _introspectMemory;
@synthesize doNotIntrospectClasses = _doNotIntrospectClasses;
@synthesize suspendThreadsForUserReported = _suspendThreadsForUserReported;


// ============================================================================
#pragma mark - Lifecycle -
// ============================================================================

IMPLEMENT_EXCLUSIVE_SHARED_INSTANCE(BugsnagKSCrash)

- (id) init
{
    if((self = [super init]))
    {
        self.bundleName = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];

        NSArray* directories = NSSearchPathForDirectoriesInDomains(NSCachesDirectory,
                                                                   NSUserDomainMask,
                                                                   YES);
        if([directories count] == 0)
        {
            BugsnagKSLOG_ERROR(@"Could not locate cache directory path.");
            goto failed;
        }
        NSString* cachePath = [directories objectAtIndex:0];
        if([cachePath length] == 0)
        {
            BugsnagKSLOG_ERROR(@"Could not locate cache directory path.");
            goto failed;
        }
        NSString* storePathEnd = [BugsnagKSCRASH_ReportFilesDirectory stringByAppendingPathComponent:self.bundleName];
        NSString* storePath = [cachePath stringByAppendingPathComponent:storePathEnd];
        if([storePath length] == 0)
        {
            BugsnagKSLOG_ERROR(@"Could not determine report files path.");
            goto failed;
        }
        if(![self ensureDirectoryExists:storePath])
        {
            goto failed;
        }

        self.nextCrashID = [self generateUUIDString];
        self.crashReportStore = [BugsnagKSCrashReportStore storeWithPath:storePath];
        self.deleteBehaviorAfterSendAll = BugsnagKSCDeleteAlways;
        self.searchThreadNames = NO;
        self.searchQueueNames = NO;
        self.introspectMemory = YES;
        self.suspendThreadsForUserReported = YES;
    }
    return self;

failed:
    BugsnagKSLOG_ERROR(@"Failed to initialize crash handler. Crash reporting disabled.");
    as_release(self);
    return nil;
}

- (void) dealloc
{
    as_release(_bundleName);
    as_release(_userInfo);
    as_release(_sink);
    as_release(_crashReportStore);
    as_release(_logFilePath);
    as_release(_nextCrashID);
    as_superdealloc();
}


// ============================================================================
#pragma mark - API -
// ============================================================================

- (void) setUserInfo:(NSDictionary*) userInfo
{
    NSError* error = nil;
    NSData* userInfoJSON = nil;
    if(userInfo != nil)
    {
        userInfoJSON = [self nullTerminated:[BugsnagKSJSONCodec encode:userInfo
                                                        options:BugsnagKSJSONEncodeOptionSorted
                                                          error:&error]];
        if(error != NULL)
        {
            BugsnagKSLOG_ERROR(@"Could not serialize user info: %@", error);
            return;
        }
    }
    
    as_autorelease_noref(_userInfo);
    _userInfo = as_retain(userInfo);
    bugsnag_kscrash_setUserInfoJSON([userInfoJSON bytes]);
}

- (void) setHandlingCrashTypes:(BugsnagKSCrashType)handlingCrashTypes
{
    _handlingCrashTypes = bugsnag_kscrash_setHandlingCrashTypes(handlingCrashTypes);
}

- (void) setZombieCacheSize:(size_t) zombieCacheSize
{
    _zombieCacheSize = zombieCacheSize;
    bugsnag_kscrash_setZombieCacheSize(zombieCacheSize);
}

- (void) setDeadlockWatchdogInterval:(double) deadlockWatchdogInterval
{
    _deadlockWatchdogInterval = deadlockWatchdogInterval;
    bugsnag_kscrash_setDeadlockWatchdogInterval(deadlockWatchdogInterval);
}

- (void) setPrintTraceToStdout:(bool)printTraceToStdout
{
    _printTraceToStdout = printTraceToStdout;
    bugsnag_kscrash_setPrintTraceToStdout(printTraceToStdout);
}

- (void) setOnCrash:(BugsnagKSReportWriteCallback) onCrash
{
    _onCrash = onCrash;
    bugsnag_kscrash_setCrashNotifyCallback(onCrash);
}

- (void) setSearchThreadNames:(bool)searchThreadNames
{
    _searchThreadNames = searchThreadNames;
    bugsnag_kscrash_setSearchThreadNames(searchThreadNames);
}

- (void) setSearchQueueNames:(bool)searchQueueNames
{
    _searchQueueNames = searchQueueNames;
    bugsnag_kscrash_setSearchQueueNames(searchQueueNames);
}

- (void) setIntrospectMemory:(bool) introspectMemory
{
    _introspectMemory = introspectMemory;
    bugsnag_kscrash_setIntrospectMemory(introspectMemory);
}

- (void) setDoNotIntrospectClasses:(NSArray *)doNotIntrospectClasses
{
    as_autorelease_noref(_doNotIntrospectClasses);
    _doNotIntrospectClasses = as_retain(doNotIntrospectClasses);
    size_t count = [doNotIntrospectClasses count];
    if(count == 0)
    {
        bugsnag_kscrash_setDoNotIntrospectClasses(nil, 0);
    }
    else
    {
        NSMutableData* data = [NSMutableData dataWithLength:count * sizeof(const char*)];
        const char** classes = data.mutableBytes;
        for(size_t i = 0; i < count; i++)
        {
            classes[i] = [[doNotIntrospectClasses objectAtIndex:i] cStringUsingEncoding:NSUTF8StringEncoding];
        }
        bugsnag_kscrash_setDoNotIntrospectClasses(classes, count);
    }
}

- (void) setSuspendThreadsForUserReported:(bool) suspendThreadsForUserReported
{
    _suspendThreadsForUserReported = suspendThreadsForUserReported;
    bugsnag_kscrash_setSuspendThreadsForUserReported(suspendThreadsForUserReported);
}

- (NSString*) crashReportPath
{
    return [self.crashReportStore pathToCrashReportWithID:self.nextCrashID];
}

- (NSString*) recrashReportPath
{
    return [self.crashReportStore pathToRecrashReportWithID:self.nextCrashID];
}

- (NSString*) stateFilePath
{
    NSString* stateFilename = [NSString stringWithFormat:@"%@" kCrashStateFilenameSuffix, self.bundleName];
    return [self.crashReportStore.path stringByAppendingPathComponent:stateFilename];
}

- (BOOL) install
{
    _handlingCrashTypes = bugsnag_kscrash_install([self.crashReportPath UTF8String],
                                          [self.recrashReportPath UTF8String],
                                          [self.stateFilePath UTF8String],
                                          [self.nextCrashID UTF8String]);
    if(self.handlingCrashTypes == 0)
    {
        return false;
    }

#ifdef __IPHONE_OS_VERSION_MAX_ALLOWED
    NSNotificationCenter* nCenter = [NSNotificationCenter defaultCenter];
    [nCenter addObserver:self
                selector:@selector(applicationDidBecomeActive)
                    name:UIApplicationDidBecomeActiveNotification
                  object:nil];
    [nCenter addObserver:self
                selector:@selector(applicationWillResignActive)
                    name:UIApplicationWillResignActiveNotification
                  object:nil];
    [nCenter addObserver:self
                selector:@selector(applicationDidEnterBackground)
                    name:UIApplicationDidEnterBackgroundNotification
                  object:nil];
    [nCenter addObserver:self
                selector:@selector(applicationWillEnterForeground)
                    name:UIApplicationWillEnterForegroundNotification
                  object:nil];
    [nCenter addObserver:self
                selector:@selector(applicationWillTerminate)
                    name:UIApplicationWillTerminateNotification
                  object:nil];
#endif
    
    return true;
}

- (void) sendAllReportsWithCompletion:(BugsnagKSCrashReportFilterCompletion) onCompletion
{
    [self.crashReportStore pruneReportsLeaving:BugsnagKSCRASH_MaxStoredReports];
    
    NSArray* reports = [self allReports];
    
    BugsnagKSLOG_INFO(@"Sending %d crash reports", [reports count]);
    
    [self sendReports:reports
         onCompletion:^(NSArray* filteredReports, BOOL completed, NSError* error)
     {
         BugsnagKSLOG_DEBUG(@"Process finished with completion: %d", completed);
         if(error != nil)
         {
             BugsnagKSLOG_ERROR(@"Failed to send reports: %@", error);
         }
         if((self.deleteBehaviorAfterSendAll == BugsnagKSCDeleteOnSucess && completed) ||
            self.deleteBehaviorAfterSendAll == BugsnagKSCDeleteAlways)
         {
             [self deleteAllReports];
         }
         bugsnag_kscrash_i_callCompletion(onCompletion, filteredReports, completed, error);
     }];
}

- (void) deleteAllReports
{
    [self.crashReportStore deleteAllReports];
}

- (void) reportUserException:(NSString*) name
                      reason:(NSString*) reason
                  lineOfCode:(NSString*) lineOfCode
                  stackTrace:(NSArray*) stackTrace
            terminateProgram:(BOOL) terminateProgram
{
    const char* cName = [name cStringUsingEncoding:NSUTF8StringEncoding];
    const char* cReason = [reason cStringUsingEncoding:NSUTF8StringEncoding];
    const char* cLineOfCode = [lineOfCode cStringUsingEncoding:NSUTF8StringEncoding];
    size_t cStackTraceCount = [stackTrace count];
    const char** cStackTrace = malloc(sizeof(*cStackTrace) * cStackTraceCount);

    for(size_t i = 0; i < cStackTraceCount; i++)
    {
        cStackTrace[i] = [[stackTrace objectAtIndex:i] cStringUsingEncoding:NSUTF8StringEncoding];
    }

    bugsnag_kscrash_reportUserException(cName,
                                cReason,
                                cLineOfCode,
                                cStackTrace,
                                cStackTraceCount,
                                terminateProgram);

    // If bugsnag_kscrash_reportUserException() returns, we did not terminate.
    // Set up IDs and paths for the next crash.

    self.nextCrashID = [self generateUUIDString];

    bugsnag_kscrash_reinstall([self.crashReportPath UTF8String],
                      [self.recrashReportPath UTF8String],
                      [self.stateFilePath UTF8String],
                      [self.nextCrashID UTF8String]);

    free((void*)cStackTrace);
}

// ============================================================================
#pragma mark - Advanced API -
// ============================================================================

#define SYNTHESIZE_CRASH_STATE_PROPERTY(TYPE, NAME) \
- (TYPE) NAME \
{ \
    return bugsnag_kscrashstate_currentState()->NAME; \
}

SYNTHESIZE_CRASH_STATE_PROPERTY(NSTimeInterval, activeDurationSinceLastCrash)
SYNTHESIZE_CRASH_STATE_PROPERTY(NSTimeInterval, backgroundDurationSinceLastCrash)
SYNTHESIZE_CRASH_STATE_PROPERTY(int, launchesSinceLastCrash)
SYNTHESIZE_CRASH_STATE_PROPERTY(int, sessionsSinceLastCrash)
SYNTHESIZE_CRASH_STATE_PROPERTY(NSTimeInterval, activeDurationSinceLaunch)
SYNTHESIZE_CRASH_STATE_PROPERTY(NSTimeInterval, backgroundDurationSinceLaunch)
SYNTHESIZE_CRASH_STATE_PROPERTY(int, sessionsSinceLaunch)
SYNTHESIZE_CRASH_STATE_PROPERTY(BOOL, crashedLastLaunch)

- (NSUInteger) reportCount
{
    return [self.crashReportStore reportCount];
}

- (NSString*) crashReportsPath
{
    return self.crashReportStore.path;
}

- (void) sendReports:(NSArray*) reports onCompletion:(BugsnagKSCrashReportFilterCompletion) onCompletion
{
    if([reports count] == 0)
    {
        bugsnag_kscrash_i_callCompletion(onCompletion, reports, YES, nil);
        return;
    }
    
    if(self.sink == nil)
    {
        bugsnag_kscrash_i_callCompletion(onCompletion, reports, NO,
                                 [NSError errorWithDomain:[[self class] description]
                                                     code:0
                                              description:@"No sink set. Crash reports not sent."]);
        return;
    }
    
    [self.sink filterReports:reports
                onCompletion:^(NSArray* filteredReports, BOOL completed, NSError* error)
     {
         bugsnag_kscrash_i_callCompletion(onCompletion, filteredReports, completed, error);
     }];
}

- (NSArray*) allReports
{
    return [self.crashReportStore allReports];
}

- (BOOL) redirectConsoleLogsToFile:(NSString*) fullPath overwrite:(BOOL) overwrite
{
    if(bugsnag_kslog_setLogFilename([fullPath UTF8String], overwrite))
    {
        self.logFilePath = fullPath;
        return YES;
    }
    return NO;
}

- (BOOL) redirectConsoleLogsToDefaultFile
{
    NSString* logFilename = [NSString stringWithFormat:@"%@" kCrashLogFilenameSuffix, self.bundleName];
    NSString* logFilePath = [self.crashReportStore.path stringByAppendingPathComponent:logFilename];
    if(![self redirectConsoleLogsToFile:logFilePath overwrite:YES])
    {
        BugsnagKSLOG_ERROR(@"Could not redirect logs to %@", logFilePath);
        return NO;
    }
    return YES;
}


// ============================================================================
#pragma mark - Utility -
// ============================================================================

- (BOOL) ensureDirectoryExists:(NSString*) path
{
    NSError* error = nil;
    NSFileManager* fm = [NSFileManager defaultManager];
    
    if(![fm fileExistsAtPath:path])
    {
        if(![fm createDirectoryAtPath:path
          withIntermediateDirectories:YES
                           attributes:nil
                                error:&error])
        {
            BugsnagKSLOG_ERROR(@"Could not create directory %@: %@.", path, error);
            return NO;
        }
    }
    
    return YES;
}

- (NSString*) generateUUIDString
{
    CFUUIDRef uuid = CFUUIDCreate(NULL);
    NSString* uuidString = (as_bridge_transfer NSString*)CFUUIDCreateString(NULL, uuid);
    CFRelease(uuid);
    
    return as_autorelease(uuidString);
}

- (NSMutableData*) nullTerminated:(NSData*) data
{
    if(data == nil)
    {
        return NULL;
    }
    NSMutableData* mutable = [NSMutableData dataWithData:data];
    [mutable appendBytes:"\0" length:1];
    return mutable;
}


// ============================================================================
#pragma mark - Callbacks -
// ============================================================================

- (void) applicationDidBecomeActive
{
    bugsnag_kscrashstate_notifyAppActive(true);
}

- (void) applicationWillResignActive
{
    bugsnag_kscrashstate_notifyAppActive(false);
}

- (void) applicationDidEnterBackground
{
    bugsnag_kscrashstate_notifyAppInForeground(false);
}

- (void) applicationWillEnterForeground
{
    bugsnag_kscrashstate_notifyAppInForeground(true);
}

- (void) applicationWillTerminate
{
    bugsnag_kscrashstate_notifyAppTerminate();
}

@end
