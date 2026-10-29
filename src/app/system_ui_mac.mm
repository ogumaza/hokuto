// SPDX-License-Identifier: MIT

// The Mac's menu bar and title bar, through AppKit. SDL3 builds a small menu bar when it starts. InstallMenuBar puts
// the program's menu bar in its place.
//
// SDL3 passes every key to the program before AppKit searches for a menu item with that key. The program leaves the
// keys with Command to the menu bar, and the menu bar leaves the keys without Command or Control to the program. Each
// key then gives its command once. A text field keeps the keys that type. Space, H and G are among them.
//
// A window of the system can hold the keyboard while the program's window stays open: a file panel, a question, the
// About panel, or a sheet over a panel. The Edit menu's Undo, Redo, Cut, Copy, Paste and Select All then send the
// system's actions to that window's first responder, such as the text field of a Save panel. Otherwise they give the
// program's commands. On macOS 27, the Save and Open panels run in another process. The panel's window in the program
// then gives a stand-in responder for the actions that the other process takes. The stand-in enables a menu item only
// when the item's action is the system's action. The six items therefore take the system's actions, such as selectAll:,
// and not perform:.
//
// SDL3's handlers of SIGINT and SIGTERM request a quit event. SDL3 sends it the next time that it polls its events.
// WakeOnQuitSignals ends a wait for events at those signals.

#include "app/system_ui.h"

#import <AppKit/AppKit.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// The menu bar. It takes only the keys with Command or Control. The program handles the others.
@interface HokutoMainMenu : NSMenu
@end

@implementation HokutoMainMenu

- (BOOL)performKeyEquivalent:(NSEvent*)event
{
    if ((event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) == 0)
    {
        return NO;
    }

    return [super performKeyEquivalent:event];
}

@end

namespace
{

// The system's action for an Edit menu command, or nothing for the program's other commands.
SEL SystemAction(hokuto::app::Command command)
{
    using hokuto::app::Command;
    switch (command)
    {
    case Command::kUndo:
        return @selector(undo:);
    case Command::kRedo:
        return @selector(redo:);
    case Command::kCut:
        return @selector(cut:);
    case Command::kCopy:
        return @selector(copy:);
    case Command::kPaste:
        return @selector(paste:);
    case Command::kSelectAll:
        return @selector(selectAll:);
    default:
        return nullptr;
    }
}

// Whether `window` is one of SDL3's windows: the program's window.
bool IsProgramWindow(NSWindow* window)
{
    int count = 0;
    SDL_Window** const windows = SDL_GetWindows(&count);
    bool found = false;
    for (int i = 0; i < count && !found; ++i)
    {
        found = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                       nullptr) == (__bridge void*)window;
    }
    SDL_free(windows);

    return found;
}

// Whether a window of the system holds the keyboard: any key window other than the program's. Such a window is a file
// panel, a question or the About panel, or a sheet over a panel, such as a file panel's Go to Folder. On macOS 27, that
// sheet is a window in the program's process with a view of the panel's process. The window isn't a panel, and its
// first responder isn't a text field.
bool SystemHasKeys()
{
    NSWindow* window = NSApp.keyWindow;
    return window != nil && !IsProgramWindow(window);
}

// Whether an app-modal session runs, such as a question of SDL_ShowMessageBox. The program waits for the answer inside
// SDL3's call. It runs none of its commands meanwhile.
bool ModalSessionRuns()
{
    return NSApp.modalWindow != nil;
}

} // namespace

// Passes the program's menu items to its MenuTarget. Each item's tag holds its command. While a window of the system
// holds the keyboard, the Edit menu's items go to that window's first responder instead. Those items take the system's
// actions. The other items take perform:.
@interface HokutoMenuTarget : NSObject <NSMenuItemValidation>

@property(nonatomic) hokuto::app::MenuTarget* target;

- (void)perform:(NSMenuItem*)item;

@end

@implementation HokutoMenuTarget

// During an app-modal session, AppKit enables only the items whose target works when modal. The Edit menu's items then
// go to the question's first responder, and Cmd+C copies the question's selected text. validateMenuItem: keeps the
// program's commands grey until the session ends.
- (BOOL)worksWhenModal
{
    return YES;
}

- (void)undo:(NSMenuItem*)item
{
    [self perform:item];
}

- (void)redo:(NSMenuItem*)item
{
    [self perform:item];
}

- (void)cut:(NSMenuItem*)item
{
    [self perform:item];
}

- (void)copy:(NSMenuItem*)item
{
    [self perform:item];
}

