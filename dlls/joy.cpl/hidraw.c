/*
 * Game controllers control panel: HID devices page
 *
 * Lists every device winebus exposes with the backend serving it, and lets
 * the user force hidraw on or off per VID/PID. winebus reloads the override
 * as soon as it is written.
 *
 * Copyright 2026 Samuel Rounce
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winreg.h>
#include <commctrl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidusage.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "dbt.h"

#include "wine/debug.h"
#include "wine/list.h"

#include "joy_private.h"

/* after joy_private.h so the dinput GUIDs are not instantiated here too */
#include "initguid.h"
#include "devpkey.h"

WINE_DEFAULT_DEBUG_CHANNEL(joycpl);

static const WCHAR winebus_key[] = L"System\\CurrentControlSet\\Services\\WineBus";

struct hid_device
{
    struct list entry;
    WCHAR product[128];
    WCHAR parent_id[MAX_DEVICE_ID_LEN]; /* winebus PDO, one per physical device */
    USHORT vid, pid;
    USAGE usage_page, usage;
    BOOL hidraw;
    BOOL present;        /* served by winebus; FALSE for a node it could not open at all */
    WCHAR node[64];      /* hidraw node winebus could not open, from HKEY_DYN_DATA\WineBus\Nodes */
    WCHAR node_error[64];
};

static struct list hid_devices = LIST_INIT( hid_devices );

static void merge_inaccessible_nodes(void);

static void clear_hid_devices(void)
{
    struct hid_device *device, *next;

    LIST_FOR_EACH_ENTRY_SAFE( device, next, &hid_devices, struct hid_device, entry )
    {
        list_remove( &device->entry );
        free( device );
    }
}

/* hidclass exposes one interface per top-level collection, keep the first */
static struct hid_device *find_hid_device( const WCHAR *parent_id )
{
    struct hid_device *device;

    LIST_FOR_EACH_ENTRY( device, &hid_devices, struct hid_device, entry )
        if (!wcsicmp( device->parent_id, parent_id )) return device;
    return NULL;
}

static BOOL multi_sz_contains( const WCHAR *list, const WCHAR *value )
{
    for (; *list; list += wcslen( list ) + 1)
        if (!wcsicmp( list, value )) return TRUE;
    return FALSE;
}

/* The HID interface belongs to the hidclass child; the winebus PDO above it
 * carries the WINEBUS compatible IDs that say which backend serves it. */
static BOOL get_winebus_backend( HDEVINFO set, SP_DEVINFO_DATA *child, struct hid_device *device )
{
    SP_DEVINFO_DATA parent = {.cbSize = sizeof(parent)};
    WCHAR *parent_id = device->parent_id, ids[1024];
    HDEVINFO parent_set;
    DEVPROPTYPE type;
    DWORD size;
    BOOL ret;

    if (!SetupDiGetDevicePropertyW( set, child, &DEVPKEY_Device_Parent, &type, (BYTE *)parent_id,
                                    sizeof(device->parent_id), &size, 0 ))
        return FALSE;

    if ((parent_set = SetupDiCreateDeviceInfoList( NULL, NULL )) == INVALID_HANDLE_VALUE) return FALSE;
    ret = SetupDiOpenDeviceInfoW( parent_set, parent_id, NULL, 0, &parent ) &&
          SetupDiGetDeviceRegistryPropertyW( parent_set, &parent, SPDRP_COMPATIBLEIDS, NULL, (BYTE *)ids,
                                             sizeof(ids) - sizeof(WCHAR), NULL );
    SetupDiDestroyDeviceInfoList( parent_set );
    if (!ret) return FALSE;

    if (!multi_sz_contains( ids, L"WINEBUS\\WINE_COMP_HID" )) return FALSE;
    device->hidraw = multi_sz_contains( ids, L"WINEBUS\\WINE_COMP_HIDRAW" );
    return TRUE;
}

