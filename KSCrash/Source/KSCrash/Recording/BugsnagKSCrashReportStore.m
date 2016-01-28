//
//  BugsnagKSCrashReportStore.m
//
//  Created by Karl Stenerud on 2012-02-05.
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


#import "BugsnagKSCrashReportStore.h"

#import "ARCSafe_MemMgmt.h"
#import "BugsnagKSCrashReportFields.h"
#import "BugsnagKSJSONCodecObjC.h"
#import "BugsnagKSSafeCollections.h"
#import "NSDictionary+Merge.h"
#import "NSError+SimpleConstructor.h"
#import "BugsnagRFC3339DateTool.h"
#import "BugsnagKSCrashDoctor.h"

//#define BugsnagKSLogger_LocalLevel TRACE
#import "BugsnagKSLogger.h"


// ============================================================================
#pragma mark - Constants -
// ============================================================================

#define kCrashReportSuffix @"-CrashReport-"
#define kRecrashReportSuffix @"-RecrashReport-"


// ============================================================================
#pragma mark - Meta Data -
// ============================================================================

/**
 * Metadata class to hold name and creation date for a file, with
 * default comparison based on the creation date (ascending).
 */
@interface BugsnagKSCrashReportInfo: NSObject

@property(nonatomic,readonly,retain) NSString* reportID;
@property(nonatomic,readonly,retain) NSDate* creationDate;

+ (BugsnagKSCrashReportInfo*) reportInfoWithID:(NSString*) reportID
                           creationDate:(NSDate*) creationDate;

- (id) initWithID:(NSString*) reportID creationDate:(NSDate*) creationDate;

- (NSComparisonResult) compare:(BugsnagKSCrashReportInfo*) other;

@end

@implementation BugsnagKSCrashReportInfo

@synthesize reportID = _reportID;
@synthesize creationDate = _creationDate;

+ (BugsnagKSCrashReportInfo*) reportInfoWithID:(NSString*) reportID
                           creationDate:(NSDate*) creationDate
{
    return as_autorelease([[self alloc] initWithID:reportID
                                      creationDate:creationDate]);
}

- (id) initWithID:(NSString*) reportID creationDate:(NSDate*) creationDate
{
    if((self = [super init]))
    {
        _reportID = as_retain(reportID);
        _creationDate = as_retain(creationDate);
    }
    return self;
}

- (void) dealloc
{
    as_release(_reportID);
    as_release(_creationDate);
    as_superdealloc();
}

- (NSComparisonResult) compare:(BugsnagKSCrashReportInfo*) other
{
    return [_creationDate compare:other->_creationDate];
}

@end


// ============================================================================
#pragma mark - Main Class -
// ============================================================================

@interface BugsnagKSCrashReportStore ()

@property(nonatomic,readwrite,retain) NSString* path;
@property(nonatomic,readwrite,retain) NSString* bundleName;

@end


@implementation BugsnagKSCrashReportStore

#pragma mark Properties

@synthesize path = _path;
@synthesize bundleName = _bundleName;


#pragma mark Construction

+ (BugsnagKSCrashReportStore*) storeWithPath:(NSString*) path
{
    return as_autorelease([[self alloc] initWithPath:path]);
}

- (id) initWithPath:(NSString*) path
{
    if((self = [super init]))
    {
        self.path = path;
        self.bundleName = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];
    }
    return self;
}

- (void) dealloc
{
    as_release(_path);
    as_release(_bundleName);
    as_superdealloc();
}

#pragma mark API

- (NSArray*) reportIDs
{
    NSError* error = nil;
    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray* filenames = [fm contentsOfDirectoryAtPath:self.path error:&error];
    if(filenames == nil)
    {
        BugsnagKSLOG_ERROR(@"Could not get contents of directory %@: %@", self.path, error);
        return nil;
    }

    NSMutableArray* reports = [NSMutableArray arrayWithCapacity:[filenames count]];
    for(NSString* filename in filenames)
    {
        NSString* reportId = [self reportIDFromFilename:filename];
        if(reportId != nil)
        {
            NSString* fullPath = [self.path stringByAppendingPathComponent:filename];
            NSDictionary* fileAttribs = [fm attributesOfItemAtPath:fullPath error:&error];
            if(fileAttribs == nil)
            {
                BugsnagKSLOG_ERROR(@"Could not read file attributes for %@: %@", fullPath, error);
            }
            else
            {
                [reports addObject:[BugsnagKSCrashReportInfo reportInfoWithID:reportId
                                                          creationDate:[fileAttribs valueForKey:NSFileCreationDate]]];
            }
        }
    }
    [reports sortUsingSelector:@selector(compare:)];

    NSMutableArray* sortedIDs = [NSMutableArray arrayWithCapacity:[reports count]];
    for(BugsnagKSCrashReportInfo* info in reports)
    {
        [sortedIDs addObject:info.reportID];
    }
    return sortedIDs;
}

