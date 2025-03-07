/* -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#import <Cocoa/Cocoa.h>

#include "nsMacSharingService.h"

#include "jsapi.h"
#include "nsCocoaUtils.h"
#include "mozilla/MacStringHelpers.h"

NS_IMPL_ISUPPORTS(nsMacSharingService, nsIMacSharingService)

// List of sharingProviders that we do not want to expose to
// the user, because they are duplicates or do not work correctly
// within the context
NSArray *filteredProviderNames = @[
  @"com.apple.share.System.add-to-safari-reading-list",
  @"com.apple.share.Mail.compose"
];

NSString* const remindersServiceName =
  @"com.apple.reminders.RemindersShareExtension";

// These are some undocumented constants also used by Safari
// to let us open the preferences window
NSString* const extensionPrefPanePath =
  @"/System/Library/PreferencePanes/Extensions.prefPane";
const UInt32 openSharingSubpaneDescriptorType = 'ptru';
NSString* const openSharingSubpaneActionKey = @"action";
NSString* const openSharingSubpaneActionValue = @"revealExtensionPoint";
NSString* const openSharingSubpaneProtocolKey = @"protocol";
NSString* const openSharingSubpaneProtocolValue = @"com.apple.share-services";

// Expose the id so we can pass reference through to JS and back
@interface NSSharingService (ExposeName)
- (id)name;
@end

// Clean up the activity once the share is complete
@interface SharingServiceDelegate : NSObject <NSSharingServiceDelegate>
{
  NSUserActivity* mShareActivity;
}

- (void)cleanup;

@end

@implementation SharingServiceDelegate

- (id)initWithActivity:(NSUserActivity*)activity
{
  self = [super init];
  mShareActivity = [activity retain];
  return self;
}

- (void)cleanup {
  [mShareActivity resignCurrent];
  [mShareActivity invalidate];
  [mShareActivity release];
  mShareActivity = nil;
}

- (void)sharingService:(NSSharingService *)sharingService
        didShareItems:(NSArray *)items {
  [self cleanup];
}

- (void)sharingService:(NSSharingService*)service
        didFailToShareItems:(NSArray*)items error:(NSError*)error {
  [self cleanup];
}

- (void)dealloc {
  [mShareActivity release];
  [super dealloc];
}

@end

static NSString*
NSImageToBase64(const NSImage* aImage)
{
  [aImage lockFocus];
  NSBitmapImageRep* bitmapRep = [[NSBitmapImageRep alloc] initWithFocusedViewRect:NSMakeRect(0, 0, aImage.size.width, aImage.size.height)];
  [aImage unlockFocus];
  NSData* imageData = [bitmapRep representationUsingType:NSPNGFileType properties:@{}];
  [bitmapRep release];
  NSString* base64Encoded = [imageData base64EncodedStringWithOptions:0];
  return [NSString stringWithFormat: @"data:image/png;base64,%@", base64Encoded];
}

static void
SetStrAttribute(JSContext* aCx,
                JS::Rooted<JSObject*>& aObj,
                const char* aKey, NSString* aVal)
{
  nsAutoString strVal;
  mozilla::CopyCocoaStringToXPCOMString(aVal, strVal);
  JS::Rooted<JSString*> title(aCx, JS_NewUCStringCopyZ(aCx, strVal.get()));
  JS::Rooted<JS::Value> attVal(aCx, JS::StringValue(title));
  JS_SetProperty(aCx, aObj, aKey, attVal);
}

nsresult
nsMacSharingService::GetSharingProviders(const nsAString& aPageUrl,
                                         JSContext* aCx,
                                         JS::MutableHandleValue aResult)
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  JS::Rooted<JSObject*> array(aCx, JS_NewArrayObject(aCx, 0));
  NSURL* url = [NSURL URLWithString:nsCocoaUtils::ToNSString(aPageUrl)];

  NSArray* sharingService = [NSSharingService
                             sharingServicesForItems:[NSArray arrayWithObject:url]];
  int32_t serviceCount = 0;

  for (NSSharingService *currentService in sharingService) {
    if ([filteredProviderNames containsObject:[currentService name]]) {
      continue;
    }
    JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));

    SetStrAttribute(aCx, obj, "name", [currentService name]);
    SetStrAttribute(aCx, obj, "menuItemTitle", currentService.menuItemTitle);
    SetStrAttribute(aCx, obj, "image", NSImageToBase64(currentService.image));

    JS::Rooted<JS::Value> element(aCx, JS::ObjectValue(*obj));
    JS_SetElement(aCx, array, serviceCount++, element);
  }

  aResult.setObject(*array);

  return NS_OK;
  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

NS_IMETHODIMP
nsMacSharingService::OpenSharingPreferences()
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  NSURL* prefPaneURL =
    [NSURL fileURLWithPath:extensionPrefPanePath isDirectory:YES];
  NSDictionary* args = @{
    openSharingSubpaneActionKey: openSharingSubpaneActionValue,
    openSharingSubpaneProtocolKey: openSharingSubpaneProtocolValue
  };
  NSData* data =
    [NSPropertyListSerialization
     dataWithPropertyList:args
     format:NSPropertyListXMLFormat_v1_0
     options:0
     error:nil];
  NSAppleEventDescriptor* descriptor =
    [[NSAppleEventDescriptor alloc]
     initWithDescriptorType:openSharingSubpaneDescriptorType
     data:data];

  [[NSWorkspace sharedWorkspace] openURLs:@[ prefPaneURL ]
   withAppBundleIdentifier:nil
   options:NSWorkspaceLaunchAsync
   additionalEventParamDescriptor:descriptor
   launchIdentifiers:NULL];

  [descriptor release];

  return NS_OK;
  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}

NS_IMETHODIMP
nsMacSharingService::ShareUrl(const nsAString& aServiceName,
                              const nsAString& aPageUrl,
                              const nsAString& aPageTitle)
{
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK_NSRESULT;

  NSString* serviceName = nsCocoaUtils::ToNSString(aServiceName);
  NSURL* pageUrl = [NSURL URLWithString:nsCocoaUtils::ToNSString(aPageUrl)];
  NSString* pageTitle = nsCocoaUtils::ToNSString(aPageTitle);
  NSSharingService* service = [NSSharingService
                               sharingServiceNamed:serviceName];

  // Reminders fetch its data from an activity, not the share data
  if ([[service name] isEqual:remindersServiceName]) {

    NSUserActivity* shareActivity =
      [[NSUserActivity alloc] initWithActivityType:NSUserActivityTypeBrowsingWeb];

    if ([pageUrl.scheme hasPrefix:@"http"]) {
      [shareActivity setWebpageURL:pageUrl];
    }
    [shareActivity setTitle:pageTitle];
    [shareActivity becomeCurrent];

    // Pass ownership of shareActivity to shareDelegate, which will release the
    // activity once sharing has completed.
    SharingServiceDelegate* shareDelegate =
      [[SharingServiceDelegate alloc] initWithActivity:shareActivity];
    [shareActivity release];

    [service setDelegate:shareDelegate];
    [shareDelegate release];
  }

  // Twitter likes the the title as an additional share item
  NSArray* toShare = [[service name] isEqual:NSSharingServiceNamePostOnTwitter]
    ? @[pageUrl, pageTitle]
    : @[pageUrl];

  [service setSubject:pageTitle];
  [service performWithItems:toShare];

  return NS_OK;

  NS_OBJC_END_TRY_ABORT_BLOCK_NSRESULT;
}