- (void)paste:(NSMenuItem*)item
{
    [self perform:item];
}

- (void)selectAll:(NSMenuItem*)item
{
    [self perform:item];
}

- (void)perform:(NSMenuItem*)item
{
    const auto command = static_cast<hokuto::app::Command>(item.tag);
    if (const SEL action = SystemAction(command); action != nullptr && SystemHasKeys())
    {
        [NSApp sendAction:action to:nil from:item];
        return;
    }

    if (ModalSessionRuns())
    {
        return;
    }

    self.target->Execute(command);
}

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    // The item's name in the menus is its represented object. The title can add to it, such as Undo's action. The first
    // responder that takes the system's action validates the item, and may name the action in its title too.
    const auto command = static_cast<hokuto::app::Command>(item.tag);
    if (const SEL action = SystemAction(command); action != nullptr && SystemHasKeys())
    {
        item.state = NSControlStateValueOff;
        if (NSString* name = item.representedObject)
        {
            item.title = name;
        }
        const id responder = [NSApp targetForAction:action to:nil from:item];
        if (responder == nil)
        {
            return NO;
        }
        if ([responder respondsToSelector:@selector(validateMenuItem:)])
        {
            return [responder validateMenuItem:item];
        }
        if ([responder respondsToSelector:@selector(validateUserInterfaceItem:)])
        {
            return [responder validateUserInterfaceItem:item];
        }
        return YES;
    }

    if (ModalSessionRuns())
    {
        return NO;
    }

    item.state = self.target->IsChecked(command) ? NSControlStateValueOn : NSControlStateValueOff;
    if (NSString* name = item.representedObject)
    {
        item.title = @(self.target->Title(command, name.UTF8String).c_str());
    }

    return self.target->IsEnabled(command) ? YES : NO;
}

@end

// The submenu of an item with `none`. It gets its titles from the target each time that it opens. Each item's tag holds
// its index among them.
@interface HokutoListedMenu : NSObject <NSMenuDelegate>

@property(nonatomic) hokuto::app::MenuTarget* target;
@property(nonatomic) hokuto::app::Command command;
@property(nonatomic) const char* none;

- (void)perform:(NSMenuItem*)item;

@end

@implementation HokutoListedMenu

- (void)menuNeedsUpdate:(NSMenu*)menu
{
    // An item without an action is grey. While the command can't run, every item is.
    [menu removeAllItems];
    const std::vector<std::string> titles = self.target->Listed(self.command);
    if (titles.empty())
    {
        [menu addItemWithTitle:@(self.none) action:nil keyEquivalent:@""];
        return;
    }

    const bool enabled = self.target->IsEnabled(self.command);
    for (std::size_t i = 0; i < titles.size(); i++)
    {
        NSMenuItem* item = [menu addItemWithTitle:@(titles[i].c_str())
                                           action:enabled ? @selector(perform:) : nil
                                    keyEquivalent:@""];
        item.target = self;
        item.tag = static_cast<NSInteger>(i);
    }
}

- (void)perform:(NSMenuItem*)item
{
    self.target->ExecuteListed(self.command, static_cast<std::size_t>(item.tag));
}

@end

