#!/usr/bin/env python3
import argparse
import socket
import struct
import sys


def parse_pcap(path, dst_port):
    data = open(path, 'rb').read()
    if len(data) < 24:
        raise RuntimeError('PCAP je prekratak')

    magic = data[:4]
    if magic == b'\xd4\xc3\xb2\xa1':
        endian = '<'
    elif magic == b'\xa1\xb2\xc3\xd4':
        endian = '>'
    elif magic == b'\x4d\x3c\xb2\xa1':
        endian = '<'
    elif magic == b'\xa1\xb2\x3c\x4d':
        endian = '>'
    else:
        raise RuntimeError('Podrzan je classic PCAP format, ne PCAPNG')

    network = struct.unpack(endian + 'I', data[20:24])[0]
    off = 24
    while off + 16 <= len(data):
        _, _, incl_len, _ = struct.unpack(endian + 'IIII', data[off:off+16])
        off += 16
        frame = data[off:off+incl_len]
        off += incl_len
        payload = extract_udp_payload(frame, network, dst_port)
        if payload is not None:
            return payload
    raise RuntimeError(f'Nije pronadjen UDP payload za destination port {dst_port}')


def extract_udp_payload(frame, network, dst_port):
    # DLT_EN10MB = 1
    if network == 1:
        if len(frame) < 14:
            return None
        ethertype = struct.unpack('!H', frame[12:14])[0]
        pos = 14
        if ethertype == 0x8100 and len(frame) >= 18:
            ethertype = struct.unpack('!H', frame[16:18])[0]
            pos = 18
        if ethertype != 0x0800:
            return None
        ip = frame[pos:]
    # DLT_RAW = 101
    elif network == 101:
        ip = frame
    # DLT_LINUX_SLL = 113
    elif network == 113:
        if len(frame) < 16 or struct.unpack('!H', frame[14:16])[0] != 0x0800:
            return None
        ip = frame[16:]
    # DLT_LINUX_SLL2 = 276
    elif network == 276:
        if len(frame) < 20 or struct.unpack('!H', frame[0:2])[0] != 0x0800:
            return None
        ip = frame[20:]
    else:
        raise RuntimeError(f'Nepodrzan PCAP link type: {network}')

    if len(ip) < 20 or (ip[0] >> 4) != 4:
        return None
    ihl = (ip[0] & 0x0F) * 4
    if len(ip) < ihl + 8 or ip[9] != 17:
        return None
    udp = ip[ihl:]
    _, dport, length, _ = struct.unpack('!HHHH', udp[:8])
    if dport != dst_port or length < 8:
        return None
    return udp[8:8+length-8]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcap')
    ap.add_argument('host')
    ap.add_argument('port', type=int)
    args = ap.parse_args()

    payload = parse_pcap(args.pcap, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(payload, (args.host, args.port))
    print(f'Replayed {len(payload)} UDP bytes to {args.host}:{args.port}')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
