//
//  RiggerApp.m
//  RiggerStatusItem
//
//  Created by Wouter Wijngaards on 8/29/11.
//  Copyright 2011 NLnet Labs. All rights reserved.
//
#include "config.h"
#include "cfg.h"
#include "log.h"
#include "osxattach.h"
#import "RiggerApp.h"

extern char* test_config_file;

/* if true logs trace for debug */
static int verb = 0;
static NSLock* feed_lock = NULL;

/** basically these are the commandline arguments to PanelAlert */
static NSLock* alert_lock;
static struct alert_arg alertinfo;

static RiggerApp* mainapp = NULL;

static void cleanup(void)
{
	[mainapp dealloc];
}

static void lock_feed_lock(void)
{
	[feed_lock lock];
}

static void unlock_feed_lock(void)
{
	[feed_lock unlock];
}

static void feed_quit(void)
{
	[feed_lock unlock];
	/* calls cleanup() atexit */
	exit(0);
}

static void feed_alert(struct alert_arg* a)
{
	/* store parameters in threadsafe manner */
	[alert_lock lock];
	alertinfo = *a;
	[alert_lock unlock];
	if(verb) printf("panel alert state in attach\n");
	[mainapp performSelectorOnMainThread:@selector(PanelAlert)
							  withObject:nil waitUntilDone:NO];
}

@implementation NowebDelegate
-(BOOL)windowShouldClose:(NSWindow*)sender
{
	if(verb) NSLog(@"nowebclose handler");
	/* like pressing skip */
	mainapp->noweb_asked = 1;
	attach_send_skip_http();
	return YES;
}
@end /* of NowebDelegate */

@implementation RiggerApp

-(void)
awakeFromNib
{
	char* cfgfile = CONFIGFILE;
	if(test_config_file)
		cfgfile = test_config_file;
	
	/* Setup the status icon in the tray */
	riggeritem = [[[NSStatusBar	systemStatusBar]
				   statusItemWithLength:NSSquareStatusItemLength] retain];
	NSBundle* bundle = [NSBundle mainBundle];
	/* Note that the icon images are 18x18px in size and are png with
	 * transparency.  This makes the tray icon work on OSX.  Other
	 * sizes do not work. */
	icon = [[NSImage alloc] initWithContentsOfFile:
			[bundle pathForResource:@"status-icon" ofType:@"png"]];
	icon_alert = [[NSImage alloc] initWithContentsOfFile:
				  [bundle pathForResource:@"status-icon-alert" ofType:@"png"]];
	[riggeritem setImage:icon];
	/* [riggeritem setAlternateImage:icon_alert]; this would be the highlight image but
	 * we use the builtin highlight code that uses the alpha channel in the icon */
	[riggeritem setMenu:riggermenu];
	[riggeritem setToolTip:@"dnssec-trigger"];
	/* highlight the icon when the statusmenu is shown */
	[riggeritem setHighlightMode:YES];
	
	/* Init */
	mainapp = self;
	unsafe_asked = 0;
	unsafe_should = 0;
	noweb_asked = 0;
	memset(&alertinfo, 0, sizeof(alertinfo));
	alert_lock = [NSLock alloc];
	log_ident_set("dnssec-trigger-panel-osx");
	log_init(NULL, 0, NULL);
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	(void)SSL_library_init();
	
	/* Read config */
	cfg = cfg_create(cfgfile);
	if(!cfg)
		fatal_exit("cannot read config file %s", cfgfile);

	/* spawn the feed thread */
	attach_create();
	feed_lock = [NSLock alloc];
	feed->lock = &lock_feed_lock;
	feed->unlock = &unlock_feed_lock;
	feed->quit = &feed_quit;
	feed->alert = &feed_alert;
	atexit(&cleanup);
	[NSThread detachNewThreadSelector:@selector(SpawnFeed:)
							 toTarget:self withObject:nil];
}

-(void)
dealloc
{
	if(verb) printf("dealloc routine\n");
	attach_stop();
	[feed_lock release];
	free(feed);
	[alert_lock release];
	[icon release];
	[icon_alert release];
	[super dealloc];
}

-(void)SpawnFeed:(id)param
{
	attach_start(cfg);
}

-(IBAction)Reprobe:(id)sender
{
	if(verb) NSLog(@"Reprobe");
	attach_send_reprobe();
}

void append_txt(NSTextView* pane, char* str)
{
	NSRange range;
	range.location = [[pane textStorage] length];
	range.length = 0;
	NSString* s = [NSString stringWithUTF8String:str];
	[pane replaceCharactersInRange: range withString: s];
	/* because the string is allocated with convenience function
	 * no need to [s release]. */
}