namespace hokuto::app
{
namespace
{

// A menu item's key and modifiers in AppKit's terms.
struct KeyEquivalent
{
    NSString* key = @"";
    NSEventModifierFlags modifiers = 0;
};

NSString* StringOf(unichar character)
{
    return [NSString stringWithCharacters:&character length:1];
}

// The key equivalent of Dear ImGui's `keys`. Dear ImGui's Ctrl stands for the Command key on the Mac, and its Super for
// the Control key.
KeyEquivalent KeyEquivalentOf(ImGuiKeyChord keys)
{
    KeyEquivalent equivalent;
    const auto key = static_cast<ImGuiKey>(keys & ~ImGuiMod_Mask_);
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
    {
        equivalent.key = StringOf(static_cast<unichar>(u'a' + (key - ImGuiKey_A)));
    }
    else if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
    {
        equivalent.key = StringOf(static_cast<unichar>(NSF1FunctionKey + (key - ImGuiKey_F1)));
    }
    else if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
    {
        equivalent.key = StringOf(static_cast<unichar>(u'0' + (key - ImGuiKey_Keypad0)));
        equivalent.modifiers |= NSEventModifierFlagNumericPad;
    }
    else
    {
        switch (key)
        {
        case ImGuiKey_Space:
            equivalent.key = @" ";
            break;

        case ImGuiKey_Enter:
            equivalent.key = @"\r";
            break;

        case ImGuiKey_KeypadEnter:
            equivalent.key = StringOf(NSEnterCharacter);
            equivalent.modifiers |= NSEventModifierFlagNumericPad;
            break;

        case ImGuiKey_KeypadDecimal:
            equivalent.key = @".";
            equivalent.modifiers |= NSEventModifierFlagNumericPad;
            break;

        case ImGuiKey_KeypadDivide:
            equivalent.key = @"/";
            equivalent.modifiers |= NSEventModifierFlagNumericPad;
            break;

        case ImGuiKey_KeypadSubtract:
            equivalent.key = @"-";
            equivalent.modifiers |= NSEventModifierFlagNumericPad;
            break;

        case ImGuiKey_KeypadAdd:
            equivalent.key = @"+";
            equivalent.modifiers |= NSEventModifierFlagNumericPad;
            break;

        default:
            break;
        }
    }

    if ((keys & ImGuiMod_Ctrl) != 0)
    {
        equivalent.modifiers |= NSEventModifierFlagCommand;
    }
    if ((keys & ImGuiMod_Shift) != 0)
    {
        equivalent.modifiers |= NSEventModifierFlagShift;
    }
    if ((keys & ImGuiMod_Alt) != 0)
    {
        equivalent.modifiers |= NSEventModifierFlagOption;
    }
    if ((keys & ImGuiMod_Super) != 0)
    {
        equivalent.modifiers |= NSEventModifierFlagControl;
    }

    return equivalent;
}

NSMenuItem* AddItem(NSMenu* menu, NSString* title, SEL action, NSString* key, NSEventModifierFlags modifiers)
{
    NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;

    return item;
}

void AddSubmenu(NSMenu* bar, NSMenu* menu)
{
    NSMenuItem* item = [bar addItemWithTitle:menu.title action:nil keyEquivalent:@""];
    item.submenu = menu;
}

// Adds `items` to `menu`, with a submenu for each item that has `items` or `none`. `target` performs the items'
// commands.
void AddItems(NSMenu* menu, const std::vector<MenuItem>& items, HokutoMenuTarget* target)
{
    // A menu doesn't keep its delegate. These live as long as the program.
    static NSMutableArray* listed_menus = [NSMutableArray array];
    for (const MenuItem& item : items)
    {
        if (item.name == nullptr)
        {
            [menu addItem:NSMenuItem.separatorItem];
            continue;
        }

        if (item.none != nullptr)
        {
            HokutoListedMenu* listed = [[HokutoListedMenu alloc] init];
            listed.target = target.target;
            listed.command = item.command;
            listed.none = item.none;
            [listed_menus addObject:listed];
            NSMenu* submenu = [[NSMenu alloc] initWithTitle:@(item.name)];
            submenu.delegate = listed;
            AddSubmenu(menu, submenu);
            continue;
        }

        if (!item.items.empty())
        {
            NSMenu* submenu = [[NSMenu alloc] initWithTitle:@(item.name)];
            AddItems(submenu, item.items, target);
            AddSubmenu(menu, submenu);
            continue;
        }

        const KeyEquivalent keys = KeyEquivalentOf(item.keys);
        const SEL system_action = SystemAction(item.command);
        NSMenuItem* menu_item =
            AddItem(menu, @(item.name), system_action != nullptr ? system_action : @selector(perform:), keys.key,
                    keys.modifiers);
        menu_item.target = target;
        menu_item.tag = static_cast<NSInteger>(item.command);
        menu_item.representedObject = @(item.name);
    }
}

// The application menu and its standard items on macOS, with Hokuto's Preferences. `target` performs Preferences.
NSMenu* ApplicationMenu(HokutoMenuTarget* target)
{
    NSString* name = NSRunningApplication.currentApplication.localizedName ?: @"Hokuto";
    NSMenu* menu = [[NSMenu alloc] initWithTitle:name];
    AddItem(menu, [@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"", 0);
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* preferences = AddItem(menu, @"Preferences…", @selector(perform:), @",", NSEventModifierFlagCommand);
    preferences.target = target;
    preferences.tag = static_cast<NSInteger>(hokuto::app::Command::kPreferences);
    preferences.representedObject = @"Preferences…";
    [menu addItem:NSMenuItem.separatorItem];
    NSMenu* services = [[NSMenu alloc] initWithTitle:@"Services"];
    AddSubmenu(menu, services);
    NSApp.servicesMenu = services;
    [menu addItem:NSMenuItem.separatorItem];
    AddItem(menu, [@"Hide " stringByAppendingString:name], @selector(hide:), @"h", NSEventModifierFlagCommand);
    AddItem(menu, @"Hide Others", @selector(hideOtherApplications:), @"h",
            NSEventModifierFlagOption | NSEventModifierFlagCommand);
    AddItem(menu, @"Show All", @selector(unhideAllApplications:), @"", 0);
    [menu addItem:NSMenuItem.separatorItem];
    AddItem(menu, [@"Quit " stringByAppendingString:name], @selector(terminate:), @"q", NSEventModifierFlagCommand);

    return menu;
}

} // namespace

bool InstallMenuBar(std::span<const Menu> menus, MenuTarget& target)
{
    // Menu items don't keep their targets. This one lives as long as the program.
    static HokutoMenuTarget* menu_target = nil;
    menu_target = [[HokutoMenuTarget alloc] init];
    menu_target.target = &target;

    HokutoMainMenu* bar = [[HokutoMainMenu alloc] initWithTitle:@""];
    AddSubmenu(bar, ApplicationMenu(menu_target));
    for (const Menu& menu : menus)
    {
        NSMenu* submenu = [[NSMenu alloc] initWithTitle:@(menu.name)];
        const bool window_menu = [submenu.title isEqualToString:@"Window"];
        if (window_menu)
        {
            AddItem(submenu, @"Minimize", @selector(performMiniaturize:), @"m", NSEventModifierFlagCommand);
            AddItem(submenu, @"Zoom", @selector(performZoom:), @"", 0);
            [submenu addItem:NSMenuItem.separatorItem];
        }

        AddItems(submenu, menu.items, menu_target);

        // The window list follows these items. macOS adds it to the menu in NSApp.windowsMenu.
        if (window_menu)
        {
            [submenu addItem:NSMenuItem.separatorItem];
            AddItem(submenu, @"Bring All to Front", @selector(arrangeInFront:), @"", 0);
            NSApp.windowsMenu = submenu;
        }

        AddSubmenu(bar, submenu);
    }

    // The Help menu. macOS puts its search field in it.
    NSMenu* help = [[NSMenu alloc] initWithTitle:@"Help"];
    AddSubmenu(bar, help);
    NSApp.helpMenu = help;
    NSApp.mainMenu = bar;

    return true;
}

void SetTitleBarColour(SDL_Window* window, ImU32 colour)
{
    auto* const pointer =
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    NSWindow* ns_window = (__bridge NSWindow*)pointer;
    if (ns_window == nil)
    {
        return;
    }

    // A transparent title bar shows the window's background. The background lies behind the program's drawing.
    const auto channel = [colour](int shift)
    {
        return static_cast<CGFloat>((colour >> shift) & 0xFF) / 255.0;
    };

    ns_window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    ns_window.titlebarAppearsTransparent = YES;
    ns_window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    ns_window.backgroundColor = [NSColor colorWithSRGBRed:channel(IM_COL32_R_SHIFT)
                                                    green:channel(IM_COL32_G_SHIFT)
                                                     blue:channel(IM_COL32_B_SHIFT)
                                                    alpha:1.0];
}

void WakeOnQuitSignals(uint32_t event_type)
{
    // The dispatch sources on the main queue report each signal after SDL3's handler has run. The sources last as long
    // as the program.
    static NSMutableArray* sources = [NSMutableArray array];
    for (const int signal : {SIGINT, SIGTERM})
    {
        dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(signal),
                                                          0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(source, ^{
          SDL_Event event{};
          event.type = event_type;
          SDL_PushEvent(&event);
        });
        dispatch_resume(source);
        [sources addObject:source];
    }
}

std::string TimeOfDay(std::chrono::system_clock::time_point time)
{
    // The system clock counts from 1970. So does NSDate's interval since 1970. The medium style shows the seconds.
    @autoreleasepool
    {
        const double seconds = std::chrono::duration<double>(time.time_since_epoch()).count();
        NSDate* date = [NSDate dateWithTimeIntervalSince1970:seconds];
        NSString* text = [NSDateFormatter localizedStringFromDate:date
                                                        dateStyle:NSDateFormatterNoStyle
                                                        timeStyle:NSDateFormatterMediumStyle];
        const char* utf8 = text.UTF8String;
        return utf8 != nullptr ? std::string(utf8) : std::string();
    }
}

std::string SystemPath(const std::string& path)
{
    return path;
}

} // namespace hokuto::app
