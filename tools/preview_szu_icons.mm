#import <AppKit/AppKit.h>

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        if (argc != 3) return 1;
        NSImage* source = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
        if (!source) return 2;
        // Offscreen asset QA only; no application windows or user UI are controlled.
        NSImage* sheet = [[NSImage alloc] initWithSize:NSMakeSize(500, 180)];
        [sheet lockFocus];
        [[NSColor colorWithWhite:0.95 alpha:1] setFill]; NSRectFill(NSMakeRect(0, 0, 250, 180));
        [[NSColor colorWithWhite:0.12 alpha:1] setFill]; NSRectFill(NSMakeRect(250, 0, 250, 180));
        for (int dark = 0; dark <= 1; ++dark)
        {
            NSColor* tint = dark ? NSColor.whiteColor : NSColor.blackColor;
            NSString* label = dark ? @"Dark menu bar" : @"Light menu bar";
            [label drawAtPoint:NSMakePoint(dark * 250 + 20, 145)
                withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:14], NSForegroundColorAttributeName:tint}];
            for (int enlarged = 0; enlarged <= 1; ++enlarged)
            {
                CGFloat size = enlarged ? 72 : 18;
                NSImage* mask = [[NSImage alloc] initWithSize:NSMakeSize(size, size)];
                [mask lockFocus];
                [source drawInRect:NSMakeRect(0, 0, size, size)];
                [tint setFill];
                NSRectFillUsingOperation(NSMakeRect(0, 0, size, size), NSCompositingOperationSourceIn);
                [mask unlockFocus];
                [mask drawInRect:NSMakeRect(dark * 250 + (enlarged ? 120 : 35), enlarged ? 40 : 70, size, size)];
                [mask release];
            }
            [@"18 pt" drawAtPoint:NSMakePoint(dark * 250 + 25, 25)
                withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:12], NSForegroundColorAttributeName:tint}];
            [@"4x detail" drawAtPoint:NSMakePoint(dark * 250 + 128, 15)
                withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:12], NSForegroundColorAttributeName:tint}];
        }
        [sheet unlockFocus];
        NSBitmapImageRep* bitmap = [NSBitmapImageRep imageRepWithData:[sheet TIFFRepresentation]];
        [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
            writeToFile:[NSString stringWithUTF8String:argv[2]] atomically:YES];
        [source release]; [sheet release];
    }
    return 0;
}