static BOOL probe_hid_device( const WCHAR *path, struct hid_device *device )
{
    PHIDP_PREPARSED_DATA preparsed;
    HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
    HIDP_CAPS caps;
    HANDLE file;
    BOOL ret;

    /* a game may hold the device exclusively, attributes need no access rights */
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
        file = CreateFileW( path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    ret = HidD_GetAttributes( file, &attrs );
    if (ret && (ret = HidD_GetPreparsedData( file, &preparsed )))
    {
        ret = HidP_GetCaps( preparsed, &caps ) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData( preparsed );
    }
    if (ret && !HidD_GetProductString( file, device->product, sizeof(device->product) ))
        device->product[0] = 0;
    CloseHandle( file );
    if (!ret) return FALSE;

    device->vid = attrs.VendorID;
    device->pid = attrs.ProductID;
    device->usage_page = caps.UsagePage;
    device->usage = caps.Usage;
    return TRUE;
}

static void enum_hid_devices(void)
{
    char buffer[FIELD_OFFSET( SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath[MAX_PATH] )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (void *)buffer;
    SP_DEVICE_INTERFACE_DATA iface = {.cbSize = sizeof(iface)};
    SP_DEVINFO_DATA devinfo = {.cbSize = sizeof(devinfo)};
    struct hid_device *device;
    HDEVINFO set;
    GUID hid_guid;
    DWORD i;

    clear_hid_devices();

    HidD_GetHidGuid( &hid_guid );
    set = SetupDiGetClassDevsW( &hid_guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT );
    if (set == INVALID_HANDLE_VALUE) return;

    for (i = 0; SetupDiEnumDeviceInterfaces( set, NULL, &hid_guid, i, &iface ); i++)
    {
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW( set, &iface, detail, sizeof(buffer), NULL, &devinfo )) continue;
        if (!(device = calloc( 1, sizeof(*device) ))) break;

        if (!get_winebus_backend( set, &devinfo, device ) || find_hid_device( device->parent_id ) ||
            !probe_hid_device( detail->DevicePath, device ))
        {
            free( device );
            continue;
        }

        /* the winebus mouse and keyboard emulation devices have nothing to switch */
        if (!device->hidraw && device->usage_page == HID_USAGE_PAGE_GENERIC &&
            (device->usage == HID_USAGE_GENERIC_MOUSE || device->usage == HID_USAGE_GENERIC_KEYBOARD))
        {
            free( device );
            continue;
        }

        TRACE( "%s vid %04x pid %04x usage %04x:%04x hidraw %u\n", debugstr_w(detail->DevicePath), device->vid,
               device->pid, device->usage_page, device->usage, device->hidraw );
        device->present = TRUE;
        list_add_tail( &hid_devices, &device->entry );
    }

    SetupDiDestroyDeviceInfoList( set );
    merge_inaccessible_nodes();
}

static struct hid_device *find_hid_device_by_id( USHORT vid, USHORT pid )
{
    struct hid_device *device;

    LIST_FOR_EACH_ENTRY( device, &hid_devices, struct hid_device, entry )
        if (device->vid == vid && device->pid == pid && device->present) return device;
    return NULL;
}

static DWORD get_node_dword( HKEY key, const WCHAR *name )
{
    DWORD value = 0, size = sizeof(value);
    RegQueryValueExW( key, name, NULL, NULL, (BYTE *)&value, &size );
    return value;
}

/* Nodes winebus could not open, published under HKEY_DYN_DATA\WineBus\Nodes.
 * Annotates the device when winebus serves it another way, otherwise adds a
 * row so the user sees why the device is missing. Keyboards, mice and
 * digitizers are left out, winebus would not take those on hidraw anyway. */
static void merge_inaccessible_nodes(void)
{
    WCHAR name[64], product[128];
    struct hid_device *device;
    DWORD i, size, len;
    HKEY key, sub;

    if (RegOpenKeyExW( HKEY_DYN_DATA, L"WineBus\\Nodes", 0, KEY_READ, &key )) return;

    for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL ); i++)
    {
        USHORT vid, pid;
        USAGE page, usage;

        if (RegOpenKeyExW( key, name, 0, KEY_READ, &sub )) continue;
        vid = get_node_dword( sub, L"VID" );
        pid = get_node_dword( sub, L"PID" );
        page = get_node_dword( sub, L"UsagePage" );
        usage = get_node_dword( sub, L"Usage" );

        if (!(device = find_hid_device_by_id( vid, pid )))
        {
            BOOL unsupported = page == HID_USAGE_PAGE_DIGITIZER ||
                               (page == HID_USAGE_PAGE_GENERIC &&
                                (usage == HID_USAGE_GENERIC_MOUSE || usage == HID_USAGE_GENERIC_KEYBOARD));
            if (unsupported || !(device = calloc( 1, sizeof(*device) )))
            {
                RegCloseKey( sub );
                continue;
            }
            device->vid = vid;
            device->pid = pid;
            device->usage_page = page;
            device->usage = usage;
            size = sizeof(product);
            if (!RegQueryValueExW( sub, L"Product", NULL, NULL, (BYTE *)product, &size )) wcscpy( device->product, product );
            list_add_tail( &hid_devices, &device->entry );
        }

        size = sizeof(device->node);
        RegQueryValueExW( sub, L"Node", NULL, NULL, (BYTE *)device->node, &size );
        size = sizeof(device->node_error);
        RegQueryValueExW( sub, L"Error", NULL, NULL, (BYTE *)device->node_error, &size );
        TRACE( "node %s vid %04x pid %04x usage %04x:%04x %s: %s\n", debugstr_w(device->node), vid, pid, page, usage,
               device->present ? "served otherwise" : "missing", debugstr_w(device->node_error) );
        RegCloseKey( sub );
    }

    RegCloseKey( key );
}

