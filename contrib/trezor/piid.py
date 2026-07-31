"""Compute WinRT parameterized interface IIDs.

A parameterized interface's IID is a UUIDv5 over the generic type signature,
under a fixed WinRT namespace GUID. Deriving it offline avoids having to build
an IRoMetaDataLocator just to call RoGetParameterizedTypeInstanceIID at runtime.

The advertisement handler below serves as a check: its IID is published in the
mingw headers, so reproducing it proves the derivation is right before it is
trusted for one that is not published anywhere we can read.
"""

import hashlib
import uuid

NS = uuid.UUID("11f47ad5-7b73-42c0-abae-878b1e16adee")
TEH = "9de1c534-6ae1-11e0-84e1-18a905bcc53f"  # ITypedEventHandler`2


def piid(sig):
    data = NS.bytes + sig.encode("utf-8")
    h = bytearray(hashlib.sha1(data).digest()[:16])
    h[6] = (h[6] & 0x0F) | 0x50  # version 5
    h[8] = (h[8] & 0x3F) | 0x80  # RFC 4122 variant
    return str(uuid.UUID(bytes=bytes(h)))


def rc(name, iid):
    return "rc(%s;{%s})" % (name, iid)


def teh(a, b):
    return "pinterface({%s};%s;%s)" % (TEH, a, b)


known = teh(
    rc("Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher",
       "a6ac336f-f3d3-4297-8d6c-c81ea6623f40"),
    rc("Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs",
       "27987ddf-e596-41be-8d43-9e6731d4a913"))
got = piid(known)
print("advertisement PIID computed :", got)
print("advertisement PIID expected : 90eb4eca-d465-5ea0-a61c-033c8c5ecef2")
print("MATCH:", got == "90eb4eca-d465-5ea0-a61c-033c8c5ecef2")
print()

pair = teh(
    rc("Windows.Devices.Enumeration.DeviceInformationCustomPairing",
       "85138c02-4ee6-4914-8370-107a39144c0e"),
    rc("Windows.Devices.Enumeration.DevicePairingRequestedEventArgs",
       "f717fc56-de6b-487f-8376-0180aca69963"))
print("PairingRequested PIID       :", piid(pair))