-(IBAction)ProbeResults:(id)sender
{
	if(verb) NSLog(@"ProbeResults");
	/* this is to help us bring a window to the front
	 * from the hidden app */
	[NSApp activateIgnoringOtherApps:YES];
	[resultpane setEditable:YES];
	NSRange range;
	range.location = 0;
	range.length = [[resultpane textStorage] length];

	[resultpane replaceCharactersInRange: range
		withString:@""];
	char buf[102400];
	fetch_proberesults(buf, sizeof(buf), "\n");
	append_txt(resultpane, buf);
	
	[resultpane setEditable:NO];
	[resultpane setSelectable:YES];
	[resultwindow setLevel:NSScreenSaverWindowLevel + 1];
	[resultwindow orderFront:sender];
}

-(IBAction)ProbeResultsOK:(id)sender
{
	if(verb) NSLog(@"ProbeResultsOK");
	[resultwindow orderOut:sender];
}

-(IBAction)UnsafeInsecure:(id)sender
{
	if(verb) NSLog(@"Unsafe:Insecure");
	[unsafewindow orderOut:sender];
	unsafe_asked = 1;
	unsafe_should = 0;
	attach_send_insecure(1);
}

-(IBAction)UnsafeDisconnect:(id)sender
{
	if(verb) NSLog(@"Unsafe:Disconnect");
	[unsafewindow orderOut:sender];
	unsafe_asked = 1;
	unsafe_should = 0;
	attach_send_insecure(0);
}

-(IBAction)HotspotSignon:(id)sender
{
    if(verb) NSLog(@"menu-hotspotsignon");
    /* this is to help us bring a window to the front
     * from the hidden app */
    [NSApp activateIgnoringOtherApps:YES];
    if([unsafewindow isVisible])
	    [unsafewindow orderOut:sender];
    [hotsignwindow center];
    [hotsignwindow deminiaturize:sender];
    [hotsignwindow setLevel:NSScreenSaverWindowLevel + 1];
    [hotsignwindow orderFront:sender];
}

-(IBAction)HotsignOK:(id)sender
{
    if(verb) NSLog(@"hotsign ok");
    attach_send_hotspot_signon();
    [hotsignwindow orderOut:sender];
    unsafe_asked = 1;
    unsafe_should = 0;
}

-(IBAction)HotsignCancel:(id)sender
{
    if(verb) NSLog(@"hotsign cancel");
    [hotsignwindow orderOut:sender];
    if(unsafe_should) [mainapp PresentUnsafeDialog];
}

-(IBAction)NowebLogin:(id)sender
{
	if(verb) NSLog(@"noweb login");
    	[nowebwindow orderOut:sender];
	noweb_asked = 1;
	attach_send_insecure(1);
}

-(IBAction)NowebSkip:(id)sender
{
	if(verb) NSLog(@"noweb skip");
    	[nowebwindow orderOut:sender];
	noweb_asked = 1;
	attach_send_skip_http();
}

-(void)PresentNowebDialog
{
	[nowebwindow center];
	[nowebwindow deminiaturize:nil];
        [nowebwindow setLevel:NSScreenSaverWindowLevel + 1];
	[nowebwindow orderFront:nil];
}

-(BOOL)windowShouldClose:(NSWindow*)sender
{
	if(verb) NSLog(@"unsafeclose handler");
	/* like pressing disconnect */
	unsafe_asked = 1;
	unsafe_should = 0;
	attach_send_insecure(0);
	return YES;
}

-(void)PresentUnsafeDialog
{
	unsafe_should = 1;
	if([hotsignwindow isVisible])
		return; /* wait for hotspot signon question to finish */
	[unsafewindow center];
	[unsafewindow deminiaturize:nil];
        [unsafewindow setLevel:NSScreenSaverWindowLevel + 1];
	[unsafewindow orderFront:nil];
}

-(void)PanelAlertDanger
{
	[riggeritem setImage:icon_alert];
}

-(void)PanelAlertSafe
{
	[riggeritem setImage:icon];
}

static void do_danger(void)
{
	[mainapp PanelAlertDanger];
}
static void do_safe(void)
{
	[mainapp PanelAlertSafe];
}
static void do_ask(void)
{
	[mainapp PresentUnsafeDialog];
}
static void do_noweb(void)
{
	[mainapp PresentNowebDialog];
}

-(void)PanelAlert
{
	NSString* tt;
	const char* ctt;
	struct alert_arg a;
	if(verb) NSLog(@"PanelAlert function");
	[alert_lock lock];
	a = alertinfo;
	[alert_lock unlock];

	ctt = state_tooltip(&a);
	/* no need to [tt release] because of convenience function */
	tt = [NSString stringWithUTF8String:ctt];
	[riggeritem setToolTip:tt];

	process_state(&a, &unsafe_asked, &noweb_asked, &do_danger, &do_safe, &do_ask,
		&do_noweb);
	if(!a.now_dark) unsafe_should = 0;
}

@end