- (NSUInteger) reportCount
{
    return [[self reportIDs] count];
}

- (NSDictionary*) reportWithID:(NSString*) reportID
{
    NSError* error = nil;
    NSMutableDictionary* crashReport = [self readReport:[self pathToCrashReportWithID:reportID] error:&error];
    if(error != nil)
    {
        BugsnagKSLOG_ERROR(@"Encountered error loading crash report %@: %@", reportID, error);
    }
    if(crashReport == nil)
    {
        BugsnagKSLOG_ERROR(@"Could not load crash report");
        return nil;
    }
    NSMutableDictionary* recrashReport = [self readReport:[self pathToRecrashReportWithID:reportID] error:nil];
    [crashReport setObjectIfNotNil:recrashReport forKey:@BugsnagKSCrashField_RecrashReport];

    return crashReport;
}

- (NSArray*) allReports
{
    NSArray* reportIDs = [self reportIDs];
    NSMutableArray* reports = [NSMutableArray arrayWithCapacity:[reportIDs count]];
    for(NSString* reportID in reportIDs)
    {
        NSDictionary* report = [self reportWithID:reportID];
        if(report != nil)
        {
            [reports addObject:report];
        }
    }

    return reports;
}

- (void) deleteReportWithID:(NSString*) reportID
{
    NSError* error = nil;
    NSString* filename = [self pathToCrashReportWithID:reportID];

    [[NSFileManager defaultManager] removeItemAtPath:filename error:&error];
    if(error != nil)
    {
        BugsnagKSLOG_ERROR(@"Could not delete file %@: %@", filename, error);
    }

    // Don't care if this succeeds or not since it may not exist.
    [[NSFileManager defaultManager] removeItemAtPath:[self pathToRecrashReportWithID:reportID]
                                               error:&error];
}

- (void) deleteAllReports
{
    for(NSString* reportID in [self reportIDs])
    {
        [self deleteReportWithID:reportID];
    }
}

- (void) pruneReportsLeaving:(int) numReports
{
    NSArray* reportIDs = [self reportIDs];
    int deleteCount = (int)[reportIDs count] - numReports;
    for(int i = 0; i < deleteCount; i++)
    {
        [self deleteReportWithID:[reportIDs objectAtIndex:(NSUInteger)i]];
    }
}


#pragma mark Utility

- (NSMutableDictionary*) fixupCrashReport:(NSDictionary*) report
{
    if(![report isKindOfClass:[NSDictionary class]])
    {
        BugsnagKSLOG_ERROR(@"Report should be a dictionary, not %@", [report class]);
        return nil;
    }

    NSMutableDictionary* mutableReport = as_autorelease([report mutableCopy]);
    NSMutableDictionary* mutableInfo = as_autorelease([[report objectForKey:@BugsnagKSCrashField_Report] mutableCopy]);
    [mutableReport setObjectIfNotNil:mutableInfo forKey:@BugsnagKSCrashField_Report];

    // Timestamp gets stored as a unix timestamp. Convert it to rfc3339.
    [self convertTimestamp:@BugsnagKSCrashField_Timestamp inReport:mutableInfo];

    [self mergeDictWithKey:@BugsnagKSCrashField_SystemAtCrash
           intoDictWithKey:@BugsnagKSCrashField_System
                  inReport:mutableReport];

    [self mergeDictWithKey:@BugsnagKSCrashField_UserAtCrash
           intoDictWithKey:@BugsnagKSCrashField_User
                  inReport:mutableReport];

    NSMutableDictionary* crashReport = as_autorelease([[report objectForKey:@BugsnagKSCrashField_Crash] mutableCopy]);
    [mutableReport setObjectIfNotNil:crashReport forKey:@BugsnagKSCrashField_Crash];
    BugsnagKSCrashDoctor* doctor = [BugsnagKSCrashDoctor doctor];
    [crashReport setObjectIfNotNil:[doctor diagnoseCrash:report] forKey:@BugsnagKSCrashField_Diagnosis];

    return mutableReport;
}

