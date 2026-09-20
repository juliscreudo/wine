/*
 * UnsecuredApartment implementation
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

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "wbemcli.h"
#include "wbemprov.h"

#include "wbemprox_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

/* A sink stub hands a client-supplied sink back to WMI wrapped in an
 * apartment-neutral object. Callbacks are delivered straight through; the
 * point of the wrapper is that it aggregates the free-threaded marshaler, so
 * the stub can be created on one apartment and passed to WMI from another.
 * .NET's System.Management relies on exactly that: SinkForEventQuery builds
 * its stub on a temporary MTA thread and then hands it to
 * IWbemServices::ExecNotificationQueryAsync from the calling apartment. */
struct sink_stub
{
    IWbemObjectSink IWbemObjectSink_iface;
    LONG refs;
    IUnknown *marshal;
    IWbemObjectSink *sink;
};

static inline struct sink_stub *impl_from_IWbemObjectSink( IWbemObjectSink *iface )
{
    return CONTAINING_RECORD( iface, struct sink_stub, IWbemObjectSink_iface );
}

static HRESULT WINAPI sink_stub_QueryInterface( IWbemObjectSink *iface, REFIID riid, void **obj )
{
    struct sink_stub *stub = impl_from_IWbemObjectSink( iface );

    TRACE( "%p, %s, %p\n", iface, debugstr_guid( riid ), obj );

    if (IsEqualGUID( riid, &IID_IWbemObjectSink ) ||
        IsEqualGUID( riid, &IID_IUnknown ))
    {
        *obj = iface;
    }
    else if (IsEqualGUID( riid, &IID_IMarshal ))
    {
        return IUnknown_QueryInterface( stub->marshal, riid, obj );
    }
    else
    {
        FIXME( "interface %s not implemented\n", debugstr_guid( riid ) );
        return E_NOINTERFACE;
    }
    IWbemObjectSink_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI sink_stub_AddRef( IWbemObjectSink *iface )
{
    struct sink_stub *stub = impl_from_IWbemObjectSink( iface );
    return InterlockedIncrement( &stub->refs );
}

static ULONG WINAPI sink_stub_Release( IWbemObjectSink *iface )
{
    struct sink_stub *stub = impl_from_IWbemObjectSink( iface );
    LONG refs = InterlockedDecrement( &stub->refs );

    if (!refs)
    {
        TRACE( "destroying %p\n", stub );
        IWbemObjectSink_Release( stub->sink );
        IUnknown_Release( stub->marshal );
        free( stub );
    }
    return refs;
}

static HRESULT WINAPI sink_stub_Indicate( IWbemObjectSink *iface, LONG count,
                                          IWbemClassObject **objects )
{
    struct sink_stub *stub = impl_from_IWbemObjectSink( iface );

    TRACE( "%p, %ld, %p\n", iface, count, objects );

    return IWbemObjectSink_Indicate( stub->sink, count, objects );
}

static HRESULT WINAPI sink_stub_SetStatus( IWbemObjectSink *iface, LONG flags, HRESULT hr,
                                           BSTR param, IWbemClassObject *obj )
{
    struct sink_stub *stub = impl_from_IWbemObjectSink( iface );

    TRACE( "%p, %ld, %#lx, %s, %p\n", iface, flags, hr, debugstr_w(param), obj );

    return IWbemObjectSink_SetStatus( stub->sink, flags, hr, param, obj );
}

static const IWbemObjectSinkVtbl sink_stub_vtbl =
{
    sink_stub_QueryInterface,
    sink_stub_AddRef,
    sink_stub_Release,
    sink_stub_Indicate,
    sink_stub_SetStatus,
};

static HRESULT sink_stub_create( IUnknown *object, IWbemObjectSink **obj )
{
    struct sink_stub *stub;
    IWbemObjectSink *sink;
    HRESULT hr;

    if (FAILED(hr = IUnknown_QueryInterface( object, &IID_IWbemObjectSink, (void **)&sink )))
        return hr;

    if (!(stub = calloc( 1, sizeof(*stub) )))
    {
        IWbemObjectSink_Release( sink );
        return E_OUTOFMEMORY;
    }
    stub->IWbemObjectSink_iface.lpVtbl = &sink_stub_vtbl;
    stub->refs = 1;
    stub->sink = sink;
    if (FAILED(hr = CoCreateFreeThreadedMarshaler( (IUnknown *)&stub->IWbemObjectSink_iface,
                                                   &stub->marshal )))
    {
        IWbemObjectSink_Release( sink );
        free( stub );
        return hr;
    }

    *obj = &stub->IWbemObjectSink_iface;
    TRACE( "returning iface %p wrapping %p\n", *obj, sink );
    return S_OK;
}

struct unsecured_apartment
{
    IWbemUnsecuredApartment IWbemUnsecuredApartment_iface;
    LONG refs;
    IUnknown *marshal;
};

static inline struct unsecured_apartment *impl_from_IWbemUnsecuredApartment(
    IWbemUnsecuredApartment *iface )
{
    return CONTAINING_RECORD( iface, struct unsecured_apartment, IWbemUnsecuredApartment_iface );
}

static HRESULT WINAPI unsecured_apartment_QueryInterface( IWbemUnsecuredApartment *iface,
                                                          REFIID riid, void **obj )
{
    struct unsecured_apartment *apartment = impl_from_IWbemUnsecuredApartment( iface );

    TRACE( "%p, %s, %p\n", iface, debugstr_guid( riid ), obj );

    if (IsEqualGUID( riid, &IID_IWbemUnsecuredApartment ) ||
        IsEqualGUID( riid, &IID_IUnsecuredApartment ) ||
        IsEqualGUID( riid, &IID_IUnknown ))
    {
        *obj = iface;
    }
    else if (IsEqualGUID( riid, &IID_IMarshal ))
    {
        return IUnknown_QueryInterface( apartment->marshal, riid, obj );
    }
    else
    {
        FIXME( "interface %s not implemented\n", debugstr_guid( riid ) );
        return E_NOINTERFACE;
    }
    IWbemUnsecuredApartment_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI unsecured_apartment_AddRef( IWbemUnsecuredApartment *iface )
{
    struct unsecured_apartment *apartment = impl_from_IWbemUnsecuredApartment( iface );
    return InterlockedIncrement( &apartment->refs );
}

static ULONG WINAPI unsecured_apartment_Release( IWbemUnsecuredApartment *iface )
{
    struct unsecured_apartment *apartment = impl_from_IWbemUnsecuredApartment( iface );
    LONG refs = InterlockedDecrement( &apartment->refs );

    if (!refs)
    {
        TRACE( "destroying %p\n", apartment );
        IUnknown_Release( apartment->marshal );
        free( apartment );
    }
    return refs;
}

static HRESULT WINAPI unsecured_apartment_CreateObjectStub( IWbemUnsecuredApartment *iface,
                                                            IUnknown *object, IUnknown **stub )
{
    IWbemObjectSink *sink;
    HRESULT hr;

    TRACE( "%p, %p, %p\n", iface, object, stub );

    if (!object || !stub) return WBEM_E_INVALID_PARAMETER;

    *stub = NULL;

    /* FIXME: native wraps an arbitrary object here; we only know how to wrap a
     * sink, which is all any known caller asks for. */
    if (FAILED(hr = sink_stub_create( object, &sink ))) return hr;

    *stub = (IUnknown *)sink;
    return S_OK;
}

static HRESULT WINAPI unsecured_apartment_CreateSinkStub( IWbemUnsecuredApartment *iface,
                                                          IWbemObjectSink *sink, DWORD flags,
                                                          const WCHAR *reserved,
                                                          IWbemObjectSink **stub )
{
    TRACE( "%p, %p, %#lx, %s, %p\n", iface, sink, flags, debugstr_w(reserved), stub );

    if (!sink || !stub) return WBEM_E_INVALID_PARAMETER;
    if (reserved) FIXME( "ignoring reserved %s\n", debugstr_w(reserved) );

    *stub = NULL;
    return sink_stub_create( (IUnknown *)sink, stub );
}

static const IWbemUnsecuredApartmentVtbl unsecured_apartment_vtbl =
{
    unsecured_apartment_QueryInterface,
    unsecured_apartment_AddRef,
    unsecured_apartment_Release,
    unsecured_apartment_CreateObjectStub,
    unsecured_apartment_CreateSinkStub,
};

HRESULT UnsecuredApartment_create( void **obj, REFIID riid )
{
    struct unsecured_apartment *apartment;
    HRESULT hr;

    TRACE( "%p, %s\n", obj, debugstr_guid( riid ) );

    if (!(apartment = calloc( 1, sizeof(*apartment) ))) return E_OUTOFMEMORY;

    apartment->IWbemUnsecuredApartment_iface.lpVtbl = &unsecured_apartment_vtbl;
    apartment->refs = 1;
    if (FAILED(hr = CoCreateFreeThreadedMarshaler(
                        (IUnknown *)&apartment->IWbemUnsecuredApartment_iface,
                        &apartment->marshal )))
    {
        free( apartment );
        return hr;
    }

    hr = IWbemUnsecuredApartment_QueryInterface( &apartment->IWbemUnsecuredApartment_iface,
                                                 riid, obj );
    IWbemUnsecuredApartment_Release( &apartment->IWbemUnsecuredApartment_iface );
    return hr;
}
