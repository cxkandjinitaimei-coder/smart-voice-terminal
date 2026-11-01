"""Select a LAN discovery address from real interface netmasks, not the default route."""
import ipaddress
import os
import socket


def ipv4_interfaces():
    """Return (IPv4, netmask) pairs without third-party packages or network IO."""
    if os.name == 'nt':
        import ctypes as c

        class Address(c.Structure):
            pass
        Address._fields_ = [('Next', c.POINTER(Address)), ('IpAddress', c.c_char * 16),
                            ('IpMask', c.c_char * 16), ('Context', c.c_uint32)]

        class Adapter(c.Structure):
            pass
        # Only the prefix through IpAddressList is needed. GetAdaptersInfo fills
        # its own full-size byte buffer; linked pointers use the Windows layout.
        Adapter._fields_ = [('Next', c.POINTER(Adapter)), ('ComboIndex', c.c_uint32),
            ('AdapterName', c.c_char * 260), ('Description', c.c_char * 132),
            ('AddressLength', c.c_uint32), ('Address', c.c_ubyte * 8),
            ('Index', c.c_uint32), ('Type', c.c_uint32), ('DhcpEnabled', c.c_uint32),
            ('CurrentIpAddress', c.POINTER(Address)), ('IpAddressList', Address)]
        api = c.WinDLL('iphlpapi').GetAdaptersInfo
        api.argtypes = [c.c_void_p, c.POINTER(c.c_uint32)]
        api.restype = c.c_uint32
        size = c.c_uint32()
        if api(None, c.byref(size)) != 111:  # ERROR_BUFFER_OVERFLOW returns required bytes.
            return []
        buf = c.create_string_buffer(size.value)
        if api(buf, c.byref(size)) != 0:
            return []
        rows = []
        adapter = c.cast(buf, c.POINTER(Adapter))
        while adapter:
            address = c.pointer(adapter.contents.IpAddressList)
            while address:
                item = address.contents
                rows.append((item.IpAddress.decode('ascii'), item.IpMask.decode('ascii')))
                address = item.Next
            adapter = adapter.contents.Next
        return rows

    # Linux LAN and cloud hosts. An interface without IPv4 is simply skipped.
    import fcntl
    import struct
    rows = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for _, name in socket.if_nameindex():
            arg = struct.pack('256s', name.encode()[:15])
            try:
                ip = socket.inet_ntoa(fcntl.ioctl(sock, 0x8915, arg)[20:24])
                mask = socket.inet_ntoa(fcntl.ioctl(sock, 0x891b, arg)[20:24])
                rows.append((ip, mask))
            except OSError:
                continue
    return rows


def select_address(peer_ip, routed_ip, interfaces):
    """Prefer the most specific connected subnet; never advertise an unrelated NIC."""
    try:
        peer = ipaddress.IPv4Address(peer_ip)
    except ipaddress.AddressValueError:
        return ''
    if peer.is_loopback:
        return routed_ip if routed_ip.startswith('127.') else '127.0.0.1'
    matches = []
    for ip, mask in interfaces:
        try:
            local = ipaddress.IPv4Interface(ip + '/' + mask)
        except ValueError:
            continue
        if (local.ip.is_unspecified or local.ip.is_loopback or local.ip.is_link_local or
                local.network.prefixlen == 0 or peer not in local.network):
            continue
        matches.append((local.network.prefixlen, str(local.ip)))
    if not matches:
        return ''  # No reachable LAN interface is safer than poisoning the ESP route.
    prefix = max(p for p, _ in matches)
    candidates = sorted(ip for p, ip in matches if p == prefix)
    return routed_ip if routed_ip in candidates else candidates[0]


def local_ip_for(peer_ip):
    routed = ''
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect((peer_ip, 1))  # UDP route selection sends no packet.
            routed = sock.getsockname()[0]
        return select_address(peer_ip, routed, ipv4_interfaces())
    except (OSError, ImportError, ValueError):
        return ''


def send_reply(sock, payload, peer, local_ip):
    if local_ip.startswith('127.'):
        return sock.sendto(payload, peer)
    # Bind the source as well as advertising it, so another default-route NIC
    # cannot send the reply with an unrelated source address.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reply:
        reply.bind((local_ip, 0))
        return reply.sendto(payload, peer)
