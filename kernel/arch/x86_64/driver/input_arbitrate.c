/* kernel/arch/x86_64/driver/input_arbitrate.c — x86_64 input device arbitration */
#include <driver/keyboard.h>
#include <driver/mouse.h>
#include <driver/xhci_hid.h>
#include <subsys/subsys.h>

#pragma weak xhci_hid_keyboard_bound
#pragma weak xhci_hid_mouse_bound
#pragma weak mouse_input_init
#pragma weak mouse_init

static int x86_keyboard_arbitrate_init(void)
{
    keyboard_input_init();
    if (xhci_hid_keyboard_bound && xhci_hid_keyboard_bound())
        return 0;
    return keyboard_init();
}

static int x86_mouse_arbitrate_init(void)
{
    if (mouse_input_init)
        mouse_input_init();
    if (xhci_hid_mouse_bound && xhci_hid_mouse_bound())
        return 0;
    return mouse_init ? mouse_init() : -1;
}

static const char *const kbd_arbitrate_requires[] = { "usb-hid" };
static const char *const mouse_arbitrate_requires[] = { "usb-hid", "keyboard" };

static int _x86_keyboard_register(void)
{
    return register_subsys_deps("keyboard", x86_keyboard_arbitrate_init,
                                SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL,
                                kbd_arbitrate_requires, 1);
}
SUBSYS_INITCALL(_x86_keyboard_register);

static int _x86_mouse_register(void)
{
    return register_subsys_deps("mouse", x86_mouse_arbitrate_init,
                                SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL,
                                mouse_arbitrate_requires, 2);
}
SUBSYS_INITCALL(_x86_mouse_register);