/* Per-device override under WineBus\Devices\VVVV/PPPP, -1 when unset. */
static INT get_device_override( USHORT vid, USHORT pid, BOOL vendor_wide )
{
    WCHAR path[ARRAY_SIZE(winebus_key) + 32];
    DWORD value, size = sizeof(value);
    INT override = -1;
    HKEY key;

    if (vendor_wide) swprintf( path, ARRAY_SIZE(path), L"%s\\Devices\\%04X", winebus_key, vid );
    else swprintf( path, ARRAY_SIZE(path), L"%s\\Devices\\%04X/%04X", winebus_key, vid, pid );

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key )) return -1;
    if (!RegQueryValueExW( key, L"Hidraw", NULL, NULL, (BYTE *)&value, &size )) override = !!value;
    RegCloseKey( key );
    return override;
}

static void set_device_override( USHORT vid, USHORT pid, INT override )
{
    WCHAR path[ARRAY_SIZE(winebus_key) + 32];
    DWORD value = override;
    HKEY key;

    swprintf( path, ARRAY_SIZE(path), L"%s\\Devices\\%04X/%04X", winebus_key, vid, pid );
    if (override < 0)
    {
        RegDeleteKeyW( HKEY_LOCAL_MACHINE, path );
        return;
    }
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"Hidraw", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static struct hid_device *get_selected_device( HWND hwnd )
{
    HWND list = GetDlgItem( hwnd, IDC_HID_LIST );
    LVITEMW item = {.mask = LVIF_PARAM};

    if ((item.iItem = SendMessageW( list, LVM_GETNEXTITEM, -1, LVNI_SELECTED )) < 0) return NULL;
    if (!SendMessageW( list, LVM_GETITEMW, 0, (LPARAM)&item )) return NULL;
    return (struct hid_device *)item.lParam;
}

/* The older EnableHidraw list, a REG_MULTI_SZ of VVVV:PPPP entries that
 * community scripts still write. Left alone here, shown so the user knows
 * why a device is on hidraw. */
static BOOL is_hidraw_forced( USHORT vid, USHORT pid )
{
    WCHAR vidpid[16], list[2048], *entry;
    DWORD size = sizeof(list);
    HKEY key;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, winebus_key, 0, KEY_READ, &key )) return FALSE;
    if (RegQueryValueExW( key, L"EnableHidraw", NULL, NULL, (BYTE *)list, &size )) size = 0;
    RegCloseKey( key );
    if (size < 2 * sizeof(WCHAR)) return FALSE;
    list[size / sizeof(WCHAR) - 1] = 0;

    swprintf( vidpid, ARRAY_SIZE(vidpid), L"%04X:%04X", vid, pid );
    for (entry = list; *entry; entry += wcslen( entry ) + 1)
        if (!wcsnicmp( entry, vidpid, 9 )) return TRUE;
    return FALSE;
}

/* Mirrors the precedence in winebus: the per-device key, then a vendor-wide
 * Devices\VVVV key, then EnableHidraw, then the usage of the device. */
