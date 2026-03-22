/*
 Copyright (c) 2012-2019, Pierre-Olivier Latour
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 * The name of Pierre-Olivier Latour may not be used to endorse
 or promote products derived from this software without specific
 prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL PIERRE-OLIVIER LATOUR BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if !__has_feature(objc_arc)
#error GCDWebUploader requires ARC
#endif

#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <SystemConfiguration/SystemConfiguration.h>
#endif

#import "GCDWebUploader.h"
#import "GCDWebServerFunctions.h"

#import "GCDWebServerDataRequest.h"
#import "GCDWebServerMultiPartFormRequest.h"
#import "GCDWebServerURLEncodedFormRequest.h"

#import "GCDWebServerDataResponse.h"
#import "GCDWebServerErrorResponse.h"
#import "GCDWebServerFileResponse.h"

#import <zlib.h>
#import <libkern/OSByteOrder.h>

NS_ASSUME_NONNULL_BEGIN

@interface GCDWebUploader (Methods)
- (nullable GCDWebServerResponse*)listDirectory:(GCDWebServerRequest*)request;
- (nullable GCDWebServerResponse*)downloadFile:(GCDWebServerRequest*)request;
- (nullable GCDWebServerResponse*)uploadFile:(GCDWebServerMultiPartFormRequest*)request;
- (nullable GCDWebServerResponse*)moveItem:(GCDWebServerURLEncodedFormRequest*)request;
- (nullable GCDWebServerResponse*)deleteItem:(GCDWebServerURLEncodedFormRequest*)request;
- (nullable GCDWebServerResponse*)createDirectory:(GCDWebServerURLEncodedFormRequest*)request;
- (nullable GCDWebServerResponse*)readFile:(GCDWebServerRequest*)request;
- (nullable GCDWebServerResponse*)writeFile:(GCDWebServerDataRequest*)request;
- (nullable GCDWebServerResponse*)downloadZip:(GCDWebServerRequest*)request;
@end

NS_ASSUME_NONNULL_END

@implementation GCDWebUploader

@dynamic delegate;

- (instancetype)initWithUploadDirectory:(NSString*)path {
  if ((self = [super init])) {
    NSString* bundlePath = [[NSBundle bundleForClass:[GCDWebUploader class]] pathForResource:@"GCDWebUploader" ofType:@"bundle"];
    if (bundlePath == nil) {
      return nil;
    }
    NSBundle* siteBundle = [NSBundle bundleWithPath:bundlePath];
    if (siteBundle == nil) {
      return nil;
    }
    _uploadDirectory = [path copy];
    GCDWebUploader* __unsafe_unretained server = self;

    // Resource files
    [self addGETHandlerForBasePath:@"/" directoryPath:(NSString*)[siteBundle resourcePath] indexFilename:nil cacheAge:3600 allowRangeRequests:NO];

    // Web page
    [self addHandlerForMethod:@"GET"
                         path:@"/"
                 requestClass:[GCDWebServerRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {

#if TARGET_OS_IPHONE
                   NSString* device = [[UIDevice currentDevice] name];
#else
          NSString* device = CFBridgingRelease(SCDynamicStoreCopyComputerName(NULL, NULL));
#endif
                   NSString* title = server.title;
                   if (title == nil) {
                     title = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"];
                     if (title == nil) {
                       title = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
                     }
#if !TARGET_OS_IPHONE
                     if (title == nil) {
                       title = [[NSProcessInfo processInfo] processName];
                     }
#endif
                   }
                   NSString* header = server.header;
                   if (header == nil) {
                     header = title;
                   }
                   NSString* prologue = server.prologue;
                   if (prologue == nil) {
                     prologue = [siteBundle localizedStringForKey:@"PROLOGUE" value:@"" table:nil];
                   }
                   NSString* epilogue = server.epilogue;
                   if (epilogue == nil) {
                     epilogue = [siteBundle localizedStringForKey:@"EPILOGUE" value:@"" table:nil];
                   }
                   NSString* footer = server.footer;
                   if (footer == nil) {
                     NSString* name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"];
                     if (name == nil) {
                       name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
                     }
                     NSString* version = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
#if !TARGET_OS_IPHONE
                     if (!name && !version) {
                       name = @"OS X";
                       version = [[NSProcessInfo processInfo] operatingSystemVersionString];
                     }
#endif
                     footer = [NSString stringWithFormat:[siteBundle localizedStringForKey:@"FOOTER_FORMAT" value:@"" table:nil], name, version];
                   }
                   return [GCDWebServerDataResponse responseWithHTMLTemplate:(NSString*)[siteBundle pathForResource:@"index" ofType:@"html"]
                                                                   variables:@{
                                                                     @"device" : device,
                                                                     @"title" : title,
                                                                     @"header" : header,
                                                                     @"prologue" : prologue,
                                                                     @"epilogue" : epilogue,
                                                                     @"footer" : footer
                                                                   }];
                 }];

    // File listing
    [self addHandlerForMethod:@"GET"
                         path:@"/list"
                 requestClass:[GCDWebServerRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server listDirectory:request];
                 }];

    // File download
    [self addHandlerForMethod:@"GET"
                         path:@"/download"
                 requestClass:[GCDWebServerRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server downloadFile:request];
                 }];

    // Folder ZIP download
    [self addHandlerForMethod:@"GET"
                         path:@"/download-zip"
                 requestClass:[GCDWebServerRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server downloadZip:request];
                 }];

    // File upload
    [self addHandlerForMethod:@"POST"
                         path:@"/upload"
                 requestClass:[GCDWebServerMultiPartFormRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server uploadFile:(GCDWebServerMultiPartFormRequest*)request];
                 }];

    // File and folder moving
    [self addHandlerForMethod:@"POST"
                         path:@"/move"
                 requestClass:[GCDWebServerURLEncodedFormRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server moveItem:(GCDWebServerURLEncodedFormRequest*)request];
                 }];

    // File and folder deletion
    [self addHandlerForMethod:@"POST"
                         path:@"/delete"
                 requestClass:[GCDWebServerURLEncodedFormRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server deleteItem:(GCDWebServerURLEncodedFormRequest*)request];
                 }];

    // Directory creation
    [self addHandlerForMethod:@"POST"
                         path:@"/create"
                 requestClass:[GCDWebServerURLEncodedFormRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server createDirectory:(GCDWebServerURLEncodedFormRequest*)request];
                 }];

    // File reading (for edit)
    [self addHandlerForMethod:@"GET"
                         path:@"/read"
                 requestClass:[GCDWebServerRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server readFile:request];
                 }];

    // File writing (for edit)
    [self addHandlerForMethod:@"POST"
                         path:@"/write"
                 requestClass:[GCDWebServerDataRequest class]
                 processBlock:^GCDWebServerResponse*(GCDWebServerRequest* request) {
                   return [server writeFile:(GCDWebServerDataRequest*)request];
                 }];
  }
  return self;
}

@end

#define ZIP_CHUNK_SIZE 32768

static void _zipWriteUInt16(NSFileHandle* fh, uint16_t value) {
  value = OSSwapHostToLittleInt16(value);
  [fh writeData:[NSData dataWithBytes:&value length:2]];
}

static void _zipWriteUInt32(NSFileHandle* fh, uint32_t value) {
  value = OSSwapHostToLittleInt32(value);
  [fh writeData:[NSData dataWithBytes:&value length:4]];
}

static uint32_t _zipDosDateTimeFromDate(NSDate* date) {
  NSCalendar* calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
  NSDateComponents* c = [calendar components:(NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay |
                                              NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond)
                                    fromDate:date];
  uint16_t dosDate = (uint16_t)(((c.year - 1980) << 9) | (c.month << 5) | c.day);
  uint16_t dosTime = (uint16_t)((c.hour << 11) | (c.minute << 5) | (c.second / 2));
  return ((uint32_t)dosDate << 16) | dosTime;
}

@implementation GCDWebUploader (Methods)

- (BOOL)_checkFileExtension:(NSString*)fileName {
  if (_allowedFileExtensions && ![_allowedFileExtensions containsObject:[[fileName pathExtension] lowercaseString]]) {
    return NO;
  }
  return YES;
}

- (NSString*)_uniquePathForPath:(NSString*)path {
  if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
    NSString* directory = [path stringByDeletingLastPathComponent];
    NSString* file = [path lastPathComponent];
    NSString* base = [file stringByDeletingPathExtension];
    NSString* extension = [file pathExtension];
    int retries = 0;
    do {
      if (extension.length) {
        path = [directory stringByAppendingPathComponent:(NSString*)[[base stringByAppendingFormat:@" (%i)", ++retries] stringByAppendingPathExtension:extension]];
      } else {
        path = [directory stringByAppendingPathComponent:[base stringByAppendingFormat:@" (%i)", ++retries]];
      }
    } while ([[NSFileManager defaultManager] fileExistsAtPath:path]);
  }
  return path;
}

- (GCDWebServerResponse*)listDirectory:(GCDWebServerRequest*)request {
  NSString* relativePath = [[request query] objectForKey:@"path"];
  NSString* absolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)];
  BOOL isDirectory = NO;
  if (!absolutePath || ![[NSFileManager defaultManager] fileExistsAtPath:absolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", relativePath];
  }
  if (!isDirectory) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_BadRequest message:@"\"%@\" is not a directory", relativePath];
  }

  NSString* directoryName = [absolutePath lastPathComponent];
  if (!_allowHiddenItems && [directoryName hasPrefix:@"."]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Listing directory name \"%@\" is not allowed", directoryName];
  }

  NSError* error = nil;
  NSArray* contents = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:absolutePath error:&error] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
  if (contents == nil) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed listing directory \"%@\"", relativePath];
  }

  NSMutableArray* array = [NSMutableArray array];
  for (NSString* item in [contents sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
    if (_allowHiddenItems || ![item hasPrefix:@"."]) {
      NSDictionary* attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:[absolutePath stringByAppendingPathComponent:item] error:NULL];
      NSString* type = [attributes objectForKey:NSFileType];
      if ([type isEqualToString:NSFileTypeRegular] && [self _checkFileExtension:item]) {
        [array addObject:@{
          @"path" : [relativePath stringByAppendingPathComponent:item],
          @"name" : item,
          @"size" : (NSNumber*)[attributes objectForKey:NSFileSize]
        }];
      } else if ([type isEqualToString:NSFileTypeDirectory]) {
        [array addObject:@{
          @"path" : [[relativePath stringByAppendingPathComponent:item] stringByAppendingString:@"/"],
          @"name" : item
        }];
      }
    }
  }
  return [GCDWebServerDataResponse responseWithJSONObject:array];
}

- (GCDWebServerResponse*)downloadFile:(GCDWebServerRequest*)request {
  NSString* relativePath = [[request query] objectForKey:@"path"];
  NSString* absolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)];
  BOOL isDirectory = NO;
  if (![[NSFileManager defaultManager] fileExistsAtPath:absolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", relativePath];
  }
  if (isDirectory) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_BadRequest message:@"\"%@\" is a directory", relativePath];
  }

  NSString* fileName = [absolutePath lastPathComponent];
  if (([fileName hasPrefix:@"."] && !_allowHiddenItems) || ![self _checkFileExtension:fileName]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Downlading file name \"%@\" is not allowed", fileName];
  }

  if ([self.delegate respondsToSelector:@selector(webUploader:didDownloadFileAtPath:)]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self.delegate webUploader:self didDownloadFileAtPath:absolutePath];
    });
  }
  return [GCDWebServerFileResponse responseWithFile:absolutePath isAttachment:YES];
}

- (GCDWebServerResponse*)uploadFile:(GCDWebServerMultiPartFormRequest*)request {
  NSRange range = [[request.headers objectForKey:@"Accept"] rangeOfString:@"application/json" options:NSCaseInsensitiveSearch];
  NSString* contentType = (range.location != NSNotFound ? @"application/json" : @"text/plain; charset=utf-8");  // Required when using iFrame transport (see https://github.com/blueimp/jQuery-File-Upload/wiki/Setup)

  GCDWebServerMultiPartFile* file = [request firstFileForControlName:@"files[]"];
  if ((!_allowHiddenItems && [file.fileName hasPrefix:@"."]) || ![self _checkFileExtension:file.fileName]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Uploaded file name \"%@\" is not allowed", file.fileName];
  }
  NSString* relativePath = [[request firstArgumentForControlName:@"path"] string];
  NSString* absolutePath = [self _uniquePathForPath:[[_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)] stringByAppendingPathComponent:file.fileName]];

  if (![self shouldUploadFileAtPath:absolutePath withTemporaryFile:file.temporaryPath]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Uploading file \"%@\" to \"%@\" is not permitted", file.fileName, relativePath];
  }

  NSError* error = nil;
  if (![[NSFileManager defaultManager] moveItemAtPath:file.temporaryPath toPath:absolutePath error:&error]) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed moving uploaded file to \"%@\"", relativePath];
  }

  if ([self.delegate respondsToSelector:@selector(webUploader:didUploadFileAtPath:)]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self.delegate webUploader:self didUploadFileAtPath:absolutePath];
    });
  }
  return [GCDWebServerDataResponse responseWithJSONObject:@{} contentType:contentType];
}

- (GCDWebServerResponse*)moveItem:(GCDWebServerURLEncodedFormRequest*)request {
  NSString* oldRelativePath = [request.arguments objectForKey:@"oldPath"];
  NSString* oldAbsolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(oldRelativePath)];
  BOOL isDirectory = NO;
  if (![[NSFileManager defaultManager] fileExistsAtPath:oldAbsolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", oldRelativePath];
  }

  NSString* oldItemName = [oldAbsolutePath lastPathComponent];
  if ((!_allowHiddenItems && [oldItemName hasPrefix:@"."]) || (!isDirectory && ![self _checkFileExtension:oldItemName])) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Moving from item name \"%@\" is not allowed", oldItemName];
  }

  NSString* newRelativePath = [request.arguments objectForKey:@"newPath"];
  NSString* newAbsolutePath = [self _uniquePathForPath:[_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(newRelativePath)]];

  NSString* newItemName = [newAbsolutePath lastPathComponent];
  if ((!_allowHiddenItems && [newItemName hasPrefix:@"."]) || (!isDirectory && ![self _checkFileExtension:newItemName])) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Moving to item name \"%@\" is not allowed", newItemName];
  }

  if (![self shouldMoveItemFromPath:oldAbsolutePath toPath:newAbsolutePath]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Moving \"%@\" to \"%@\" is not permitted", oldRelativePath, newRelativePath];
  }

  NSError* error = nil;
  if (![[NSFileManager defaultManager] moveItemAtPath:oldAbsolutePath toPath:newAbsolutePath error:&error]) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed moving \"%@\" to \"%@\"", oldRelativePath, newRelativePath];
  }

  if ([self.delegate respondsToSelector:@selector(webUploader:didMoveItemFromPath:toPath:)]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self.delegate webUploader:self didMoveItemFromPath:oldAbsolutePath toPath:newAbsolutePath];
    });
  }
  return [GCDWebServerDataResponse responseWithJSONObject:@{}];
}

- (GCDWebServerResponse*)deleteItem:(GCDWebServerURLEncodedFormRequest*)request {
  NSString* relativePath = [request.arguments objectForKey:@"path"];
  NSString* absolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)];
  BOOL isDirectory = NO;
  if (![[NSFileManager defaultManager] fileExistsAtPath:absolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", relativePath];
  }

  NSString* itemName = [absolutePath lastPathComponent];
  if (([itemName hasPrefix:@"."] && !_allowHiddenItems) || (!isDirectory && ![self _checkFileExtension:itemName])) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Deleting item name \"%@\" is not allowed", itemName];
  }

  if (![self shouldDeleteItemAtPath:absolutePath]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Deleting \"%@\" is not permitted", relativePath];
  }

  NSError* error = nil;
  if (![[NSFileManager defaultManager] removeItemAtPath:absolutePath error:&error]) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed deleting \"%@\"", relativePath];
  }

  if ([self.delegate respondsToSelector:@selector(webUploader:didDeleteItemAtPath:)]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self.delegate webUploader:self didDeleteItemAtPath:absolutePath];
    });
  }
  return [GCDWebServerDataResponse responseWithJSONObject:@{}];
}

- (GCDWebServerResponse*)readFile:(GCDWebServerRequest*)request {
  NSString* relativePath = [[request query] objectForKey:@"path"];
  NSString* absolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)];
  BOOL isDirectory = NO;
  if (![[NSFileManager defaultManager] fileExistsAtPath:absolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", relativePath];
  }
  if (isDirectory) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_BadRequest message:@"\"%@\" is a directory", relativePath];
  }

  NSString* fileName = [absolutePath lastPathComponent];
  if (([fileName hasPrefix:@"."] && !_allowHiddenItems) || ![self _checkFileExtension:fileName]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Reading file name \"%@\" is not allowed", fileName];
  }

  NSData* data = [[NSFileManager defaultManager] contentsAtPath:absolutePath];
  if (data == nil) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError message:@"Failed reading \"%@\"", relativePath];
  }

  NSString* content = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  if (content == nil) {
    content = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
  }
  if (content == nil) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_BadRequest message:@"\"%@\" is not a text file", relativePath];
  }

  return [GCDWebServerDataResponse responseWithText:content];
}

- (GCDWebServerResponse*)writeFile:(GCDWebServerDataRequest*)request {
  NSString* relativePath = [[request query] objectForKey:@"path"];
  NSString* absolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)];
  BOOL isDirectory = NO;
  if (![[NSFileManager defaultManager] fileExistsAtPath:absolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", relativePath];
  }
  if (isDirectory) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_BadRequest message:@"\"%@\" is a directory", relativePath];
  }

  NSString* fileName = [absolutePath lastPathComponent];
  if (([fileName hasPrefix:@"."] && !_allowHiddenItems) || ![self _checkFileExtension:fileName]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Editing file name \"%@\" is not allowed", fileName];
  }

  if (![self shouldEditFileAtPath:absolutePath]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Editing \"%@\" is not permitted", relativePath];
  }

  NSError* error = nil;
  if (![request.data writeToFile:absolutePath options:NSDataWritingAtomic error:&error]) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed writing \"%@\"", relativePath];
  }

  if ([self.delegate respondsToSelector:@selector(webUploader:didEditFileAtPath:)]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self.delegate webUploader:self didEditFileAtPath:absolutePath];
    });
  }
  return [GCDWebServerDataResponse responseWithJSONObject:@{}];
}

- (GCDWebServerResponse*)createDirectory:(GCDWebServerURLEncodedFormRequest*)request {
  NSString* relativePath = [request.arguments objectForKey:@"path"];
  NSString* absolutePath = [self _uniquePathForPath:[_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)]];

  NSString* directoryName = [absolutePath lastPathComponent];
  if (!_allowHiddenItems && [directoryName hasPrefix:@"."]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Creating directory name \"%@\" is not allowed", directoryName];
  }

  if (![self shouldCreateDirectoryAtPath:absolutePath]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Creating directory \"%@\" is not permitted", relativePath];
  }

  NSError* error = nil;
  if (![[NSFileManager defaultManager] createDirectoryAtPath:absolutePath withIntermediateDirectories:NO attributes:nil error:&error]) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed creating directory \"%@\"", relativePath];
  }

  if ([self.delegate respondsToSelector:@selector(webUploader:didCreateDirectoryAtPath:)]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self.delegate webUploader:self didCreateDirectoryAtPath:absolutePath];
    });
  }
  return [GCDWebServerDataResponse responseWithJSONObject:@{}];
}

- (void)_cleanupOldZipTempFiles {
  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* tmpDir = NSTemporaryDirectory();
  NSArray* contents = [fm contentsOfDirectoryAtPath:tmpDir error:NULL];
  NSDate* cutoff = [NSDate dateWithTimeIntervalSinceNow:-300];
  for (NSString* item in contents) {
    if ([item hasPrefix:@"GCDWebUploader_zip_"]) {
      NSString* fullPath = [tmpDir stringByAppendingPathComponent:item];
      NSDictionary* attrs = [fm attributesOfItemAtPath:fullPath error:NULL];
      NSDate* created = [attrs objectForKey:NSFileCreationDate];
      if (created && [created compare:cutoff] == NSOrderedAscending) {
        [fm removeItemAtPath:fullPath error:NULL];
      }
    }
  }
}

- (nullable NSString*)_createZipFromDirectory:(NSString*)directoryPath
                                    folderName:(NSString*)folderName
                                        error:(NSError**)error {
  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* tempPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                        [NSString stringWithFormat:@"GCDWebUploader_zip_%@.zip", [[NSUUID UUID] UUIDString]]];

  if (![fm createFileAtPath:tempPath contents:nil attributes:nil]) {
    if (error) {
      *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{NSLocalizedDescriptionKey: @"Failed to create temp file"}];
    }
    return nil;
  }

  NSFileHandle* zipFile = [NSFileHandle fileHandleForWritingAtPath:tempPath];
  if (!zipFile) {
    if (error) {
      *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{NSLocalizedDescriptionKey: @"Failed to open temp file"}];
    }
    return nil;
  }

  NSMutableArray* entries = [NSMutableArray array];
  NSDirectoryEnumerator* enumerator = [fm enumeratorAtPath:directoryPath];
  NSString* relativeName;

  @try {
    while ((relativeName = [enumerator nextObject]) != nil) {
      NSString* itemName = [relativeName lastPathComponent];

      if (!_allowHiddenItems && [itemName hasPrefix:@"."]) {
        NSDictionary* attrs = [enumerator fileAttributes];
        if ([[attrs objectForKey:NSFileType] isEqualToString:NSFileTypeDirectory]) {
          [enumerator skipDescendants];
        }
        continue;
      }

      NSString* fullPath = [directoryPath stringByAppendingPathComponent:relativeName];
      NSDictionary* attrs = [fm attributesOfItemAtPath:fullPath error:NULL];
      if (!attrs) continue;

      NSString* fileType = [attrs objectForKey:NSFileType];
      NSString* zipEntryName = [folderName stringByAppendingPathComponent:relativeName];
      NSDate* modDate = [attrs objectForKey:NSFileModificationDate] ?: [NSDate date];
      uint32_t dosDateTime = _zipDosDateTimeFromDate(modDate);

      if ([fileType isEqualToString:NSFileTypeDirectory]) {
        NSString* dirEntryName = [zipEntryName stringByAppendingString:@"/"];
        NSData* nameData = [dirEntryName dataUsingEncoding:NSUTF8StringEncoding];
        uint32_t localHeaderOffset = (uint32_t)[zipFile offsetInFile];

        // Local file header for directory
        _zipWriteUInt32(zipFile, 0x04034b50);  // signature
        _zipWriteUInt16(zipFile, 20);           // version needed
        _zipWriteUInt16(zipFile, 0x0800);       // flags (bit 11: UTF-8)
        _zipWriteUInt16(zipFile, 0);            // compression: stored
        _zipWriteUInt32(zipFile, dosDateTime);  // mod date/time
        _zipWriteUInt32(zipFile, 0);            // CRC-32
        _zipWriteUInt32(zipFile, 0);            // compressed size
        _zipWriteUInt32(zipFile, 0);            // uncompressed size
        _zipWriteUInt16(zipFile, (uint16_t)[nameData length]);  // name length
        _zipWriteUInt16(zipFile, 0);            // extra field length
        [zipFile writeData:nameData];

        [entries addObject:@{
          @"name": nameData,
          @"crc32": @(0),
          @"compressedSize": @(0),
          @"uncompressedSize": @(0),
          @"localHeaderOffset": @(localHeaderOffset),
          @"dosDateTime": @(dosDateTime),
          @"compressionMethod": @(0),
          @"externalAttributes": @(0x10),  // directory flag
        }];

      } else if ([fileType isEqualToString:NSFileTypeRegular]) {
        if (![self _checkFileExtension:itemName]) continue;

        NSData* nameData = [zipEntryName dataUsingEncoding:NSUTF8StringEncoding];
        uint64_t fileSize = [[attrs objectForKey:NSFileSize] unsignedLongLongValue];
        uint32_t localHeaderOffset = (uint32_t)[zipFile offsetInFile];

        // Local file header with data descriptor flag (bit 3) + UTF-8 flag (bit 11)
        _zipWriteUInt32(zipFile, 0x04034b50);  // signature
        _zipWriteUInt16(zipFile, 20);           // version needed
        _zipWriteUInt16(zipFile, 0x0808);       // flags (bit 3: data descriptor, bit 11: UTF-8)
        _zipWriteUInt16(zipFile, 8);            // compression: deflate
        _zipWriteUInt32(zipFile, dosDateTime);  // mod date/time
        _zipWriteUInt32(zipFile, 0);            // CRC-32 (in data descriptor)
        _zipWriteUInt32(zipFile, 0);            // compressed size (in data descriptor)
        _zipWriteUInt32(zipFile, 0);            // uncompressed size (in data descriptor)
        _zipWriteUInt16(zipFile, (uint16_t)[nameData length]);  // name length
        _zipWriteUInt16(zipFile, 0);            // extra field length
        [zipFile writeData:nameData];

        // Compress file data
        uint32_t fileCrc32 = (uint32_t)crc32(0L, Z_NULL, 0);
        uint32_t compressedSize = 0;

        NSFileHandle* sourceFile = [NSFileHandle fileHandleForReadingAtPath:fullPath];
        if (!sourceFile) continue;

        z_stream stream;
        memset(&stream, 0, sizeof(stream));
        if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
          [sourceFile closeFile];
          continue;
        }

        uint8_t outBuffer[ZIP_CHUNK_SIZE];
        uint64_t bytesRemaining = fileSize;

        while (bytesRemaining > 0) {
          NSUInteger readSize = (NSUInteger)MIN((uint64_t)ZIP_CHUNK_SIZE, bytesRemaining);
          NSData* chunk = [sourceFile readDataOfLength:readSize];
          if ([chunk length] == 0) break;

          fileCrc32 = (uint32_t)crc32(fileCrc32, [chunk bytes], (uInt)[chunk length]);
          bytesRemaining -= [chunk length];

          stream.next_in = (Bytef*)[chunk bytes];
          stream.avail_in = (uInt)[chunk length];

          do {
            stream.next_out = outBuffer;
            stream.avail_out = ZIP_CHUNK_SIZE;
            deflate(&stream, Z_NO_FLUSH);
            NSUInteger have = ZIP_CHUNK_SIZE - stream.avail_out;
            if (have > 0) {
              [zipFile writeData:[NSData dataWithBytes:outBuffer length:have]];
              compressedSize += (uint32_t)have;
            }
          } while (stream.avail_out == 0);
        }

        // Flush remaining compressed data
        do {
          stream.next_out = outBuffer;
          stream.avail_out = ZIP_CHUNK_SIZE;
          deflate(&stream, Z_FINISH);
          NSUInteger have = ZIP_CHUNK_SIZE - stream.avail_out;
          if (have > 0) {
            [zipFile writeData:[NSData dataWithBytes:outBuffer length:have]];
            compressedSize += (uint32_t)have;
          }
        } while (stream.avail_out == 0);

        deflateEnd(&stream);
        [sourceFile closeFile];

        // Data descriptor
        _zipWriteUInt32(zipFile, 0x08074b50);  // signature
        _zipWriteUInt32(zipFile, fileCrc32);
        _zipWriteUInt32(zipFile, compressedSize);
        _zipWriteUInt32(zipFile, (uint32_t)fileSize);

        [entries addObject:@{
          @"name": nameData,
          @"crc32": @(fileCrc32),
          @"compressedSize": @(compressedSize),
          @"uncompressedSize": @((uint32_t)fileSize),
          @"localHeaderOffset": @(localHeaderOffset),
          @"dosDateTime": @(dosDateTime),
          @"compressionMethod": @(8),
          @"externalAttributes": @(0),
        }];
      }
    }

    // Central directory
    uint32_t centralDirOffset = (uint32_t)[zipFile offsetInFile];
    uint32_t centralDirSize = 0;

    for (NSDictionary* entry in entries) {
      NSData* nameData = entry[@"name"];
      uint32_t entryStart = (uint32_t)[zipFile offsetInFile];

      _zipWriteUInt32(zipFile, 0x02014b50);  // signature
      _zipWriteUInt16(zipFile, 20);           // version made by
      _zipWriteUInt16(zipFile, 20);           // version needed
      uint16_t flags = ([entry[@"compressionMethod"] unsignedShortValue] == 8) ? 0x0808 : 0x0800;
      _zipWriteUInt16(zipFile, flags);
      _zipWriteUInt16(zipFile, [entry[@"compressionMethod"] unsignedShortValue]);
      _zipWriteUInt32(zipFile, [entry[@"dosDateTime"] unsignedIntValue]);
      _zipWriteUInt32(zipFile, [entry[@"crc32"] unsignedIntValue]);
      _zipWriteUInt32(zipFile, [entry[@"compressedSize"] unsignedIntValue]);
      _zipWriteUInt32(zipFile, [entry[@"uncompressedSize"] unsignedIntValue]);
      _zipWriteUInt16(zipFile, (uint16_t)[nameData length]);
      _zipWriteUInt16(zipFile, 0);   // extra field length
      _zipWriteUInt16(zipFile, 0);   // comment length
      _zipWriteUInt16(zipFile, 0);   // disk number start
      _zipWriteUInt16(zipFile, 0);   // internal file attributes
      _zipWriteUInt32(zipFile, [entry[@"externalAttributes"] unsignedIntValue]);
      _zipWriteUInt32(zipFile, [entry[@"localHeaderOffset"] unsignedIntValue]);
      [zipFile writeData:nameData];

      centralDirSize += (uint32_t)([zipFile offsetInFile] - entryStart);
    }

    // End of central directory
    _zipWriteUInt32(zipFile, 0x06054b50);  // signature
    _zipWriteUInt16(zipFile, 0);           // disk number
    _zipWriteUInt16(zipFile, 0);           // disk with central dir
    _zipWriteUInt16(zipFile, (uint16_t)[entries count]);
    _zipWriteUInt16(zipFile, (uint16_t)[entries count]);
    _zipWriteUInt32(zipFile, centralDirSize);
    _zipWriteUInt32(zipFile, centralDirOffset);
    _zipWriteUInt16(zipFile, 0);           // comment length

    [zipFile closeFile];
  } @catch (NSException* exception) {
    [zipFile closeFile];
    [fm removeItemAtPath:tempPath error:NULL];
    if (error) {
      *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO
                               userInfo:@{NSLocalizedDescriptionKey: [exception reason] ?: @"Unknown error creating ZIP"}];
    }
    return nil;
  }

  return tempPath;
}

- (GCDWebServerResponse*)downloadZip:(GCDWebServerRequest*)request {
  [self _cleanupOldZipTempFiles];

  NSString* relativePath = [[request query] objectForKey:@"path"];
  NSString* absolutePath = [_uploadDirectory stringByAppendingPathComponent:GCDWebServerNormalizePath(relativePath)];
  BOOL isDirectory = NO;
  if (!absolutePath || ![[NSFileManager defaultManager] fileExistsAtPath:absolutePath isDirectory:&isDirectory]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_NotFound message:@"\"%@\" does not exist", relativePath];
  }
  if (!isDirectory) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_BadRequest message:@"\"%@\" is not a directory", relativePath];
  }

  NSString* directoryName = [absolutePath lastPathComponent];
  if (!_allowHiddenItems && [directoryName hasPrefix:@"."]) {
    return [GCDWebServerErrorResponse responseWithClientError:kGCDWebServerHTTPStatusCode_Forbidden message:@"Downloading directory \"%@\" is not allowed", directoryName];
  }

  NSError* error = nil;
  NSString* zipPath = [self _createZipFromDirectory:absolutePath folderName:directoryName error:&error];
  if (!zipPath) {
    return [GCDWebServerErrorResponse responseWithServerError:kGCDWebServerHTTPStatusCode_InternalServerError underlyingError:error message:@"Failed creating ZIP for \"%@\"", relativePath];
  }

  GCDWebServerFileResponse* response = [GCDWebServerFileResponse responseWithFile:zipPath isAttachment:YES];
  NSString* zipFileName = [directoryName stringByAppendingPathExtension:@"zip"];
  NSString* disposition = [NSString stringWithFormat:@"attachment; filename=\"%@\"; filename*=UTF-8''%@",
                           zipFileName, GCDWebServerEscapeURLString(zipFileName)];
  [response setValue:disposition forAdditionalHeader:@"Content-Disposition"];

  return response;
}

@end

@implementation GCDWebUploader (Subclassing)

- (BOOL)shouldUploadFileAtPath:(NSString*)path withTemporaryFile:(NSString*)tempPath {
  return YES;
}

- (BOOL)shouldMoveItemFromPath:(NSString*)fromPath toPath:(NSString*)toPath {
  return YES;
}

- (BOOL)shouldDeleteItemAtPath:(NSString*)path {
  return YES;
}

- (BOOL)shouldCreateDirectoryAtPath:(NSString*)path {
  return YES;
}

- (BOOL)shouldEditFileAtPath:(NSString*)path {
  return YES;
}

@end
