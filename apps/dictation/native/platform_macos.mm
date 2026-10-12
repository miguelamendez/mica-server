#include "platform.hpp"
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <atomic>
#include <thread>

static AVAudioRecorder* recorder;
static CFMachPortRef event_tap;
static CFRunLoopRef shortcut_loop;
static std::thread shortcut_thread;
static std::atomic<bool> quit_shortcut{false};
static std::function<void()> shortcut_callback;

bool microphone_start(const std::string& path,std::string& error) {
  @autoreleasepool {
    auto permission=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if(permission==AVAuthorizationStatusDenied||permission==AVAuthorizationStatusRestricted){error="Microphone permission denied. Enable this app/terminal in macOS Privacy & Security.";return false;}
    if(permission==AVAuthorizationStatusNotDetermined){
      dispatch_semaphore_t wait=dispatch_semaphore_create(0);__block BOOL allowed=NO;
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted){allowed=granted;dispatch_semaphore_signal(wait);}];
      // Permission is user-controlled; keep the UI responsive by calling on its worker thread.
      if(dispatch_semaphore_wait(wait,dispatch_time(DISPATCH_TIME_NOW,60*NSEC_PER_SEC))||!allowed){error="Microphone permission not granted.";return false;}
    }
    NSError* failure=nil;
    NSDictionary* settings=@{AVFormatIDKey:@(kAudioFormatLinearPCM),AVSampleRateKey:@16000,AVNumberOfChannelsKey:@1,AVLinearPCMBitDepthKey:@16,AVLinearPCMIsFloatKey:@NO,AVLinearPCMIsBigEndianKey:@NO};
    recorder=[[AVAudioRecorder alloc] initWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]] settings:settings error:&failure];
    if(!recorder||![recorder record]){error=failure?[[failure localizedDescription] UTF8String]:"Cannot record microphone";recorder=nil;return false;}return true;
  }
}
void microphone_stop(){@autoreleasepool{[recorder stop];recorder=nil;}}
static CGEventRef hotkey(CGEventTapProxy,CGEventType type,CGEventRef event,void*) {
  if(type==kCGEventTapDisabledByTimeout||type==kCGEventTapDisabledByUserInput){if(event_tap)CGEventTapEnable(event_tap,true);return event;}
  const auto flags=CGEventGetFlags(event);
  if(type==kCGEventKeyDown&&CGEventGetIntegerValueField(event,kCGKeyboardEventKeycode)==15 &&
     (flags&kCGEventFlagMaskControl)&&(flags&kCGEventFlagMaskAlternate)&&!CGEventGetIntegerValueField(event,kCGKeyboardEventAutorepeat))shortcut_callback();
  return event;
}
bool shortcut_start(std::function<void()> callback,std::string& error) {
  if(!CGPreflightListenEventAccess()){CGRequestListenEventAccess();error="Allow Input Monitoring for Dictation/your terminal, then enable the shortcut again. TUI Record still works.";return false;}
  shortcut_callback=std::move(callback);
  event_tap=CGEventTapCreate(kCGSessionEventTap,kCGHeadInsertEventTap,kCGEventTapOptionListenOnly,CGEventMaskBit(kCGEventKeyDown),hotkey,nullptr);
  if(!event_tap){error="Global shortcut unavailable. Check Input Monitoring permission.";return false;}
  quit_shortcut=false;
  shortcut_thread=std::thread([]{auto source=CFMachPortCreateRunLoopSource(kCFAllocatorDefault,event_tap,0);auto loop=CFRunLoopGetCurrent();CFRunLoopAddSource(loop,source,kCFRunLoopCommonModes);CGEventTapEnable(event_tap,true);while(!quit_shortcut)CFRunLoopRunInMode(kCFRunLoopDefaultMode,.1,false);CFRunLoopRemoveSource(loop,source,kCFRunLoopCommonModes);CFRelease(source);});return true;
}
void shortcut_stop(){quit_shortcut=true;if(shortcut_thread.joinable())shortcut_thread.join();if(event_tap){CGEventTapEnable(event_tap,false);CFMachPortInvalidate(event_tap);CFRelease(event_tap);}event_tap=nullptr;}
long focused_application(){@autoreleasepool{return [[NSWorkspace sharedWorkspace] frontmostApplication].processIdentifier;}}
bool deliver_text(const std::string& text,bool type,long original_focus,std::string& error) {
  @autoreleasepool {
    auto pasteboard=[NSPasteboard generalPasteboard];[pasteboard clearContents];
    if(![pasteboard setString:[NSString stringWithUTF8String:text.c_str()] forType:NSPasteboardTypeString]){error="Cannot copy transcript";return false;}
    if(!type)return true;
    if(original_focus<=0||focused_application()!=original_focus){error="Focus changed: transcript copied, not typed into another app.";return true;}
    auto bundle=[[[NSWorkspace sharedWorkspace] frontmostApplication] bundleIdentifier];
    if([bundle isEqualToString:@"com.apple.Terminal"]||[bundle isEqualToString:@"com.googlecode.iterm2"]||[bundle isEqualToString:@"dev.warp.Warp"]){error="Transcript copied. Automatic paste into terminal applications is disabled.";return true;}
    if(!AXIsProcessTrusted()){error="Transcript copied. Enable Accessibility permission to paste into the focused app.";return true;}
    auto down=CGEventCreateKeyboardEvent(nullptr,9,true),up=CGEventCreateKeyboardEvent(nullptr,9,false);
    CGEventSetFlags(down,kCGEventFlagMaskCommand);CGEventSetFlags(up,kCGEventFlagMaskCommand);CGEventPost(kCGHIDEventTap,down);CGEventPost(kCGHIDEventTap,up);CFRelease(down);CFRelease(up);return true;
  }
}