static void describe_device( const struct hid_device *device, INT override, WCHAR *text, SIZE_T len )
{
    INT vendor = get_device_override( device->vid, device->pid, TRUE );
    SIZE_T used;

    if (!device->present)
    {
        swprintf( text, len, L"Not accessible: winebus cannot open %s (%s). Grant access with a udev rule, "
                             L"for example TAG+=\"uaccess\" for this VID/PID, then replug the device.",
                  device->node, device->node_error );
        return;
    }

    if (device->node[0])
    {
        used = swprintf( text, len, L"Its hidraw node %s is not readable (%s), so it is served through SDL / evdev. "
                                    L"Forcing hidraw has no effect until a udev rule grants access. ",
                         device->node, device->node_error );
        if (override < 0 && vendor < 0 && !is_hidraw_forced( device->vid, device->pid )) return;
        text += used;
        len -= used;
    }

    if (override >= 0)
        swprintf( text, len, L"Per-device override for %04X:%04X. Default lets winebus decide again.",
                  device->vid, device->pid );
    else if (vendor >= 0)
        swprintf( text, len, L"Forced to %s by a vendor-wide Devices\\%04X registry key (read-only here).",
                  vendor ? L"hidraw" : L"SDL / evdev", device->vid );
    else if (is_hidraw_forced( device->vid, device->pid ))
        swprintf( text, len, L"Forced to hidraw by the EnableHidraw registry list (read-only here)." );
    else
        swprintf( text, len, L"Default: gamepads go through SDL / evdev, every other HID device through hidraw. "
                             L"A device without a readable hidraw node stays on SDL / evdev." );
}

static void update_override_controls( HWND hwnd )
{
    static const UINT ids[] = {IDC_HID_DEFAULT, IDC_HID_HIDRAW, IDC_HID_EVDEV};
    struct hid_device *device = get_selected_device( hwnd );
    WCHAR text[256] = L"Select a device to choose the backend it is served through.";
    INT override = -1;
    UINT i, checked;

    if (device)
    {
        override = get_device_override( device->vid, device->pid, FALSE );
        describe_device( device, override, text, ARRAY_SIZE(text) );
    }
    checked = override < 0 ? IDC_HID_DEFAULT : override ? IDC_HID_HIDRAW : IDC_HID_EVDEV;

    for (i = 0; i < ARRAY_SIZE(ids); i++)
    {
        EnableWindow( GetDlgItem( hwnd, ids[i] ), device && device->present );
        CheckDlgButton( hwnd, ids[i], device && device->present && ids[i] == checked ? BST_CHECKED : BST_UNCHECKED );
    }
    SetDlgItemTextW( hwnd, IDC_HID_INFO, text );
}

static void select_device( HWND hwnd, USHORT vid, USHORT pid )
{
    HWND list = GetDlgItem( hwnd, IDC_HID_LIST );
    LVITEMW item = {.mask = LVIF_PARAM};
    struct hid_device *device;
    INT count = SendMessageW( list, LVM_GETITEMCOUNT, 0, 0 );

    for (item.iItem = 0; item.iItem < count; item.iItem++)
    {
        if (!SendMessageW( list, LVM_GETITEMW, 0, (LPARAM)&item )) continue;
        device = (struct hid_device *)item.lParam;
        if (device->vid != vid || device->pid != pid) continue;
        item.mask = LVIF_STATE;
        item.state = item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW( list, LVM_SETITEMSTATE, item.iItem, (LPARAM)&item );
        return;
    }
}

static const WCHAR *usage_name( USAGE page, USAGE usage, WCHAR *buffer, SIZE_T len )
{
    if (page >= 0xff00) return L"Vendor defined";
    if (page == HID_USAGE_PAGE_GENERIC)
    {
        switch (usage)
        {
        case HID_USAGE_GENERIC_MOUSE: return L"Mouse";
        case HID_USAGE_GENERIC_JOYSTICK: return L"Joystick";
        case HID_USAGE_GENERIC_GAMEPAD: return L"Gamepad";
        case HID_USAGE_GENERIC_KEYBOARD: return L"Keyboard";
        case HID_USAGE_GENERIC_MULTI_AXIS_CONTROLLER: return L"Multi-axis controller";
        }
    }
    if (page == HID_USAGE_PAGE_SIMULATION) return L"Simulation";
    if (page == HID_USAGE_PAGE_CONSUMER) return L"Consumer";
    if (page == HID_USAGE_PAGE_DIGITIZER) return L"Digitizer";
    swprintf( buffer, len, L"%04x:%04x", page, usage );
    return buffer;
}