- (void) mergeDictWithKey:(NSString*) srcKey
          intoDictWithKey:(NSString*) dstKey
                 inReport:(NSMutableDictionary*) report
{
    NSDictionary* srcDict = [report objectForKey:srcKey];
    if(srcDict == nil)
    {
        // It's OK if the source dict didn't exist.
        return;
    }
    if(![srcDict isKindOfClass:[NSDictionary class]])
    {
        BugsnagKSLOG_ERROR(@"'%@' should be a dictionary, not %@", srcKey, [srcDict class]);
        return;
    }

    NSDictionary* dstDict = [report objectForKey:dstKey];
    if(dstDict == nil)
    {
        dstDict = [NSDictionary dictionary];
    }
    if(![dstDict isKindOfClass:[NSDictionary class]])
    {
        BugsnagKSLOG_ERROR(@"'%@' should be a dictionary, not %@", dstKey, [dstDict class]);
        return;
    }

    [report setObjectIfNotNil:[srcDict mergedInto:dstDict] forKey:dstKey];
    [report removeObjectForKey:srcKey];
}

- (void) convertTimestamp:(NSString*) key
                 inReport:(NSMutableDictionary*) report
{
    NSNumber* timestamp = [report objectForKey:key];
    if(timestamp == nil)
    {
        BugsnagKSLOG_ERROR(@"entry '%@' not found", key);
        return;
    }
    if(![timestamp isKindOfClass:[NSNumber class]])
    {
        BugsnagKSLOG_ERROR(@"'%@' should be a number, not %@", key, [key class]);
        return;
    }
    [report setValue:[BugsnagRFC3339DateTool stringFromUNIXTimestamp:[timestamp unsignedLongLongValue]]
              forKey:key];
}

- (NSString*) crashReportFilenameWithID:(NSString*) reportID
{
    return [NSString stringWithFormat:@"%@" kCrashReportSuffix "%@.json",
            self.bundleName, reportID];
}

- (NSString*) recrashReportFilenameWithID:(NSString*) reportID
{
    return [NSString stringWithFormat:@"%@" kRecrashReportSuffix "%@.json",
            self.bundleName, reportID];
}

- (NSString*) reportIDFromFilename:(NSString*) filename
{
    if([filename length] == 0)
    {
        return nil;
    }

    NSString* prefix = [NSString stringWithFormat:@"%@" kCrashReportSuffix,
                        self.bundleName];
    NSString* suffix = @".json";
    if([filename rangeOfString:prefix].location == 0 &&
       [filename rangeOfString:suffix].location != NSNotFound)
    {
        NSUInteger prefixLength = [prefix length];
        NSUInteger suffixLength = [suffix length];
        NSRange range = NSMakeRange(prefixLength, [filename length] - prefixLength - suffixLength);
        return [filename substringWithRange:range];
    }
    return nil;
}

- (NSString*) pathToCrashReportWithID:(NSString*) reportID
{
    NSString* filename = [self crashReportFilenameWithID:reportID];
    return [self.path stringByAppendingPathComponent:filename];
}

- (NSString*) pathToRecrashReportWithID:(NSString*) reportID
{
    NSString* filename = [self recrashReportFilenameWithID:reportID];
    return [self.path stringByAppendingPathComponent:filename];
}

- (NSMutableDictionary*) readReport:(NSString*) path error:(NSError* __autoreleasing *) error
{
    if(path == nil)
    {
        [NSError fillError:error withDomain:[[self class] description]
                      code:0
               description:@"Path is nil"];
        return nil;
    }

    NSData* jsonData = [NSData dataWithContentsOfFile:path options:0 error:error];
    if(jsonData == nil)
    {
        return nil;
    }

    NSMutableDictionary* report = [self fixupCrashReport:[BugsnagKSJSONCodec decode:jsonData
                                                                     options:BugsnagKSJSONDecodeOptionIgnoreNullInArray |
                                                          BugsnagKSJSONDecodeOptionIgnoreNullInObject |
                                                          BugsnagKSJSONDecodeOptionKeepPartialObject
                                                                       error:error]];
    if(error != nil && *error != nil)
    {
        
        BugsnagKSLOG_ERROR(@"Error decoding JSON data from %@: %@", path, *error);
        [report setObject:[NSNumber numberWithBool:YES] forKey:@BugsnagKSCrashField_Incomplete];
    }

    return report;
}

@end
