
#include <TargetConditionals.h>
#if TARGET_OS_IOS == 1 || TARGET_OS_VISION == 1
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif

#define IPLUG_AUVIEWCONTROLLER IPlugAUViewController_vSparkles
#define IPLUG_AUAUDIOUNIT IPlugAUAudioUnit_vSparkles
#import <SparklesAU/IPlugAUViewController.h>
#import <SparklesAU/IPlugAUAudioUnit.h>

//! Project version number for SparklesAU.
FOUNDATION_EXPORT double SparklesAUVersionNumber;

//! Project version string for SparklesAU.
FOUNDATION_EXPORT const unsigned char SparklesAUVersionString[];

@class IPlugAUViewController_vSparkles;