static void refresh_hid_list( HWND hwnd )
{
    HWND list = GetDlgItem( hwnd, IDC_HID_LIST );
    struct hid_device *device;
    WCHAR buffer[64];
    LVITEMW item = {.mask = LVIF_TEXT | LVIF_PARAM};
    INT override, index = 0;
    USHORT selected_vid = 0, selected_pid = 0;
    BOOL had_selection;

    /* a backend switch re-creates the device, keep it selected by VID/PID */
    if ((had_selection = !!(device = get_selected_device( hwnd ))))
    {
        selected_vid = device->vid;
        selected_pid = device->pid;
    }

    enum_hid_devices();

    SendMessageW( list, WM_SETREDRAW, FALSE, 0 );
    SendMessageW( list, LVM_DELETEALLITEMS, 0, 0 );

    LIST_FOR_EACH_ENTRY( device, &hid_devices, struct hid_device, entry )
    {
        item.iItem = index++;
        item.iSubItem = 0;
        item.lParam = (LPARAM)device;
        item.pszText = device->product[0] ? device->product : (WCHAR *)L"(unknown)";
        item.iItem = SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );

        swprintf( buffer, ARRAY_SIZE(buffer), L"%04X:%04X", device->vid, device->pid );
        item.iSubItem = 1;
        item.pszText = buffer;
        SendMessageW( list, LVM_SETITEMTEXTW, item.iItem, (LPARAM)&item );

        item.iSubItem = 2;
        item.pszText = (WCHAR *)usage_name( device->usage_page, device->usage, buffer, ARRAY_SIZE(buffer) );
        SendMessageW( list, LVM_SETITEMTEXTW, item.iItem, (LPARAM)&item );

        item.iSubItem = 3;
        item.pszText = (WCHAR *)(!device->present ? L"none" : device->hidraw ? L"hidraw" : L"SDL / evdev");
        SendMessageW( list, LVM_SETITEMTEXTW, item.iItem, (LPARAM)&item );

        override = get_device_override( device->vid, device->pid, FALSE );
        item.iSubItem = 4;
        item.pszText = (WCHAR *)(override < 0 ? L"" : override ? L"hidraw" : L"SDL / evdev");
        SendMessageW( list, LVM_SETITEMTEXTW, item.iItem, (LPARAM)&item );
    }

    if (had_selection) select_device( hwnd, selected_vid, selected_pid );
    SendMessageW( list, WM_SETREDRAW, TRUE, 0 );
    update_override_controls( hwnd );
}

static void init_hid_list( HWND hwnd )
{
    static const struct { const WCHAR *name; int width; } columns[] =
    {
        {L"Device", 150}, {L"VID:PID", 60}, {L"Usage", 90}, {L"Backend", 70}, {L"Override", 70},
    };
    HWND list = GetDlgItem( hwnd, IDC_HID_LIST );
    LVCOLUMNW column = {.mask = LVCF_TEXT | LVCF_WIDTH};
    UINT i;

    SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT );
    for (i = 0; i < ARRAY_SIZE(columns); i++)
    {
        column.pszText = (WCHAR *)columns[i].name;
        column.cx = columns[i].width;
        SendMessageW( list, LVM_INSERTCOLUMNW, i, (LPARAM)&column );
    }
}

INT_PTR CALLBACK hidraw_dialog_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    DEV_BROADCAST_DEVICEINTERFACE_W filter =
    {
        .dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_W),
        .dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE,
    };
    struct hid_device *device;
    INT override;

    TRACE( "hwnd %p, msg %#x, wparam %#Ix, lparam %#Ix\n", hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_INITDIALOG:
        HidD_GetHidGuid( &filter.dbcc_classguid );
        RegisterDeviceNotificationW( hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE );
        init_hid_list( hwnd );
        refresh_hid_list( hwnd );
        return TRUE;

    case WM_DEVICECHANGE:
        /* a backend switch removes and re-creates the device, coalesce the two */
        SetTimer( hwnd, 1, 500, NULL );
        return TRUE;

    case WM_TIMER:
        KillTimer( hwnd, wparam );
        refresh_hid_list( hwnd );
        return TRUE;

    case WM_NOTIFY:
        if (((NMHDR *)lparam)->idFrom == IDC_HID_LIST && ((NMHDR *)lparam)->code == LVN_ITEMCHANGED)
            update_override_controls( hwnd );
        return TRUE;

    case WM_COMMAND:
        if (HIWORD(wparam) != BN_CLICKED) return FALSE;
        switch (LOWORD(wparam))
        {
        case IDC_HID_DEFAULT: override = -1; break;
        case IDC_HID_HIDRAW: override = 1; break;
        case IDC_HID_EVDEV: override = 0; break;
        default: return FALSE;
        }
        if (!(device = get_selected_device( hwnd ))) return TRUE;
        set_device_override( device->vid, device->pid, override );
        refresh_hid_list( hwnd );
        return TRUE;

    case WM_DESTROY:
        clear_hid_devices();
        return TRUE;
    }

    return FALSE;
}
